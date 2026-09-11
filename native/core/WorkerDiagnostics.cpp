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

#include "core/WorkerDiagnostics.h"

namespace ffl::core {

void WorkerDiagnostics::updateMaximum(
    std::atomic<uint64_t> &value, uint64_t candidate) noexcept {
    uint64_t current = value.load(std::memory_order_relaxed);
    while (candidate > current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

void WorkerDiagnostics::recordWake(
    bool eventSignaled, platform::Timestamp deadline, platform::Timestamp actualWake) noexcept {
    if (eventSignaled)
        eventWakeups_.fetch_add(1, std::memory_order_relaxed);

    if (deadline == platform::InfiniteTimestamp || actualWake < deadline)
        return;

    timerWakeups_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t lateness = actualWake - deadline;
    timerWakeLatenessNS_.fetch_add(lateness, std::memory_order_relaxed);
    updateMaximum(maxTimerWakeLatenessNS_, lateness);
}

void WorkerDiagnostics::recordConnectionRun(
    size_t operationsProcessed, uint64_t durationNS) noexcept {
    connectionRuns_.fetch_add(1, std::memory_order_relaxed);
    operationsProcessed_.fetch_add(operationsProcessed, std::memory_order_relaxed);
    processingDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    updateMaximum(maxProcessingDurationNS_, durationNS);
    updateMaximum(maxOperationsPerRun_, static_cast<uint64_t>(operationsProcessed));
}

WorkerDiagnosticsSnapshot WorkerDiagnostics::getSnapshot() const noexcept {
    WorkerDiagnosticsSnapshot result;
    result.eventWakeups = eventWakeups_.load(std::memory_order_relaxed);
    result.timerWakeups = timerWakeups_.load(std::memory_order_relaxed);
    result.timerWakeLatenessNS = timerWakeLatenessNS_.load(std::memory_order_relaxed);
    result.maxTimerWakeLatenessNS = maxTimerWakeLatenessNS_.load(std::memory_order_relaxed);
    result.connectionRuns = connectionRuns_.load(std::memory_order_relaxed);
    result.operationsProcessed = operationsProcessed_.load(std::memory_order_relaxed);
    result.processingDurationNS = processingDurationNS_.load(std::memory_order_relaxed);
    result.maxProcessingDurationNS = maxProcessingDurationNS_.load(std::memory_order_relaxed);
    result.maxOperationsPerRun = maxOperationsPerRun_.load(std::memory_order_relaxed);
    return result;
}

} // namespace ffl::core
