/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * PICO OS 5.13.7 (Smartisan) cropped heap dump ("hprof_opt"), ported from the factory libart.
 *
 * A single-pass variant of hprof.cc that writes a compact, non-standard dump: a length-prefixed
 * name, the heap body without stack-trace serials, thread serials, JNI global ref ids, char/byte
 * array contents or class overhead bytes, then the LOAD_CLASS and STRING tables (with 2-byte
 * STRING lengths and no headers on LOAD_CLASS / HEAP_DUMP_SEGMENT / HEAP_DUMP_END records), and
 * finally the 8-byte offsets of those two tables. On the first out-of-memory error of a process
 * (unless setDumpFlag(false) was called), checkDumpHeap() writes such a dump into the existing
 * /data/hprofopt/.pre_hprofopt file, at most once per process and month.
 */

#include "hprof_opt.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/array_ref.h"
#include "base/file_utils.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "class_root.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/allocation_record.h"
#include "gc/heap-visit-objects-inl.h"
#include "gc/heap.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/space.h"
#include "gc_root.h"
#include "jdwp/jdwp.h"
#include "jdwp/jdwp_priv.h"
#include "mirror/class-inl.h"
#include "mirror/class.h"
#include "mirror/object-refvisitor-inl.h"
#include "runtime_globals.h"
#include "scoped_thread_state_change-inl.h"
#include "thread_list.h"

namespace art {

namespace hprof_opt {

static constexpr uint32_t kHprofTime = 0;

static constexpr size_t kMaxObjectsPerSegment = 128;
static constexpr size_t kMaxBytesPerSegment = 4096;

// The static field-name for the synthetic object generated to account for class static overhead.
static constexpr const char* kClassOverheadName = "$classOverhead";

enum HprofTag {
  HPROF_TAG_STRING = 0x01,
  HPROF_TAG_LOAD_CLASS = 0x02,
  HPROF_TAG_UNLOAD_CLASS = 0x03,
  HPROF_TAG_STACK_FRAME = 0x04,
  HPROF_TAG_STACK_TRACE = 0x05,
  HPROF_TAG_ALLOC_SITES = 0x06,
  HPROF_TAG_HEAP_SUMMARY = 0x07,
  HPROF_TAG_START_THREAD = 0x0A,
  HPROF_TAG_END_THREAD = 0x0B,
  HPROF_TAG_HEAP_DUMP = 0x0C,
  HPROF_TAG_HEAP_DUMP_SEGMENT = 0x1C,
  HPROF_TAG_HEAP_DUMP_END = 0x2C,
  HPROF_TAG_CPU_SAMPLES = 0x0D,
  HPROF_TAG_CONTROL_SETTINGS = 0x0E,
};

// Values for the first byte of HEAP_DUMP and HEAP_DUMP_SEGMENT records:
enum HprofHeapTag {
  // Traditional.
  HPROF_ROOT_UNKNOWN = 0xFF,
  HPROF_ROOT_JNI_GLOBAL = 0x01,
  HPROF_ROOT_JNI_LOCAL = 0x02,
  HPROF_ROOT_JAVA_FRAME = 0x03,
  HPROF_ROOT_NATIVE_STACK = 0x04,
  HPROF_ROOT_STICKY_CLASS = 0x05,
  HPROF_ROOT_THREAD_BLOCK = 0x06,
  HPROF_ROOT_MONITOR_USED = 0x07,
  HPROF_ROOT_THREAD_OBJECT = 0x08,
  HPROF_CLASS_DUMP = 0x20,
  HPROF_INSTANCE_DUMP = 0x21,
  HPROF_OBJECT_ARRAY_DUMP = 0x22,
  HPROF_PRIMITIVE_ARRAY_DUMP = 0x23,

  // Android.
  HPROF_HEAP_DUMP_INFO = 0xfe,
  HPROF_ROOT_INTERNED_STRING = 0x89,
  HPROF_ROOT_FINALIZING = 0x8a,  // Obsolete.
  HPROF_ROOT_DEBUGGER = 0x8b,
  HPROF_ROOT_REFERENCE_CLEANUP = 0x8c,  // Obsolete.
  HPROF_ROOT_VM_INTERNAL = 0x8d,
  HPROF_ROOT_JNI_MONITOR = 0x8e,
  HPROF_UNREACHABLE = 0x90,  // Obsolete.
  HPROF_PRIMITIVE_ARRAY_NODATA_DUMP = 0xc3,  // Obsolete.
};

enum HprofHeapId {
  HPROF_HEAP_DEFAULT = 0,
  HPROF_HEAP_ZYGOTE = 'Z',
  HPROF_HEAP_APP = 'A',
  HPROF_HEAP_IMAGE = 'I',
};

enum HprofBasicType {
  hprof_basic_object = 2,
  hprof_basic_boolean = 4,
  hprof_basic_char = 5,
  hprof_basic_float = 6,
  hprof_basic_double = 7,
  hprof_basic_byte = 8,
  hprof_basic_short = 9,
  hprof_basic_int = 10,
  hprof_basic_long = 11,
};

using HprofStringId = uint32_t;
using HprofClassObjectId = uint32_t;
using HprofClassSerialNumber = uint32_t;
using HprofStackTraceSerialNumber = uint32_t;
using HprofStackFrameId = uint32_t;
static constexpr HprofStackTraceSerialNumber kHprofNullStackTrace = 0;

class EndianOutput {
 public:
  EndianOutput() : length_(0), sum_length_(0), max_length_(0), started_(false), tag_(0) {}
  virtual ~EndianOutput() {}

  void StartNewRecord(uint8_t tag, uint32_t time) {
    if (length_ > 0) {
      EndRecord();
    }
    DCHECK_EQ(length_, 0U);
    AddU1(tag);
    tag_ = tag;
    switch (tag) {
      case HPROF_TAG_STRING:
        // Cropped STRING record: no time, 2-byte length.
        AddU2(0xdead);  // Length, replaced on flush.
        break;
      case HPROF_TAG_LOAD_CLASS:
      case HPROF_TAG_HEAP_DUMP_SEGMENT:
      case HPROF_TAG_HEAP_DUMP_END:
        // Cropped records without time and length.
        break;
      default:
        AddU4(time);
        AddU4(0xdeaddead);  // Length, replaced on flush.
        break;
    }
    started_ = true;
  }

  void EndRecord() {
    // Replace length in header.
    if (started_) {
      switch (tag_) {
        case HPROF_TAG_STRING:
          UpdateU2(sizeof(uint8_t), length_ - sizeof(uint8_t) - sizeof(uint16_t));
          break;
        case HPROF_TAG_LOAD_CLASS:
        case HPROF_TAG_HEAP_DUMP_SEGMENT:
        case HPROF_TAG_HEAP_DUMP_END:
          break;
        default:
          UpdateU4(sizeof(uint8_t) + sizeof(uint32_t),
                   length_ - sizeof(uint8_t) - 2 * sizeof(uint32_t));
          break;
      }
    }

    HandleEndRecord();

    sum_length_ += length_;
    max_length_ = std::max(max_length_, length_);
    length_ = 0;
    started_ = false;
  }

  void AddU1(uint8_t value) {
    AddU1List(&value, 1);
  }
  void AddU2(uint16_t value) {
    AddU2List(&value, 1);
  }
  void AddU4(uint32_t value) {
    AddU4List(&value, 1);
  }

  void AddU8(uint64_t value) {
    AddU8List(&value, 1);
  }

  void AddObjectId(const mirror::Object* value) {
    AddU4(PointerToLowMemUInt32(value));
  }

  void AddStackTraceSerialNumber(HprofStackTraceSerialNumber value) {
    AddU4(value);
  }

  // The ID for the synthetic object generated to account for class static overhead.
  void AddClassStaticsId(const mirror::Class* value) {
    AddU4(1 | PointerToLowMemUInt32(value));
  }

  void AddJniGlobalRefId(jobject value) {
    AddU4(PointerToLowMemUInt32(value));
  }

  void AddClassId(HprofClassObjectId value) {
    AddU4(value);
  }

  void AddStringId(HprofStringId value) {
    AddU4(value);
  }

  void AddU1List(const uint8_t* values, size_t count) {
    HandleU1List(values, count);
    length_ += count;
  }
  void AddU2List(const uint16_t* values, size_t count) {
    HandleU2List(values, count);
    length_ += count * sizeof(uint16_t);
  }
  void AddU4List(const uint32_t* values, size_t count) {
    HandleU4List(values, count);
    length_ += count * sizeof(uint32_t);
  }
  virtual void UpdateU2(size_t offset, uint32_t new_value ATTRIBUTE_UNUSED) {
    DCHECK_LE(offset, length_ - 2);
  }
  virtual void UpdateU4(size_t offset, uint32_t new_value ATTRIBUTE_UNUSED) {
    DCHECK_LE(offset, length_ - 4);
  }
  void AddU8List(const uint64_t* values, size_t count) {
    HandleU8List(values, count);
    length_ += count * sizeof(uint64_t);
  }

  void AddIdList(mirror::ObjectArray<mirror::Object>* values)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const int32_t length = values->GetLength();
    for (int32_t i = 0; i < length; ++i) {
      AddObjectId(values->GetWithoutChecks(i).Ptr());
    }
  }

  void AddUtf8String(const char* str) {
    // The terminating NUL character is NOT written.
    AddU1List((const uint8_t*)str, strlen(str));
  }

  size_t Length() const {
    return length_;
  }

  size_t SumLength() const {
    return sum_length_;
  }

  size_t MaxLength() const {
    return max_length_;
  }

 protected:
  virtual void HandleU1List(const uint8_t* values ATTRIBUTE_UNUSED,
                            size_t count ATTRIBUTE_UNUSED) {
  }
  virtual void HandleU1AsU2List(const uint8_t* values ATTRIBUTE_UNUSED,
                                size_t count ATTRIBUTE_UNUSED) {
  }
  virtual void HandleU2List(const uint16_t* values ATTRIBUTE_UNUSED,
                            size_t count ATTRIBUTE_UNUSED) {
  }
  virtual void HandleU4List(const uint32_t* values ATTRIBUTE_UNUSED,
                            size_t count ATTRIBUTE_UNUSED) {
  }
  virtual void HandleU8List(const uint64_t* values ATTRIBUTE_UNUSED,
                            size_t count ATTRIBUTE_UNUSED) {
  }
  virtual void HandleEndRecord() {
  }

  size_t length_;      // Current record size.
  size_t sum_length_;  // Size of all data.
  size_t max_length_;  // Maximum seen length.
  bool started_;       // Was StartRecord called?
  uint8_t tag_;        // Tag of the current record.
};

// This keeps things buffered until flushed.
class EndianOutputBuffered : public EndianOutput {
 public:
  explicit EndianOutputBuffered(size_t reserve_size) {
    buffer_.reserve(reserve_size);
  }
  virtual ~EndianOutputBuffered() {}

  void UpdateU2(size_t offset, uint32_t new_value) override {
    DCHECK_LE(offset, length_ - 2);
    buffer_[offset + 0] = static_cast<uint8_t>((new_value >> 8) & 0xFF);
    buffer_[offset + 1] = static_cast<uint8_t>((new_value >> 0) & 0xFF);
  }

  void UpdateU4(size_t offset, uint32_t new_value) override {
    DCHECK_LE(offset, length_ - 4);
    buffer_[offset + 0] = static_cast<uint8_t>((new_value >> 24) & 0xFF);
    buffer_[offset + 1] = static_cast<uint8_t>((new_value >> 16) & 0xFF);
    buffer_[offset + 2] = static_cast<uint8_t>((new_value >> 8)  & 0xFF);
    buffer_[offset + 3] = static_cast<uint8_t>((new_value >> 0)  & 0xFF);
  }

 protected:
  void HandleU1List(const uint8_t* values, size_t count) override {
    DCHECK_EQ(length_, buffer_.size());
    buffer_.insert(buffer_.end(), values, values + count);
  }

  void HandleU1AsU2List(const uint8_t* values, size_t count) override {
    DCHECK_EQ(length_, buffer_.size());
    // All 8-bits are grouped in 2 to make 16-bit block like Java Char
    if (count & 1) {
      buffer_.push_back(0);
    }
    for (size_t i = 0; i < count; ++i) {
      uint8_t value = *values;
      buffer_.push_back(value);
      values++;
    }
  }

  void HandleU2List(const uint16_t* values, size_t count) override {
    DCHECK_EQ(length_, buffer_.size());
    for (size_t i = 0; i < count; ++i) {
      uint16_t value = *values;
      buffer_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 0) & 0xFF));
      values++;
    }
  }

  void HandleU4List(const uint32_t* values, size_t count) override {
    DCHECK_EQ(length_, buffer_.size());
    for (size_t i = 0; i < count; ++i) {
      uint32_t value = *values;
      buffer_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 8)  & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 0)  & 0xFF));
      values++;
    }
  }

  void HandleU8List(const uint64_t* values, size_t count) override {
    DCHECK_EQ(length_, buffer_.size());
    for (size_t i = 0; i < count; ++i) {
      uint64_t value = *values;
      buffer_.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 8)  & 0xFF));
      buffer_.push_back(static_cast<uint8_t>((value >> 0)  & 0xFF));
      values++;
    }
  }

  void HandleEndRecord() override {
    DCHECK_EQ(buffer_.size(), length_);
    if (kIsDebugBuild && started_) {
      uint32_t stored_length =
          static_cast<uint32_t>(buffer_[5]) << 24 |
          static_cast<uint32_t>(buffer_[6]) << 16 |
          static_cast<uint32_t>(buffer_[7]) << 8 |
          static_cast<uint32_t>(buffer_[8]);
      DCHECK_EQ(stored_length, length_ - sizeof(uint8_t) - 2 * sizeof(uint32_t));
    }
    HandleFlush(buffer_.data(), length_);
    buffer_.clear();
  }

  virtual void HandleFlush(const uint8_t* buffer ATTRIBUTE_UNUSED, size_t length ATTRIBUTE_UNUSED) {
  }

  std::vector<uint8_t> buffer_;
};

class CropFileEndianOutput : public EndianOutputBuffered {
 public:
  explicit CropFileEndianOutput(File* fp)
      : EndianOutputBuffered(0), fp_(fp), errors_(false) {
    DCHECK(fp != nullptr);
  }
  ~CropFileEndianOutput() {
  }

  bool Errors() {
    return errors_;
  }

 protected:
  void HandleEndRecord() override {
    HandleFlush(buffer_.data(), length_);
    buffer_.clear();
  }

  void HandleFlush(const uint8_t* buffer, size_t length) override {
    if (!errors_) {
      errors_ = !fp_->WriteFully(buffer, length);
    }
  }

 private:
  File* fp_;
  bool errors_;
};

#define __ output_->

class HprofOpt : public SingleRootVisitor {
 public:
  HprofOpt(const char* output_filename, int fd, const char* name)
      : filename_(output_filename),
        fd_(fd),
        name_(name) {
    LOG(INFO) << "HprofOpt: heap dump \"" << filename_ << "\" starting..." << ", fd: " << fd_
              << ", prefix: " << name_;
  }

  void Dump()
    REQUIRES(Locks::mutator_lock_)
    REQUIRES(!Locks::heap_bitmap_lock_, !Locks::alloc_tracker_lock_) {
    {
      MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
      if (Runtime::Current()->GetHeap()->IsAllocTrackingEnabled()) {
        PopulateAllocationTrackingTraces();
      }
    }

    // Single pass: the cropped dump is written directly to the file.
    visited_objects_.clear();
    DumpToFile();
  }

 private:
  void DumpHeapObject(mirror::Object* obj)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpHeapClass(mirror::Class* klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpHeapArray(mirror::Array* obj, mirror::Class* klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpFakeObjectArray(mirror::Object* obj, const std::set<mirror::Object*>& elements)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpHeapInstanceObject(mirror::Object* obj,
                              mirror::Class* klass,
                              const std::set<mirror::Object*>& fake_roots)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool AddRuntimeInternalObjectsField(mirror::Class* klass) REQUIRES_SHARED(Locks::mutator_lock_);

  void ProcessHeap()
      REQUIRES(Locks::mutator_lock_) {
    // Reset current heap and object count.
    current_heap_ = HPROF_HEAP_DEFAULT;
    objects_in_segment_ = 0;

    // U4: length of the name, U1*: name (NOT null terminated).
    if (name_ != nullptr) {
      __ AddU4(static_cast<uint32_t>(strlen(name_)));
      __ AddUtf8String(name_);
    } else {
      __ AddU4(0);
    }

    ProcessBody();
    ProcessHeader();
  }

  void ProcessBody() REQUIRES(Locks::mutator_lock_) {
    Runtime* const runtime = Runtime::Current();
    // Walk the roots and the heap.
    output_->StartNewRecord(HPROF_TAG_HEAP_DUMP_SEGMENT, kHprofTime);

    simple_roots_.clear();
    runtime->VisitRoots(this);
    runtime->VisitImageRoots(this);
    auto dump_object = [this](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK(obj != nullptr);
      DumpHeapObject(obj);
    };
    runtime->GetHeap()->VisitObjectsPaused(dump_object);
    output_->StartNewRecord(HPROF_TAG_HEAP_DUMP_END, kHprofTime);
    output_->EndRecord();
  }

  void ProcessHeader() REQUIRES(Locks::mutator_lock_) {
    // The class and string tables follow the body; their offsets are appended at the end.
    header_position_ = output_->SumLength();
    WriteClassTable();
    string_position_ = output_->SumLength() + output_->Length();
    WriteStringTable();
    output_->EndRecord();
    __ AddU8(header_position_);
    __ AddU8(string_position_);
    LOG(INFO) << "HprofOpt header_position_: " << header_position_
              << ", string_position_: " << string_position_;
    output_->EndRecord();
  }

  void WriteClassTable() REQUIRES_SHARED(Locks::mutator_lock_) {
    for (const auto& p : classes_) {
      mirror::Class* c = p.first;
      CHECK(c != nullptr);
      output_->StartNewRecord(HPROF_TAG_LOAD_CLASS, kHprofTime);
      // Cropped LOAD CLASS format:
      // ID: class object ID. We use the address of the class object structure as its ID.
      // ID: class name string ID
      __ AddObjectId(c);
      __ AddStringId(LookupClassNameId(c));
    }
  }

  void WriteStringTable() {
    for (const auto& p : strings_) {
      const std::string& string = p.first;
      const HprofStringId id = p.second;

      output_->StartNewRecord(HPROF_TAG_STRING, kHprofTime);

      // STRING format:
      // ID:  ID for this string
      // U1*: UTF8 characters for string (NOT null terminated)
      //      (the record format encodes the length)
      __ AddU4(id);
      __ AddUtf8String(string.c_str());
    }
  }

  void StartNewHeapDumpSegment() {
    // This flushes the old segment and starts a new one.
    output_->StartNewRecord(HPROF_TAG_HEAP_DUMP_SEGMENT, kHprofTime);
    objects_in_segment_ = 0;
    // Starting a new HEAP_DUMP resets the heap to default.
    current_heap_ = HPROF_HEAP_DEFAULT;
  }

  void CheckHeapSegmentConstraints() {
    if (objects_in_segment_ >= kMaxObjectsPerSegment || output_->Length() >= kMaxBytesPerSegment) {
      StartNewHeapDumpSegment();
    }
  }

  void VisitRoot(mirror::Object* obj, const RootInfo& root_info)
      override REQUIRES_SHARED(Locks::mutator_lock_);
  void MarkRootObject(const mirror::Object* obj, jobject jni_obj, HprofHeapTag heap_tag,
                      uint32_t thread_serial);

  HprofClassObjectId LookupClassId(mirror::Class* c) REQUIRES_SHARED(Locks::mutator_lock_) {
    if (c != nullptr) {
      auto it = classes_.find(c);
      if (it == classes_.end()) {
        // first time to see this class
        HprofClassSerialNumber sn = next_class_serial_number_++;
        classes_.Put(c, sn);
        // Make sure that we've assigned a string ID for this class' name
        LookupClassNameId(c);
      }
    }
    return PointerToLowMemUInt32(c);
  }

  HprofStringId LookupStringId(mirror::String* string) REQUIRES_SHARED(Locks::mutator_lock_) {
    return LookupStringId(string->ToModifiedUtf8());
  }

  HprofStringId LookupStringId(const char* string) {
    return LookupStringId(std::string(string));
  }

  HprofStringId LookupStringId(const std::string& string) {
    auto it = strings_.find(string);
    if (it != strings_.end()) {
      return it->second;
    }
    HprofStringId id = next_string_id_++;
    strings_.Put(string, id);
    return id;
  }

  HprofStringId LookupClassNameId(mirror::Class* c) REQUIRES_SHARED(Locks::mutator_lock_) {
    return LookupStringId(c->PrettyDescriptor());
  }

  bool DumpToFile() REQUIRES(Locks::mutator_lock_) {
    // Where exactly are we writing to?
    int out_fd;
    bool locked = false;
    if (fd_ >= 0) {
      out_fd = DupCloexec(fd_);
      if (out_fd < 0) {
        LOG(ERROR) << "Couldn't dump heap; dup( " << fd_ << " failed: " << strerror(errno);
        return false;
      }
    } else {
      out_fd = open(filename_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0777);
      if (out_fd < 0) {
        LOG(ERROR) << "Couldn't dump heap; open(" << filename_ << ") failed:" << strerror(errno);
        return false;
      }
      if (flock(out_fd, LOCK_EX | LOCK_NB) != 0) {
        LOG(ERROR) << "Couldn't dump heap; flock(" << filename_ << ") failed:" << strerror(errno);
        return false;
      }
      locked = true;
    }

    std::unique_ptr<File> file(new File(out_fd, filename_, true));
    bool okay;
    size_t overall_size;
    {
      CropFileEndianOutput file_output(file.get());
      output_ = &file_output;
      ProcessHeap();
      okay = !file_output.Errors();
      overall_size = file_output.SumLength();
      output_ = nullptr;
    }

    if (locked && flock(out_fd, LOCK_UN | LOCK_NB) != 0) {
      LOG(ERROR) << "unlock failed: " << strerror(errno);
    }

    if (okay) {
      okay = file->FlushCloseOrErase() == 0;
    } else {
      file->Erase();
    }
    if (!okay) {
      std::string msg(android::base::StringPrintf("Couldn't dump heap; writing \"%s\" failed: %s",
                                                  filename_.c_str(),
                                                  strerror(errno)));
      ThrowRuntimeException("%s", msg.c_str());
      LOG(ERROR) << msg;
    } else {
      const uint64_t duration = NanoTime() - start_ns_;
      LOG(INFO) << "HprofOpt: heap dump completed (" << PrettySize(RoundUp(overall_size, KB))
                << ") in " << PrettyDuration(duration)
                << " objects " << total_objects_
                << " objects with stack traces " << total_objects_with_stack_trace_;
    }

    return okay;
  }

  void PopulateAllocationTrackingTraces()
      REQUIRES(Locks::mutator_lock_, Locks::alloc_tracker_lock_) {
    gc::AllocRecordObjectMap* records = Runtime::Current()->GetHeap()->GetAllocationRecords();
    CHECK(records != nullptr);
    HprofStackTraceSerialNumber next_trace_sn = kHprofNullStackTrace + 1;
    HprofStackFrameId next_frame_id = 0;
    size_t count = 0;

    for (auto it = records->Begin(), end = records->End(); it != end; ++it) {
      const mirror::Object* obj = it->first.Read();
      if (obj == nullptr) {
        continue;
      }
      ++count;
      const gc::AllocRecordStackTrace* trace = it->second.GetStackTrace();

      // Copy the pair into a real hash map to speed up look up.
      auto records_result = allocation_records_.emplace(obj, trace);
      // The insertion should always succeed, i.e. no duplicate object pointers in "records"
      CHECK(records_result.second);

      // Generate serial numbers for traces, and IDs for frames.
      auto traces_result = traces_.find(trace);
      if (traces_result == traces_.end()) {
        traces_.emplace(trace, next_trace_sn++);
        // only check frames if the trace is newly discovered
        for (size_t i = 0, depth = trace->GetDepth(); i < depth; ++i) {
          const gc::AllocRecordStackTraceElement* frame = &trace->GetStackElement(i);
          auto frames_result = frames_.find(frame);
          if (frames_result == frames_.end()) {
            frames_.emplace(frame, next_frame_id++);
          }
        }
      }
    }
    CHECK_EQ(traces_.size(), next_trace_sn - kHprofNullStackTrace - 1);
    CHECK_EQ(frames_.size(), next_frame_id);
    total_objects_with_stack_trace_ = count;
  }

  // "filename_" must be valid, though if "fd" >= 0 it will only be used for debug messages.
  std::string filename_;
  int fd_;
  // Name written at the start of the dump.
  const char* name_;
  // Offsets of the class and string tables, appended at the end of the dump.
  size_t header_position_ = 0;
  size_t string_position_ = 0;

  uint64_t start_ns_ = NanoTime();

  EndianOutput* output_ = nullptr;

  HprofHeapId current_heap_ = HPROF_HEAP_DEFAULT;  // Which heap we're currently dumping.
  size_t objects_in_segment_ = 0;

  size_t total_objects_ = 0u;
  size_t total_objects_with_stack_trace_ = 0u;

  HprofStringId next_string_id_ = 0x400000;
  SafeMap<std::string, HprofStringId> strings_;
  HprofClassSerialNumber next_class_serial_number_ = 1;
  SafeMap<mirror::Class*, HprofClassSerialNumber> classes_;

  std::unordered_map<const gc::AllocRecordStackTrace*, HprofStackTraceSerialNumber,
                     gc::HashAllocRecordTypesPtr<gc::AllocRecordStackTrace>,
                     gc::EqAllocRecordTypesPtr<gc::AllocRecordStackTrace>> traces_;
  std::unordered_map<const gc::AllocRecordStackTraceElement*, HprofStackFrameId,
                     gc::HashAllocRecordTypesPtr<gc::AllocRecordStackTraceElement>,
                     gc::EqAllocRecordTypesPtr<gc::AllocRecordStackTraceElement>> frames_;
  std::unordered_map<const mirror::Object*, const gc::AllocRecordStackTrace*> allocation_records_;

  // Set used to keep track of what simple root records we have already
  // emitted, to avoid emitting duplicate entries. The simple root records are
  // those that contain no other information than the root type and the object
  // id. A pair of root type and object id is packed into a uint64_t, with
  // the root type in the upper 32 bits and the object id in the lower 32
  // bits.
  std::unordered_set<uint64_t> simple_roots_;

  // To make sure we don't dump the same object multiple times. b/34967844
  std::unordered_set<mirror::Object*> visited_objects_;

  friend class GcRootVisitor;
  DISALLOW_COPY_AND_ASSIGN(HprofOpt);
};

static HprofBasicType SignatureToBasicTypeAndSize(const char* sig, size_t* size_out) {
  char c = sig[0];
  HprofBasicType ret;
  size_t size;

  switch (c) {
    case '[':
    case 'L':
      ret = hprof_basic_object;
      size = 4;
      break;
    case 'Z':
      ret = hprof_basic_boolean;
      size = 1;
      break;
    case 'C':
      ret = hprof_basic_char;
      size = 2;
      break;
    case 'F':
      ret = hprof_basic_float;
      size = 4;
      break;
    case 'D':
      ret = hprof_basic_double;
      size = 8;
      break;
    case 'B':
      ret = hprof_basic_byte;
      size = 1;
      break;
    case 'S':
      ret = hprof_basic_short;
      size = 2;
      break;
    case 'I':
      ret = hprof_basic_int;
      size = 4;
      break;
    case 'J':
      ret = hprof_basic_long;
      size = 8;
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }

  if (size_out != nullptr) {
    *size_out = size;
  }

  return ret;
}

// Always called when marking objects, but only does
// something when ctx->gc_scan_state_ is non-zero, which is usually
// only true when marking the root set or unreachable
// objects.  Used to add rootset references to obj.
void HprofOpt::MarkRootObject(const mirror::Object* obj,
                              jobject jni_obj ATTRIBUTE_UNUSED,
                              HprofHeapTag heap_tag,
                              uint32_t thread_serial ATTRIBUTE_UNUSED) {
  if (heap_tag == 0) {
    return;
  }

  CheckHeapSegmentConstraints();

  switch (heap_tag) {
    // ID: object ID
    case HPROF_ROOT_UNKNOWN:
    case HPROF_ROOT_STICKY_CLASS:
    case HPROF_ROOT_MONITOR_USED:
    case HPROF_ROOT_INTERNED_STRING:
    case HPROF_ROOT_DEBUGGER:
    case HPROF_ROOT_VM_INTERNAL: {
      uint64_t key = (static_cast<uint64_t>(heap_tag) << 32) | PointerToLowMemUInt32(obj);
      if (simple_roots_.insert(key).second) {
        __ AddU1(heap_tag);
        __ AddObjectId(obj);
      }
      break;
    }

      // ID: object ID
      // ID: JNI global ref ID (cropped: always 0)
    case HPROF_ROOT_JNI_GLOBAL:
      __ AddU1(heap_tag);
      __ AddObjectId(obj);
      __ AddJniGlobalRefId(nullptr);
      break;

      // ID: object ID (cropped: no thread serial number / frame number)
    case HPROF_ROOT_JNI_LOCAL:
    case HPROF_ROOT_JNI_MONITOR:
    case HPROF_ROOT_JAVA_FRAME:
    case HPROF_ROOT_NATIVE_STACK:
    case HPROF_ROOT_THREAD_BLOCK:
    case HPROF_ROOT_THREAD_OBJECT:
      __ AddU1(heap_tag);
      __ AddObjectId(obj);
      break;

    case HPROF_CLASS_DUMP:
    case HPROF_INSTANCE_DUMP:
    case HPROF_OBJECT_ARRAY_DUMP:
    case HPROF_PRIMITIVE_ARRAY_DUMP:
    case HPROF_HEAP_DUMP_INFO:
    case HPROF_PRIMITIVE_ARRAY_NODATA_DUMP:
      // Ignored.
      break;

    case HPROF_ROOT_FINALIZING:
    case HPROF_ROOT_REFERENCE_CLEANUP:
    case HPROF_UNREACHABLE:
      LOG(FATAL) << "obsolete tag " << static_cast<int>(heap_tag);
      UNREACHABLE();
  }

  ++objects_in_segment_;
}

bool HprofOpt::AddRuntimeInternalObjectsField(mirror::Class* klass) {
  if (klass->IsDexCacheClass()) {
    return true;
  }
  // IsClassLoaderClass is true for subclasses of classloader but we only want to add the fake
  // field to the java.lang.ClassLoader class.
  if (klass->IsClassLoaderClass() && klass->GetSuperClass()->IsObjectClass()) {
    return true;
  }
  return false;
}

void HprofOpt::DumpHeapObject(mirror::Object* obj) {
  // Ignore classes that are retired.
  if (obj->IsClass() && obj->AsClass()->IsRetired()) {
    return;
  }
  DCHECK(visited_objects_.insert(obj).second)
      << "Already visited " << obj << "(" << obj->PrettyTypeOf() << ")";

  ++total_objects_;

  class RootCollector {
   public:
    RootCollector() {}

    void operator()(mirror::Object*, MemberOffset, bool) const {}

    // Note that these don't have read barriers. Its OK however since the GC is guaranteed to not be
    // running during the hprof dumping process.
    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
        REQUIRES_SHARED(Locks::mutator_lock_) {
      if (!root->IsNull()) {
        VisitRoot(root);
      }
    }

    void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
        REQUIRES_SHARED(Locks::mutator_lock_) {
      roots_.insert(root->AsMirrorPtr());
    }

    const std::set<mirror::Object*>& GetRoots() const {
      return roots_;
    }

   private:
    // These roots are actually live from the object. Avoid marking them as roots in hprof to make
    // it easier to debug class unloading.
    mutable std::set<mirror::Object*> roots_;
  };

  RootCollector visitor;
  // Collect all native roots.
  if (!obj->IsClass()) {
    obj->VisitReferences(visitor, VoidFunctor());
  }

  gc::Heap* const heap = Runtime::Current()->GetHeap();
  const gc::space::ContinuousSpace* const space = heap->FindContinuousSpaceFromObject(obj, true);
  HprofHeapId heap_type = HPROF_HEAP_APP;
  if (space != nullptr) {
    if (space->IsZygoteSpace()) {
      heap_type = HPROF_HEAP_ZYGOTE;
      VisitRoot(obj, RootInfo(kRootVMInternal));
    } else if (space->IsImageSpace() && heap->ObjectIsInBootImageSpace(obj)) {
      // Only count objects in the boot image as HPROF_HEAP_IMAGE, this leaves app image objects as
      // HPROF_HEAP_APP. b/35762934
      heap_type = HPROF_HEAP_IMAGE;
      VisitRoot(obj, RootInfo(kRootVMInternal));
    }
  } else {
    const auto* los = heap->GetLargeObjectsSpace();
    if (los->Contains(obj) && los->IsZygoteLargeObject(Thread::Current(), obj)) {
      heap_type = HPROF_HEAP_ZYGOTE;
      VisitRoot(obj, RootInfo(kRootVMInternal));
    }
  }
  CheckHeapSegmentConstraints();

  if (heap_type != current_heap_) {
    HprofStringId nameId;

    // This object is in a different heap than the current one.
    // Emit a HEAP_DUMP_INFO tag to change heaps.
    __ AddU1(HPROF_HEAP_DUMP_INFO);
    __ AddU4(static_cast<uint32_t>(heap_type));   // uint32_t: heap type
    switch (heap_type) {
    case HPROF_HEAP_APP:
      nameId = LookupStringId("app");
      break;
    case HPROF_HEAP_ZYGOTE:
      nameId = LookupStringId("zygote");
      break;
    case HPROF_HEAP_IMAGE:
      nameId = LookupStringId("image");
      break;
    default:
      // Internal error
      LOG(ERROR) << "Unexpected desiredHeap";
      nameId = LookupStringId("<ILLEGAL>");
      break;
    }
    __ AddStringId(nameId);
    current_heap_ = heap_type;
  }

  mirror::Class* c = obj->GetClass();
  if (c == nullptr) {
    // This object will bother HprofReader, because it has a null
    // class, so just don't dump it. It could be
    // gDvm.unlinkedJavaLangClass or it could be an object just
    // allocated which hasn't been initialized yet.
  } else {
    if (obj->IsClass()) {
      DumpHeapClass(obj->AsClass().Ptr());
    } else if (c->IsArrayClass()) {
      DumpHeapArray(obj->AsArray().Ptr(), c);
    } else {
      DumpHeapInstanceObject(obj, c, visitor.GetRoots());
    }
  }

  ++objects_in_segment_;
}

void HprofOpt::DumpHeapClass(mirror::Class* klass) {
  if (!klass->IsResolved()) {
    // Class is allocated but not yet resolved: we cannot access its fields or super class.
    return;
  }

  // Note: We will emit instance fields of Class as synthetic static fields with a prefix of
  //       "$class$" so the class fields are visible in hprof dumps. For tools to account for that
  //       correctly, we'll emit an instance size of zero for java.lang.Class, and also emit the
  //       instance fields of java.lang.Object.
  //
  //       For other overhead (currently only the embedded vtable), we will generate a synthetic
  //       byte array (or field[s] in case the overhead size is of reference size or less).

  const size_t num_static_fields = klass->NumStaticFields();

  // Total class size:
  //   * class instance fields (including Object instance fields)
  //   * vtable
  //   * class static fields
  const size_t total_class_size = klass->GetClassSize();

  // Base class size (common parts of all Class instances):
  //   * class instance fields (including Object instance fields)
  constexpr size_t base_class_size = sizeof(mirror::Class);
  CHECK_LE(base_class_size, total_class_size);

  // Difference of Total and Base:
  //   * vtable
  //   * class static fields
  const size_t base_overhead_size = total_class_size - base_class_size;

  // Tools (ahat/Studio) will count the static fields and account for them in the class size. We
  // must thus subtract them from base_overhead_size or they will be double-counted.
  size_t class_static_fields_size = 0;
  for (ArtField& class_static_field : klass->GetSFields()) {
    size_t size = 0;
    SignatureToBasicTypeAndSize(class_static_field.GetTypeDescriptor(), &size);
    class_static_fields_size += size;
  }

  CHECK_GE(base_overhead_size, class_static_fields_size);
  // Now we have:
  //   * vtable
  const size_t base_no_statics_overhead_size = base_overhead_size - class_static_fields_size;

  // We may decide to display native overhead (the actual IMT, ArtFields and ArtMethods) in the
  // future.
  const size_t java_heap_overhead_size = base_no_statics_overhead_size;

  // For overhead greater 4, we'll allocate a synthetic array.
  if (java_heap_overhead_size > 4) {
    // Create a byte array to reflect the allocation of the
    // StaticField array at the end of this class.
    // Cropped: the array contents are not written.
    __ AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
    __ AddClassStaticsId(klass);
    __ AddU4(java_heap_overhead_size - 4);
    __ AddU1(hprof_basic_byte);
  }
  const size_t java_heap_overhead_field_count = java_heap_overhead_size > 0
                                                    ? (java_heap_overhead_size == 3 ? 2u : 1u)
                                                    : 0;

  // Cropped CLASS DUMP: no stack trace serial number, no reserved ids, 2-byte instance size.
  __ AddU1(HPROF_CLASS_DUMP);
  __ AddClassId(LookupClassId(klass));
  __ AddClassId(LookupClassId(klass->GetSuperClass().Ptr()));
  __ AddObjectId(klass->GetClassLoader().Ptr());
  __ AddObjectId(nullptr);    // no signer
  __ AddObjectId(nullptr);    // no prot domain
  // Instance size.
  if (klass->IsClassClass()) {
    // As mentioned above, we will emit instance fields as synthetic static fields. So the
    // base object is "empty."
    __ AddU2(0);
  } else if (klass->IsStringClass()) {
    // Strings are variable length with character data at the end like arrays.
    // This outputs the size of an empty string.
    __ AddU2(sizeof(mirror::String));
  } else if (klass->IsArrayClass() || klass->IsPrimitive()) {
    __ AddU2(0);
  } else {
    __ AddU2(klass->GetObjectSize());  // instance size
  }

  __ AddU2(0);  // empty const pool

  // Static fields
  //
  // Note: we report Class' and Object's instance fields here, too. This is for visibility reasons.
  //       (b/38167721)
  mirror::Class* class_class = klass->GetClass();

  DCHECK(class_class->GetSuperClass()->IsObjectClass());
  const size_t static_fields_reported = class_class->NumInstanceFields()
                                        + class_class->GetSuperClass()->NumInstanceFields()
                                        + java_heap_overhead_field_count
                                        + num_static_fields;
  __ AddU2(dchecked_integral_cast<uint16_t>(static_fields_reported));

  if (java_heap_overhead_size != 0) {
    __ AddStringId(LookupStringId(kClassOverheadName));
    size_t overhead_fields = 0;
    if (java_heap_overhead_size > 4) {
      __ AddU1(hprof_basic_object);
      __ AddClassStaticsId(klass);
      ++overhead_fields;
    } else {
      switch (java_heap_overhead_size) {
        case 4: {
          __ AddU1(hprof_basic_int);
          __ AddU4(0);
          ++overhead_fields;
          break;
        }

        case 2: {
          __ AddU1(hprof_basic_short);
          __ AddU2(0);
          ++overhead_fields;
          break;
        }

        case 3: {
          __ AddU1(hprof_basic_short);
          __ AddU2(0);
          __ AddStringId(LookupStringId(std::string(kClassOverheadName) + "2"));
          ++overhead_fields;
        }
        FALLTHROUGH_INTENDED;

        case 1: {
          __ AddU1(hprof_basic_byte);
          __ AddU1(0);
          ++overhead_fields;
          break;
        }
      }
    }
    DCHECK_EQ(java_heap_overhead_field_count, overhead_fields);
  }

  // Helper lambda to emit the given static field. The second argument name_fn will be called to
  // generate the name to emit. This can be used to emit something else than the field's actual
  // name.
  auto static_field_writer = [&](ArtField& field, auto name_fn)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    __ AddStringId(LookupStringId(name_fn(field)));

    size_t size;
    HprofBasicType t = SignatureToBasicTypeAndSize(field.GetTypeDescriptor(), &size);
    __ AddU1(t);
    switch (t) {
      case hprof_basic_byte:
        __ AddU1(field.GetByte(klass));
        return;
      case hprof_basic_boolean:
        __ AddU1(field.GetBoolean(klass));
        return;
      case hprof_basic_char:
        __ AddU2(field.GetChar(klass));
        return;
      case hprof_basic_short:
        __ AddU2(field.GetShort(klass));
        return;
      case hprof_basic_float:
      case hprof_basic_int:
      case hprof_basic_object:
        __ AddU4(field.Get32(klass));
        return;
      case hprof_basic_double:
      case hprof_basic_long:
        __ AddU8(field.Get64(klass));
        return;
    }
    LOG(FATAL) << "Unexpected size " << size;
    UNREACHABLE();
  };

  {
    auto class_instance_field_name_fn = [](ArtField& field) REQUIRES_SHARED(Locks::mutator_lock_) {
      return std::string("$class$") + field.GetName();
    };
    for (ArtField& class_instance_field : class_class->GetIFields()) {
      static_field_writer(class_instance_field, class_instance_field_name_fn);
    }
    for (ArtField& object_instance_field : class_class->GetSuperClass()->GetIFields()) {
      static_field_writer(object_instance_field, class_instance_field_name_fn);
    }
  }

  {
    auto class_static_field_name_fn = [](ArtField& field) REQUIRES_SHARED(Locks::mutator_lock_) {
      return field.GetName();
    };
    for (ArtField& class_static_field : klass->GetSFields()) {
      static_field_writer(class_static_field, class_static_field_name_fn);
    }
  }

  // Instance fields for this class (no superclass fields)
  int iFieldCount = klass->NumInstanceFields();
  // add_internal_runtime_objects is only for classes that may retain objects live through means
  // other than fields. It is never the case for strings.
  const bool add_internal_runtime_objects = AddRuntimeInternalObjectsField(klass);
  if (klass->IsStringClass() || add_internal_runtime_objects) {
    __ AddU2((uint16_t)iFieldCount + 1);
  } else {
    __ AddU2((uint16_t)iFieldCount);
  }
  for (int i = 0; i < iFieldCount; ++i) {
    ArtField* f = klass->GetInstanceField(i);
    __ AddStringId(LookupStringId(f->GetName()));
    HprofBasicType t = SignatureToBasicTypeAndSize(f->GetTypeDescriptor(), nullptr);
    __ AddU1(t);
  }
  // Add native value character array for strings / byte array for compressed strings.
  if (klass->IsStringClass()) {
    __ AddStringId(LookupStringId("value"));
    __ AddU1(hprof_basic_object);
  } else if (add_internal_runtime_objects) {
    __ AddStringId(LookupStringId("runtimeInternalObjects"));
    __ AddU1(hprof_basic_object);
  }
}

void HprofOpt::DumpFakeObjectArray(mirror::Object* obj, const std::set<mirror::Object*>& elements) {
  __ AddU1(HPROF_OBJECT_ARRAY_DUMP);
  __ AddObjectId(obj);
  __ AddU4(elements.size());
  __ AddClassId(LookupClassId(GetClassRoot<mirror::ObjectArray<mirror::Object>>().Ptr()));
  for (mirror::Object* e : elements) {
    __ AddObjectId(e);
  }
}

void HprofOpt::DumpHeapArray(mirror::Array* obj, mirror::Class* klass) {
  uint32_t length = obj->GetLength();

  if (obj->IsObjectArray()) {
    // obj is an object array.
    __ AddU1(HPROF_OBJECT_ARRAY_DUMP);

    __ AddObjectId(obj);
    __ AddU4(length);
    __ AddClassId(LookupClassId(klass));

    // Dump the elements, which are always objects or null.
    __ AddIdList(obj->AsObjectArray<mirror::Object>().Ptr());
  } else {
    size_t size;
    HprofBasicType t = SignatureToBasicTypeAndSize(
        Primitive::Descriptor(klass->GetComponentType()->GetPrimitiveType()), &size);

    // obj is a primitive array.
    __ AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);

    __ AddObjectId(obj);
    __ AddU4(length);
    __ AddU1(t);

    // Cropped: char and byte array contents are not written.
    if (t == hprof_basic_char || t == hprof_basic_byte) {
      return;
    }

    // Dump the raw, packed element values.
    if (size == 1) {
      __ AddU1List(reinterpret_cast<const uint8_t*>(obj->GetRawData(sizeof(uint8_t), 0)), length);
    } else if (size == 2) {
      __ AddU2List(reinterpret_cast<const uint16_t*>(obj->GetRawData(sizeof(uint16_t), 0)), length);
    } else if (size == 4) {
      __ AddU4List(reinterpret_cast<const uint32_t*>(obj->GetRawData(sizeof(uint32_t), 0)), length);
    } else if (size == 8) {
      __ AddU8List(reinterpret_cast<const uint64_t*>(obj->GetRawData(sizeof(uint64_t), 0)), length);
    }
  }
}

void HprofOpt::DumpHeapInstanceObject(mirror::Object* obj,
                                   mirror::Class* klass,
                                   const std::set<mirror::Object*>& fake_roots) {
  // obj is an instance object.
  __ AddU1(HPROF_INSTANCE_DUMP);
  __ AddObjectId(obj);
  __ AddClassId(LookupClassId(klass));

  // Reserve some space for the length of the instance data, which we won't
  // know until we're done writing it.
  size_t size_patch_offset = output_->Length();
  __ AddU4(0x77777777);

  // What we will use for the string value if the object is a string.
  mirror::Object* string_value = nullptr;
  mirror::Object* fake_object_array = nullptr;

  // Write the instance data;  fields for this class, followed by super class fields, and so on.
  do {
    const size_t instance_fields = klass->NumInstanceFields();
    for (size_t i = 0; i < instance_fields; ++i) {
      ArtField* f = klass->GetInstanceField(i);
      size_t size;
      HprofBasicType t = SignatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
      switch (t) {
      case hprof_basic_byte:
        __ AddU1(f->GetByte(obj));
        break;
      case hprof_basic_boolean:
        __ AddU1(f->GetBoolean(obj));
        break;
      case hprof_basic_char:
        __ AddU2(f->GetChar(obj));
        break;
      case hprof_basic_short:
        __ AddU2(f->GetShort(obj));
        break;
      case hprof_basic_int:
        if (mirror::kUseStringCompression &&
            klass->IsStringClass() &&
            f->GetOffset().SizeValue() == mirror::String::CountOffset().SizeValue()) {
          // Store the string length instead of the raw count field with compression flag.
          __ AddU4(obj->AsString()->GetLength());
          break;
        }
        FALLTHROUGH_INTENDED;
      case hprof_basic_float:
      case hprof_basic_object:
        __ AddU4(f->Get32(obj));
        break;
      case hprof_basic_double:
      case hprof_basic_long:
        __ AddU8(f->Get64(obj));
        break;
      }
    }
    // Add value field for String if necessary.
    if (klass->IsStringClass()) {
      ObjPtr<mirror::String> s = obj->AsString();
      if (s->GetLength() == 0) {
        // If string is empty, use an object-aligned address within the string for the value.
        string_value = reinterpret_cast<mirror::Object*>(
            reinterpret_cast<uintptr_t>(s.Ptr()) + kObjectAlignment);
      } else {
        if (s->IsCompressed()) {
          string_value = reinterpret_cast<mirror::Object*>(s->GetValueCompressed());
        } else {
          string_value = reinterpret_cast<mirror::Object*>(s->GetValue());
        }
      }
      __ AddObjectId(string_value);
    } else if (AddRuntimeInternalObjectsField(klass)) {
      // We need an id that is guaranteed to not be used, use 1/2 of the object alignment.
      fake_object_array = reinterpret_cast<mirror::Object*>(
          reinterpret_cast<uintptr_t>(obj) + kObjectAlignment / 2);
      __ AddObjectId(fake_object_array);
    }
    klass = klass->GetSuperClass().Ptr();
  } while (klass != nullptr);

  // Patch the instance field length.
  __ UpdateU4(size_patch_offset, output_->Length() - (size_patch_offset + 4));

  // Output native value character array for strings.
  CHECK_EQ(obj->IsString(), string_value != nullptr);
  if (string_value != nullptr) {
    ObjPtr<mirror::String> s = obj->AsString();
    __ AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
    __ AddObjectId(string_value);
    __ AddU4(s->GetLength());
    // Cropped: the characters are not written.
    if (s->IsCompressed()) {
      __ AddU1(hprof_basic_byte);
    } else {
      __ AddU1(hprof_basic_char);
    }
  } else if (fake_object_array != nullptr) {
    DumpFakeObjectArray(fake_object_array, fake_roots);
  }
}

void HprofOpt::VisitRoot(mirror::Object* obj, const RootInfo& info) {
  static const HprofHeapTag xlate[] = {
    HPROF_ROOT_UNKNOWN,
    HPROF_ROOT_JNI_GLOBAL,
    HPROF_ROOT_JNI_LOCAL,
    HPROF_ROOT_JAVA_FRAME,
    HPROF_ROOT_NATIVE_STACK,
    HPROF_ROOT_STICKY_CLASS,
    HPROF_ROOT_THREAD_BLOCK,
    HPROF_ROOT_MONITOR_USED,
    HPROF_ROOT_THREAD_OBJECT,
    HPROF_ROOT_INTERNED_STRING,
    HPROF_ROOT_FINALIZING,
    HPROF_ROOT_DEBUGGER,
    HPROF_ROOT_REFERENCE_CLEANUP,
    HPROF_ROOT_VM_INTERNAL,
    HPROF_ROOT_JNI_MONITOR,
  };
  CHECK_LT(info.GetType(), sizeof(xlate) / sizeof(HprofHeapTag));
  if (obj == nullptr) {
    return;
  }
  MarkRootObject(obj, nullptr, xlate[info.GetType()], info.GetThreadId());
}

// If "fd" is >= 0, the output will be written to that file descriptor.
// Otherwise, "filename" is used to create an output file. "name" is written at the start.
void DumpHeap(const char* filename, int fd, const char* name) {
  CHECK(filename != nullptr);
  Thread* self = Thread::Current();
  // Need to take a heap dump while GC isn't running. See the comment in Heap::VisitObjects().
  // Also we need the critical section to avoid visiting the same object twice. See b/34967844
  gc::ScopedGCCriticalSection gcs(self,
                                  gc::kGcCauseHprof,
                                  gc::kCollectorTypeHprof);
  ScopedSuspendAll ssa(__FUNCTION__, true /* long suspend */);
  HprofOpt hprof(filename, fd, name);
  hprof.Dump();
}

static constexpr const char* kHprofOptDir = "/data/hprofopt";
static constexpr const char* kHprofOptPreFile = "/data/hprofopt/.pre_hprofopt";

// True once this process must not (or no longer) dump: set by setDumpFlag(false) and by the first
// checkDumpHeap() that gets past it.
static bool dump_hprof_ = false;

void setDumpFlag(bool flag) {
  LOG(INFO) << "hprofopt setDumpFlag: " << flag;
  dump_hprof_ = !flag;
}

uint8_t checkDumpHeap() {
  static Mutex* dump_hprof_lock_ = new Mutex("dump hprof lock");
  if (!dump_hprof_lock_->ExclusiveTryLock(Thread::Current())) {
    LOG(INFO) << "hprofopt dumphprof ongoing";
    return kDumpHeapOngoing;
  }
  if (dump_hprof_) {
    LOG(INFO) << "hprofopt dumphprof no need";
    dump_hprof_lock_->ExclusiveUnlock(Thread::Current());
    return kDumpHeapSkipped;
  }
  dump_hprof_ = true;
  if (access(kHprofOptPreFile, F_OK) != 0) {
    LOG(INFO) << "/data/hprofopt/.pre_hprofopt not exist";
    dump_hprof_lock_->ExclusiveUnlock(Thread::Current());
    return kDumpHeapSkipped;
  }

  std::string cmd_line;
  if (android::base::ReadFileToString("/proc/self/cmdline", &cmd_line)) {
    cmd_line.resize(cmd_line.find_last_not_of('\0') + 1);  // trim trailing '\0's
    std::replace(cmd_line.begin(), cmd_line.end(), '\0', ' ');
  }

  time_t now = time(nullptr);
  struct tm tm_now;
  struct tm* t = localtime_r(&now, &tm_now);
  std::string date = android::base::StringPrintf("%04d-%02d-%02d-%02d-%02d-%02d",
                                                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                                 t->tm_hour, t->tm_min, t->tm_sec);
  std::string month = android::base::StringPrintf("%04d-%02d-", t->tm_year + 1900, t->tm_mon + 1);

  DIR* dir = opendir(kHprofOptDir);
  if (dir == nullptr) {
    LOG(ERROR) << "opendir " << kHprofOptDir << ", error: " << strerror(errno);
    dump_hprof_lock_->ExclusiveUnlock(Thread::Current());
    return kDumpHeapSkipped;
  }
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type != DT_REG || strstr(entry->d_name, cmd_line.c_str()) == nullptr) {
      continue;
    }
    if (strstr(entry->d_name, month.c_str()) != nullptr) {
      // One dump per process and month.
      LOG(INFO) << "DumpHprof already exist: " << entry->d_name;
      closedir(dir);
      dump_hprof_lock_->ExclusiveUnlock(Thread::Current());
      return kDumpHeapSkipped;
    }
    // Remove the dumps of this process from earlier months.
    std::string old_dump = android::base::StringPrintf("%s/%s", kHprofOptDir, entry->d_name);
    remove(old_dump.c_str());
  }
  closedir(dir);

  std::string name = android::base::StringPrintf("%s_%d_%d_%s.profopt",
                                                 cmd_line.c_str(),
                                                 getuid(),
                                                 getpid(),
                                                 date.c_str());
  DumpHeap(kHprofOptPreFile, -1, name.c_str());
  dump_hprof_lock_->ExclusiveUnlock(Thread::Current());
  return kDumpHeapDumped;
}

}  // namespace hprof_opt
}  // namespace art
