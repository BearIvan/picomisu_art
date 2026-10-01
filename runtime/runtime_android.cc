/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "runtime.h"

#include <signal.h>

#include <cstring>
#include <sstream>

#include "base/mutex-inl.h"
#include "runtime_common.h"
#include "thread-current-inl.h"
#include "thread.h"

// PICO OS 5.13.7: exported helpers that dump the stack of the current thread / of a given thread.

// Dumps the Java stack of the given thread to "os".
extern "C" void threadDumpJavaStack(std::ostream& os, const art::Thread* thread)
    NO_THREAD_SAFETY_ANALYSIS {
  thread->DumpJavaStack(os, /* check_suspended= */ true, /* dump_locks= */ true);
}

// Dumps the given thread (Java and native stack) to "os".
extern "C" void threadDumpStack(std::ostream& os, const art::Thread* thread)
    NO_THREAD_SAFETY_ANALYSIS {
  thread->Dump(os, /* dump_native_stack= */ true, /* backtrace_map= */ nullptr,
               /* force_dump_stack= */ false);
}

// Returns a malloc'ed dump of the Java stack of the current thread, or null when the thread is not
// attached, the mutator lock is not available or the dump is shorter than 30 characters.
extern "C" char* DumpJavaStack() NO_THREAD_SAFETY_ANALYSIS {
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) {
    return nullptr;
  }
  std::ostringstream oss;
  if (art::Locks::mutator_lock_->IsExclusiveHeld(self) ||
      art::Locks::mutator_lock_->IsSharedHeld(self)) {
    self->DumpJavaStack(oss, /* check_suspended= */ true, /* dump_locks= */ true);
  } else if (art::Locks::mutator_lock_->SharedTryLock(self)) {
    self->DumpJavaStack(oss, /* check_suspended= */ true, /* dump_locks= */ true);
    art::Locks::mutator_lock_->SharedUnlock(self);
  } else {
    return nullptr;
  }
  if (oss.str().size() < 30) {
    return nullptr;
  }
  return strdup(oss.str().c_str());
}

// Returns a malloc'ed dump of the current thread (Java and native stack), or null when the thread
// is not attached, the mutator lock is not available or the dump is shorter than 30 characters.
extern "C" char* DumpStack() NO_THREAD_SAFETY_ANALYSIS {
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) {
    return nullptr;
  }
  std::ostringstream oss;
  if (art::Locks::mutator_lock_->IsExclusiveHeld(self) ||
      art::Locks::mutator_lock_->IsSharedHeld(self)) {
    self->Dump(oss, /* dump_native_stack= */ true, /* backtrace_map= */ nullptr,
               /* force_dump_stack= */ false);
  } else if (art::Locks::mutator_lock_->SharedTryLock(self)) {
    self->Dump(oss, /* dump_native_stack= */ true, /* backtrace_map= */ nullptr,
               /* force_dump_stack= */ false);
    art::Locks::mutator_lock_->SharedUnlock(self);
  } else {
    return nullptr;
  }
  if (oss.str().size() < 30) {
    return nullptr;
  }
  return strdup(oss.str().c_str());
}

namespace art {

struct sigaction old_action;

void HandleUnexpectedSignalAndroid(int signal_number, siginfo_t* info, void* raw_context) {
  // PICO OS 5.13.7: log the Java stack of the crashing thread for the tombstone instead of the
  // runtime's own crash dump, then run the old (debuggerd) handler.
  char* java_stack = DumpJavaStack();
  if (java_stack != nullptr) {
    LOG(ERROR) << ">>>>>>> Java Backtrace for tombstone <<<<<<<" << std::endl << java_stack;
  }

  // Run the old signal handler.
  old_action.sa_sigaction(signal_number, info, raw_context);
}

void Runtime::InitPlatformSignalHandlers() {
  // PICO OS 5.13.7: install the handler logging the Java backtrace of crashing threads when the
  // Android root is "/system".
  const char* android_root = getenv("ANDROID_ROOT");
  LOG(INFO) << "java trace enabled!" << std::endl;
  if (android_root != nullptr && strcmp(android_root, "/system") == 0) {
    InitPlatformSignalHandlersCommon(HandleUnexpectedSignalAndroid,
                                     &old_action,
                                     /* handle_timeout_signal= */ false);
  }
}

}  // namespace art
