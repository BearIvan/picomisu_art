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

#ifndef ART_RUNTIME_IPROCESS_TIME_H_
#define ART_RUNTIME_IPROCESS_TIME_H_

#include <stdint.h>

namespace art {

// PICO OS 5.13.7 (Smartisan) process time source: a read-only page of kernel time counters shared
// by /dev/binder_get_time. The runtime keeps one instance per forked non-zygote process
// (Runtime::process_time_); dalvik.system.VMDebug.getSysMillisecond/getSysUptimeMillis read it.
class IProcessTime {
 public:
  IProcessTime();
  ~IProcessTime();

  // Milliseconds since boot from the jiffies counter (word 0 of the page); -1 if unmapped.
  int64_t getSysMillisecond();
  // Uptime milliseconds from the jiffies-based counter in word 2 of the page; -1 if unmapped.
  int64_t getSysUptimeMillis();

 private:
  int fd_;
  uint64_t* time_;
};

}  // namespace art

#endif  // ART_RUNTIME_IPROCESS_TIME_H_
