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

#include "dalvik_system_VMStack.h"

#include <unistd.h>

#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "backtrace/BacktraceMap.h"
#include "nativehelper/jni_macros.h"

#include "art_method-inl.h"
#include "gc/task_processor.h"
#include "jni/jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "native_util.h"
#include "nth_caller_visitor.h"
#include "scoped_fast_native_object_access-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "thread_list.h"

namespace art {

template <typename T,
          typename ResultT =
              typename std::result_of<T(Thread*, const ScopedFastNativeObjectAccess&)>::type>
static ResultT GetThreadStack(const ScopedFastNativeObjectAccess& soa,
                              jobject peer,
                              T fn)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ResultT trace = nullptr;
  ObjPtr<mirror::Object> decoded_peer = soa.Decode<mirror::Object>(peer);
  if (decoded_peer == soa.Self()->GetPeer()) {
    trace = fn(soa.Self(), soa);
  } else {
    // Never allow suspending the heap task thread since it may deadlock if allocations are
    // required for the stack trace.
    Thread* heap_task_thread =
        Runtime::Current()->GetHeap()->GetTaskProcessor()->GetRunningThread();
    // heap_task_thread could be null if the daemons aren't yet started.
    if (heap_task_thread != nullptr && decoded_peer == heap_task_thread->GetPeerFromOtherThread()) {
      return nullptr;
    }
    // Suspend thread to build stack trace.
    ScopedThreadSuspension sts(soa.Self(), kNative);
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    bool timed_out;
    Thread* thread = thread_list->SuspendThreadByPeer(peer,
                                                      /* request_suspension= */ true,
                                                      SuspendReason::kInternal,
                                                      &timed_out);
    if (thread != nullptr) {
      // Must be runnable to create returned array.
      {
        ScopedObjectAccess soa2(soa.Self());
        trace = fn(thread, soa);
      }
      // Restart suspended thread.
      bool resumed = thread_list->Resume(thread, SuspendReason::kInternal);
      DCHECK(resumed);
    } else if (timed_out) {
      LOG(ERROR) << "Trying to get thread's stack failed as the thread failed to suspend within a "
          "generous timeout.";
    }
  }
  return trace;
}

static jint VMStack_fillStackTraceElements(JNIEnv* env, jclass, jobject javaThread,
                                           jobjectArray javaSteArray) {
  ScopedFastNativeObjectAccess soa(env);
  auto fn = [](Thread* thread, const ScopedFastNativeObjectAccess& soaa)
      REQUIRES_SHARED(Locks::mutator_lock_) -> jobject {
    return thread->CreateInternalStackTrace<false>(soaa);
  };
  jobject trace = GetThreadStack(soa, javaThread, fn);
  if (trace == nullptr) {
    return 0;
  }
  int32_t depth;
  Thread::InternalStackTraceToStackTraceElementArray(soa, trace, javaSteArray, &depth);
  return depth;
}

// Returns the defining class loader of the caller's caller.
static jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self(), 2);
  visitor.WalkStack();
  if (UNLIKELY(visitor.caller == nullptr)) {
    // The caller is an attached native thread.
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(visitor.caller->GetDeclaringClass()->GetClassLoader());
}

static jobject VMStack_getClosestUserClassLoader(JNIEnv* env, jclass) {
  struct ClosestUserClassLoaderVisitor : public StackVisitor {
    explicit ClosestUserClassLoaderVisitor(Thread* thread)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        class_loader(nullptr) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK(class_loader == nullptr);
      ObjPtr<mirror::Class> c = GetMethod()->GetDeclaringClass();
      // c is null for runtime methods.
      if (c != nullptr) {
        ObjPtr<mirror::Object> cl = c->GetClassLoader();
        if (cl != nullptr) {
          class_loader = cl;
          return false;
        }
      }
      return true;
    }

    ObjPtr<mirror::Object> class_loader;
  };
  ScopedFastNativeObjectAccess soa(env);
  ClosestUserClassLoaderVisitor visitor(soa.Self());
  visitor.WalkStack();
  return soa.AddLocalReference<jobject>(visitor.class_loader);
}

// Returns the class of the caller's caller's caller.
static jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self(), 3);
  visitor.WalkStack();
  if (UNLIKELY(visitor.caller == nullptr)) {
    // The caller is an attached native thread.
    return nullptr;
  }
  return soa.AddLocalReference<jclass>(visitor.caller->GetDeclaringClass());
}

static jobjectArray VMStack_getThreadStackTrace(JNIEnv* env, jclass, jobject javaThread) {
  ScopedFastNativeObjectAccess soa(env);
  auto fn = [](Thread* thread, const ScopedFastNativeObjectAccess& soaa)
     REQUIRES_SHARED(Locks::mutator_lock_) -> jobject {
    return thread->CreateInternalStackTrace<false>(soaa);
  };
  jobject trace = GetThreadStack(soa, javaThread, fn);
  if (trace == nullptr) {
    return nullptr;
  }
  return Thread::InternalStackTraceToStackTraceElementArray(soa, trace);
}

static jobjectArray VMStack_getAnnotatedThreadStackTrace(JNIEnv* env, jclass, jobject javaThread) {
  ScopedFastNativeObjectAccess soa(env);
  auto fn = [](Thread* thread, const ScopedFastNativeObjectAccess& soaa)
      REQUIRES_SHARED(Locks::mutator_lock_) -> jobjectArray {
    return thread->CreateAnnotatedStackTrace(soaa);
  };
  return GetThreadStack(soa, javaThread, fn);
}

// PICO OS 5.13.7: dumps the full stack of the thread with kernel tid "tid" (when > 0), else of the
// thread with thin-lock id "thread_id" (when != 0), else of "javaThread".
static jstring VMStack_getThreadStackAll(JNIEnv* env, jclass, jint tid, jint thread_id,
                                         jobject javaThread) {
  ScopedFastNativeObjectAccess soa(env);
  std::ostringstream os;
  Thread* self = soa.Self();
  if ((javaThread != nullptr && soa.Decode<mirror::Object>(javaThread) == self->GetPeer()) ||
      self->GetTid() == tid ||
      self->GetThreadId() == static_cast<uint32_t>(thread_id)) {
    self->Dump(os);
    return env->NewStringUTF(os.str().c_str());
  }
  // Never allow suspending the heap task thread since it may deadlock if allocations are
  // required for the stack trace; its dump is empty.
  Thread* heap_task_thread =
      Runtime::Current()->GetHeap()->GetTaskProcessor()->GetRunningThread();
  if (heap_task_thread != nullptr &&
      (heap_task_thread->GetTid() == tid ||
       heap_task_thread->GetThreadId() == static_cast<uint32_t>(thread_id) ||
       soa.Decode<mirror::Object>(javaThread) == heap_task_thread->GetPeerFromOtherThread())) {
    return env->NewStringUTF(os.str().c_str());
  }
  {
    // Suspend thread to build stack trace.
    ScopedThreadSuspension sts(self, kNative);
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    bool timed_out;
    Thread* thread;
    if (tid > 0) {
      thread = thread_list->SuspendThreadByThreadId(0, SuspendReason::kInternal, &timed_out, tid);
    } else if (thread_id != 0) {
      thread = thread_list->SuspendThreadByThreadId(thread_id, SuspendReason::kInternal, &timed_out);
    } else if (javaThread != nullptr) {
      thread = thread_list->SuspendThreadByPeer(javaThread,
                                                /* request_suspension= */ true,
                                                SuspendReason::kInternal,
                                                &timed_out);
    } else {
      // The factory runtime reports a missing target like a suspension timeout.
      thread = nullptr;
      timed_out = true;
    }
    if (thread != nullptr) {
      {
        ScopedObjectAccess soa2(self);
        // As on the factory: the map is created (and never freed) but not passed to Dump().
        BacktraceMap* map = BacktraceMap::Create(getpid());
        map->SetSuffixesToIgnore(std::vector<std::string> { "oat", "odex" });
        thread->Dump(os);
      }
      // Restart suspended thread.
      bool resumed = thread_list->Resume(thread, SuspendReason::kInternal);
      DCHECK(resumed);
    } else if (timed_out) {
      LOG(ERROR) << "Trying to get thread's stack failed as the thread failed to suspend within a "
          "generous timeout.";
    }
  }
  return env->NewStringUTF(os.str().c_str());
}

static JNINativeMethod gMethods[] = {
  FAST_NATIVE_METHOD(VMStack, fillStackTraceElements, "(Ljava/lang/Thread;[Ljava/lang/StackTraceElement;)I"),
  FAST_NATIVE_METHOD(VMStack, getCallingClassLoader, "()Ljava/lang/ClassLoader;"),
  FAST_NATIVE_METHOD(VMStack, getClosestUserClassLoader, "()Ljava/lang/ClassLoader;"),
  FAST_NATIVE_METHOD(VMStack, getStackClass2, "()Ljava/lang/Class;"),
  FAST_NATIVE_METHOD(VMStack, getThreadStackTrace, "(Ljava/lang/Thread;)[Ljava/lang/StackTraceElement;"),
  FAST_NATIVE_METHOD(VMStack, getAnnotatedThreadStackTrace, "(Ljava/lang/Thread;)[Ldalvik/system/AnnotatedStackTraceElement;"),
  FAST_NATIVE_METHOD(VMStack, getThreadStackAll, "(IILjava/lang/Thread;)Ljava/lang/String;"),
};

void register_dalvik_system_VMStack(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMStack");
}

}  // namespace art
