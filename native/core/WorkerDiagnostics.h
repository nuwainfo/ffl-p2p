/**
SPDX-License-Identifier: Apache-2.0

FastFileLink CLI - Fast, no-fuss file sharing
Copyright (C) 2025-2026 FastFileLink contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef FFL_P2P_CORE_WORKER_DIAGNOSTICS_H
#define FFL_P2P_CORE_WORKER_DIAGNOSTICS_H

#include "platform/Platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ffl::core {

struct WorkerDiagnosticsSnapshot {
    uint64_t eventWakeups{0};
    uint64_t timerWakeups{0};
    uint64_t timerWakeLatenessNS{0};
    uint64_t maxTimerWakeLatenessNS{0};
    uint64_t connectionRuns{0};
    uint64_t operationsProcessed{0};
    uint64_t processingDurationNS{0};
    uint64_t maxProcessingDurationNS{0};
    uint64_t maxOperationsPerRun{0};
};

class WorkerDiagnostics {
public:
    platform::Timestamp startTiming() const noexcept { return platform::getCurrentTimestampNS(); }
    uint64_t elapsedSince(platform::Timestamp startedAt) const noexcept {
        return platform::getCurrentTimestampNS() - startedAt;
    }

    void recordWake(bool eventSignaled, platform::Timestamp deadline,
                    platform::Timestamp actualWake) noexcept;
    void recordConnectionRun(size_t operationsProcessed, uint64_t durationNS) noexcept;

    WorkerDiagnosticsSnapshot getSnapshot() const noexcept;

private:
    static void updateMaximum(std::atomic<uint64_t> &value, uint64_t candidate) noexcept;

    std::atomic<uint64_t> eventWakeups_{0};
    std::atomic<uint64_t> timerWakeups_{0};
    std::atomic<uint64_t> timerWakeLatenessNS_{0};
    std::atomic<uint64_t> maxTimerWakeLatenessNS_{0};
    std::atomic<uint64_t> connectionRuns_{0};
    std::atomic<uint64_t> operationsProcessed_{0};
    std::atomic<uint64_t> processingDurationNS_{0};
    std::atomic<uint64_t> maxProcessingDurationNS_{0};
    std::atomic<uint64_t> maxOperationsPerRun_{0};
};

} // namespace ffl::core

#endif
