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

#include "quic/RuntimeDiagnostics.h"

namespace ffl::quic {

void RuntimeDiagnostics::updateMaximum(
    std::atomic<uint64_t> &value, uint64_t candidate) noexcept {
    uint64_t current = value.load(std::memory_order_relaxed);
    while (candidate > current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

void RuntimeDiagnostics::recordScheduled(platform::Timestamp timestamp) noexcept {
    scheduledAt_.store(timestamp, std::memory_order_release);
    scheduleCount_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeDiagnostics::recordWorkerStarted(platform::Timestamp timestamp) noexcept {
    const platform::Timestamp scheduledAt = scheduledAt_.exchange(0, std::memory_order_acq_rel);
    if (scheduledAt == 0 || timestamp < scheduledAt)
        return;

    const uint64_t latency = timestamp - scheduledAt;
    scheduleLatencySamples_.fetch_add(1, std::memory_order_relaxed);
    scheduleLatencyNS_.fetch_add(latency, std::memory_order_relaxed);
    updateMaximum(maxScheduleLatencyNS_, latency);
}

void RuntimeDiagnostics::recordSendFlush(
    platform::Timestamp startedAt, uint64_t durationNS, uint64_t packetCount,
    uint64_t byteCount, uint64_t transportCalls, bool pendingDataWithoutPackets) noexcept {
    sendFlushCalls_.fetch_add(1, std::memory_order_relaxed);
    const platform::Timestamp previousStartedAt =
        previousSendFlushAt_.exchange(startedAt, std::memory_order_relaxed);
    if (previousStartedAt != 0 && startedAt >= previousStartedAt) {
        const uint64_t interval = startedAt - previousStartedAt;
        sendFlushIntervalSamples_.fetch_add(1, std::memory_order_relaxed);
        sendFlushIntervalNS_.fetch_add(interval, std::memory_order_relaxed);
        updateMaximum(maxSendFlushIntervalNS_, interval);
    }
    sendFlushDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    sendFlushPackets_.fetch_add(packetCount, std::memory_order_relaxed);
    sendFlushBytes_.fetch_add(byteCount, std::memory_order_relaxed);
    sendFlushTransportCalls_.fetch_add(transportCalls, std::memory_order_relaxed);
    updateMaximum(maxSendFlushDurationNS_, durationNS);
    updateMaximum(maxSendFlushPackets_, packetCount);
    updateMaximum(maxSendFlushBytes_, byteCount);
    updateMaximum(maxTransportCallsPerFlush_, transportCalls);

    if (pendingDataWithoutPackets)
        zeroSendFlushesWithPendingData_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeDiagnostics::recordPacketWrite(
    uint64_t durationNS, uint64_t packetCount, uint64_t byteCount) noexcept {
    packetWriteCalls_.fetch_add(1, std::memory_order_relaxed);
    packetWriteDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    packetWritePackets_.fetch_add(packetCount, std::memory_order_relaxed);
    packetWriteBytes_.fetch_add(byteCount, std::memory_order_relaxed);
    updateMaximum(maxPacketWriteDurationNS_, durationNS);

    if (packetCount == 0 || byteCount == 0)
        zeroPacketWriteCalls_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeDiagnostics::recordTransportSend(uint64_t durationNS, uint64_t sentBytes) noexcept {
    transportSendCalls_.fetch_add(1, std::memory_order_relaxed);
    transportSendDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    transportSentBytes_.fetch_add(sentBytes, std::memory_order_relaxed);
    updateMaximum(maxTransportSendDurationNS_, durationNS);
}

void RuntimeDiagnostics::recordReceiveFlush(uint64_t durationNS) noexcept {
    receiveFlushCalls_.fetch_add(1, std::memory_order_relaxed);
    receiveFlushDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    updateMaximum(maxReceiveFlushDurationNS_, durationNS);
}

void RuntimeDiagnostics::recordTimer(uint64_t durationNS) noexcept {
    timerCalls_.fetch_add(1, std::memory_order_relaxed);
    timerDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    updateMaximum(maxTimerDurationNS_, durationNS);
}

RuntimeDiagnosticsSnapshot RuntimeDiagnostics::getSnapshot() const noexcept {
    RuntimeDiagnosticsSnapshot result;
    result.scheduleCount = scheduleCount_.load(std::memory_order_relaxed);
    result.scheduleLatencySamples = scheduleLatencySamples_.load(std::memory_order_relaxed);
    result.scheduleLatencyNS = scheduleLatencyNS_.load(std::memory_order_relaxed);
    result.maxScheduleLatencyNS = maxScheduleLatencyNS_.load(std::memory_order_relaxed);
    result.sendFlushCalls = sendFlushCalls_.load(std::memory_order_relaxed);
    result.sendFlushIntervalSamples = sendFlushIntervalSamples_.load(std::memory_order_relaxed);
    result.sendFlushIntervalNS = sendFlushIntervalNS_.load(std::memory_order_relaxed);
    result.maxSendFlushIntervalNS = maxSendFlushIntervalNS_.load(std::memory_order_relaxed);
    result.sendFlushDurationNS = sendFlushDurationNS_.load(std::memory_order_relaxed);
    result.maxSendFlushDurationNS = maxSendFlushDurationNS_.load(std::memory_order_relaxed);
    result.sendFlushPackets = sendFlushPackets_.load(std::memory_order_relaxed);
    result.sendFlushBytes = sendFlushBytes_.load(std::memory_order_relaxed);
    result.sendFlushTransportCalls = sendFlushTransportCalls_.load(std::memory_order_relaxed);
    result.maxSendFlushPackets = maxSendFlushPackets_.load(std::memory_order_relaxed);
    result.maxSendFlushBytes = maxSendFlushBytes_.load(std::memory_order_relaxed);
    result.maxTransportCallsPerFlush = maxTransportCallsPerFlush_.load(std::memory_order_relaxed);
    result.zeroSendFlushesWithPendingData = zeroSendFlushesWithPendingData_.load(std::memory_order_relaxed);
    result.packetWriteCalls = packetWriteCalls_.load(std::memory_order_relaxed);
    result.zeroPacketWriteCalls = zeroPacketWriteCalls_.load(std::memory_order_relaxed);
    result.packetWriteDurationNS = packetWriteDurationNS_.load(std::memory_order_relaxed);
    result.maxPacketWriteDurationNS = maxPacketWriteDurationNS_.load(std::memory_order_relaxed);
    result.packetWritePackets = packetWritePackets_.load(std::memory_order_relaxed);
    result.packetWriteBytes = packetWriteBytes_.load(std::memory_order_relaxed);
    result.transportSendCalls = transportSendCalls_.load(std::memory_order_relaxed);
    result.transportSendDurationNS = transportSendDurationNS_.load(std::memory_order_relaxed);
    result.maxTransportSendDurationNS = maxTransportSendDurationNS_.load(std::memory_order_relaxed);
    result.transportSentBytes = transportSentBytes_.load(std::memory_order_relaxed);
    result.receiveFlushCalls = receiveFlushCalls_.load(std::memory_order_relaxed);
    result.receiveFlushDurationNS = receiveFlushDurationNS_.load(std::memory_order_relaxed);
    result.maxReceiveFlushDurationNS = maxReceiveFlushDurationNS_.load(std::memory_order_relaxed);
    result.timerCalls = timerCalls_.load(std::memory_order_relaxed);
    result.timerDurationNS = timerDurationNS_.load(std::memory_order_relaxed);
    result.maxTimerDurationNS = maxTimerDurationNS_.load(std::memory_order_relaxed);
    return result;
}

} // namespace ffl::quic
