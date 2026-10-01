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

#ifndef ART_RUNTIME_HPROF_HPROF_OPT_H_
#define ART_RUNTIME_HPROF_HPROF_OPT_H_

#include <stdint.h>

namespace art {

// PICO OS 5.13.7 (Smartisan) cropped heap dump.
namespace hprof_opt {

// checkDumpHeap() results.
static constexpr uint8_t kDumpHeapOngoing = 0;  // Another thread is dumping.
static constexpr uint8_t kDumpHeapDumped = 1;   // A cropped dump was written.
static constexpr uint8_t kDumpHeapSkipped = 2;  // No dump needed or possible.

// Writes a cropped heap dump to "fd" if >= 0, else to "filename"; "name" is written first.
void DumpHeap(const char* filename, int fd, const char* name);

// dalvik.system.VMDebug.setDumpFlag: false disables the out-of-memory dump of this process.
void setDumpFlag(bool flag);

// Out-of-memory hook: once per process and month, writes a cropped dump (prefixed with
// "<cmdline>_<uid>_<pid>_<date>.profopt") into /data/hprofopt/.pre_hprofopt when that file exists.
uint8_t checkDumpHeap();

}  // namespace hprof_opt

}  // namespace art

#endif  // ART_RUNTIME_HPROF_HPROF_OPT_H_
