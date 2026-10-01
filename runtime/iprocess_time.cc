/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "iprocess_time.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace art {

// Size of the counter page mapped from /dev/binder_get_time.
static constexpr size_t kTimePageSize = 4096;
// The kernel jiffies counters start at INITIAL_JIFFIES (-300 s at HZ=250, 32-bit wrapped) and
// tick every 4 ms: millis = (jiffies - INITIAL_JIFFIES) * 4, as computed by the factory libart.
static constexpr uint64_t kInitialJiffies = static_cast<uint32_t>(-300 * 250);
static constexpr uint64_t kMillisPerJiffy = 4;

IProcessTime::IProcessTime() {
  fd_ = open("/dev/binder_get_time", O_RDONLY);
  time_ = nullptr;
  if (fd_ > 0) {
    time_ = reinterpret_cast<uint64_t*>(
        mmap(nullptr, kTimePageSize, PROT_READ, MAP_SHARED, fd_, 0));
    if (time_ == MAP_FAILED || time_ == nullptr) {
      close(fd_);
      fd_ = -1;
    }
  }
}

IProcessTime::~IProcessTime() {
  if (fd_ > 0) {
    if (time_ != MAP_FAILED && time_ != nullptr) {
      munmap(time_, kTimePageSize);
    }
    close(fd_);
    fd_ = -1;
  }
}

int64_t IProcessTime::getSysMillisecond() {
  if (time_ == nullptr) {
    return -1;
  }
  return static_cast<int64_t>((time_[0] - kInitialJiffies) * kMillisPerJiffy);
}

int64_t IProcessTime::getSysUptimeMillis() {
  if (time_ == nullptr) {
    return -1;
  }
  return static_cast<int64_t>((time_[2] - kInitialJiffies) * kMillisPerJiffy);
}

}  // namespace art
