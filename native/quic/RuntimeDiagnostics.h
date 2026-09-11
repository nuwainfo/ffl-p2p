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

#ifndef FFL_P2P_QUIC_RUNTIME_DIAGNOSTICS_H
#define FFL_P2P_QUIC_RUNTIME_DIAGNOSTICS_H

#include "platform/Platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ffl::quic {

struct RuntimeDiagnosticsSnapshot {
    uint64_t scheduleCount{0};
    uint64_t scheduleLatencySamples{0};
    uint64_t scheduleLatencyNS{0};
    uint64_t maxScheduleLatencyNS{0};
    uint64_t sendFlushCalls{0};
    uint64_t sendFlushIntervalSamples{0};
    uint64_t sendFlushIntervalNS{0};
    uint64_t maxSendFlushIntervalNS{0};
    uint64_t sendFlushDurationNS{0};
    uint64_t maxSendFlushDurationNS{0};
    uint64_t sendFlushPackets{0};
    uint64_t sendFlushBytes{0};
    uint64_t sendFlushTransportCalls{0};
    uint64_t maxSendFlushPackets{0};
    uint64_t maxSendFlushBytes{0};
    uint64_t maxTransportCallsPerFlush{0};
    uint64_t zeroSendFlushesWithPendingData{0};
    uint64_t packetWriteCalls{0};
    uint64_t zeroPacketWriteCalls{0};
    uint64_t packetWriteDurationNS{0};
    uint64_t maxPacketWriteDurationNS{0};
    uint64_t packetWritePackets{0};
    uint64_t packetWriteBytes{0};
    uint64_t transportSendCalls{0};
    uint64_t transportSendDurationNS{0};
    uint64_t maxTransportSendDurationNS{0};
    uint64_t transportSentBytes{0};
    uint64_t receiveFlushCalls{0};
    uint64_t receiveFlushDurationNS{0};
    uint64_t maxReceiveFlushDurationNS{0};
    uint64_t timerCalls{0};
    uint64_t timerDurationNS{0};
    uint64_t maxTimerDurationNS{0};
};

class RuntimeDiagnostics {
public:
    platform::Timestamp startTiming() const noexcept { return platform::getCurrentTimestampNS(); }
    uint64_t elapsedSince(platform::Timestamp startedAt) const noexcept {
        return platform::getCurrentTimestampNS() - startedAt;
    }

    void recordScheduled(platform::Timestamp timestamp) noexcept;
    void recordWorkerStarted(platform::Timestamp timestamp) noexcept;
    void recordSendFlush(platform::Timestamp startedAt, uint64_t durationNS,
                         uint64_t packetCount, uint64_t byteCount, uint64_t transportCalls,
                         bool pendingDataWithoutPackets) noexcept;
    void recordPacketWrite(uint64_t durationNS, uint64_t packetCount, uint64_t byteCount) noexcept;
    void recordTransportSend(uint64_t durationNS, uint64_t sentBytes) noexcept;
    void recordReceiveFlush(uint64_t durationNS) noexcept;
    void recordTimer(uint64_t durationNS) noexcept;

    RuntimeDiagnosticsSnapshot getSnapshot() const noexcept;

private:
    static void updateMaximum(std::atomic<uint64_t> &value, uint64_t candidate) noexcept;

    std::atomic<platform::Timestamp> scheduledAt_{0};
    std::atomic<uint64_t> scheduleCount_{0};
    std::atomic<uint64_t> scheduleLatencySamples_{0};
    std::atomic<uint64_t> scheduleLatencyNS_{0};
    std::atomic<uint64_t> maxScheduleLatencyNS_{0};
    std::atomic<uint64_t> sendFlushCalls_{0};
    std::atomic<platform::Timestamp> previousSendFlushAt_{0};
    std::atomic<uint64_t> sendFlushIntervalSamples_{0};
    std::atomic<uint64_t> sendFlushIntervalNS_{0};
    std::atomic<uint64_t> maxSendFlushIntervalNS_{0};
    std::atomic<uint64_t> sendFlushDurationNS_{0};
    std::atomic<uint64_t> maxSendFlushDurationNS_{0};
    std::atomic<uint64_t> sendFlushPackets_{0};
    std::atomic<uint64_t> sendFlushBytes_{0};
    std::atomic<uint64_t> sendFlushTransportCalls_{0};
    std::atomic<uint64_t> maxSendFlushPackets_{0};
    std::atomic<uint64_t> maxSendFlushBytes_{0};
    std::atomic<uint64_t> maxTransportCallsPerFlush_{0};
    std::atomic<uint64_t> zeroSendFlushesWithPendingData_{0};
    std::atomic<uint64_t> packetWriteCalls_{0};
    std::atomic<uint64_t> zeroPacketWriteCalls_{0};
    std::atomic<uint64_t> packetWriteDurationNS_{0};
    std::atomic<uint64_t> maxPacketWriteDurationNS_{0};
    std::atomic<uint64_t> packetWritePackets_{0};
    std::atomic<uint64_t> packetWriteBytes_{0};
    std::atomic<uint64_t> transportSendCalls_{0};
    std::atomic<uint64_t> transportSendDurationNS_{0};
    std::atomic<uint64_t> maxTransportSendDurationNS_{0};
    std::atomic<uint64_t> transportSentBytes_{0};
    std::atomic<uint64_t> receiveFlushCalls_{0};
    std::atomic<uint64_t> receiveFlushDurationNS_{0};
    std::atomic<uint64_t> maxReceiveFlushDurationNS_{0};
    std::atomic<uint64_t> timerCalls_{0};
    std::atomic<uint64_t> timerDurationNS_{0};
    std::atomic<uint64_t> maxTimerDurationNS_{0};
};

} // namespace ffl::quic

#endif
