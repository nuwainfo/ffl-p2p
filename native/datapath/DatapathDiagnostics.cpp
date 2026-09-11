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

#include "datapath/DatapathDiagnostics.h"

#include <cstring>

namespace ffl::datapath {

void DatapathDiagnostics::updateMaximum(
    std::atomic<uint32_t> &value, uint32_t candidate) noexcept {
    uint32_t current = value.load(std::memory_order_relaxed);
    while (candidate > current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

void DatapathDiagnostics::updateMaximum(
    std::atomic<uint64_t> &value, uint64_t candidate) noexcept {
    uint64_t current = value.load(std::memory_order_relaxed);
    while (candidate > current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

void DatapathDiagnostics::recordSendBatchCall() noexcept {
    sendBatchCalls_.fetch_add(1, std::memory_order_relaxed);
}

void DatapathDiagnostics::recordSendCall(
    size_t datagramCount, size_t byteCount, uint64_t durationNS, bool segmented) noexcept {
    sendCalls_.fetch_add(1, std::memory_order_relaxed);
    sendDatagrams_.fetch_add(datagramCount, std::memory_order_relaxed);
    sendBytes_.fetch_add(byteCount, std::memory_order_relaxed);
    sendDurationNS_.fetch_add(durationNS, std::memory_order_relaxed);
    updateMaximum(maxSendDurationNS_, durationNS);
    updateMaximum(maxSendBatch_, static_cast<uint32_t>(datagramCount));

    if (segmented) {
        segmentedCalls_.fetch_add(1, std::memory_order_relaxed);
        segmentedDatagrams_.fetch_add(datagramCount, std::memory_order_relaxed);
    }
}

void DatapathDiagnostics::recordSendFallback() noexcept {
    sendFallbacks_.fetch_add(1, std::memory_order_relaxed);
}

void DatapathDiagnostics::recordReceiveBatchCall() noexcept {
    receiveBatchCalls_.fetch_add(1, std::memory_order_relaxed);
}

void DatapathDiagnostics::recordReceiveCall() noexcept {
    receiveCalls_.fetch_add(1, std::memory_order_relaxed);
}

void DatapathDiagnostics::recordReceiveBatch(
    size_t datagramCount, size_t byteCount, bool coalesced) noexcept {
    receiveDatagrams_.fetch_add(datagramCount, std::memory_order_relaxed);
    receiveBytes_.fetch_add(byteCount, std::memory_order_relaxed);
    updateMaximum(maxReceiveBatch_, static_cast<uint32_t>(datagramCount));

    if (datagramCount > 1) {
        batchedReceiveCalls_.fetch_add(1, std::memory_order_relaxed);
        batchedReceiveDatagrams_.fetch_add(datagramCount, std::memory_order_relaxed);
    }

    if (coalesced && datagramCount > 1) {
        coalescedCalls_.fetch_add(1, std::memory_order_relaxed);
        coalescedDatagrams_.fetch_add(datagramCount, std::memory_order_relaxed);
    }
}

void DatapathDiagnostics::setSendSegmentationSupported(bool supported) noexcept {
    sendSegmentationSupported_.store(supported, std::memory_order_relaxed);
}

void DatapathDiagnostics::setSendSegmentationEnabled(bool value) noexcept {
    sendSegmentationEnabled_.store(value, std::memory_order_relaxed);
}

void DatapathDiagnostics::setReceiveBatchingSupported(bool supported) noexcept {
    receiveBatchingSupported_.store(supported, std::memory_order_relaxed);
}

void DatapathDiagnostics::setReceiveBatchingEnabled(bool value) noexcept {
    receiveBatchingEnabled_.store(value, std::memory_order_relaxed);
}

void DatapathDiagnostics::setReceiveCoalescingSupported(bool supported) noexcept {
    receiveCoalescingSupported_.store(supported, std::memory_order_relaxed);
}

void DatapathDiagnostics::setReceiveCoalescingEnabled(bool value) noexcept {
    receiveCoalescingEnabled_.store(value, std::memory_order_relaxed);
}

void DatapathDiagnostics::getSnapshot(FFLP2PDatapathStats *stats) const noexcept {
    std::memset(stats, 0, sizeof(*stats));

    stats->sendBatchCalls = sendBatchCalls_.load(std::memory_order_relaxed);
    stats->sendCalls = sendCalls_.load(std::memory_order_relaxed);
    stats->sendDatagrams = sendDatagrams_.load(std::memory_order_relaxed);
    stats->sendBytes = sendBytes_.load(std::memory_order_relaxed);
    stats->segmentedCalls = segmentedCalls_.load(std::memory_order_relaxed);
    stats->segmentedDatagrams = segmentedDatagrams_.load(std::memory_order_relaxed);
    stats->sendFallbacks = sendFallbacks_.load(std::memory_order_relaxed);
    stats->sendDurationNS = sendDurationNS_.load(std::memory_order_relaxed);
    stats->maxSendDurationNS = maxSendDurationNS_.load(std::memory_order_relaxed);
    stats->maxSendBatch = maxSendBatch_.load(std::memory_order_relaxed);
    stats->receiveBatchCalls = receiveBatchCalls_.load(std::memory_order_relaxed);
    stats->receiveCalls = receiveCalls_.load(std::memory_order_relaxed);
    stats->receiveDatagrams = receiveDatagrams_.load(std::memory_order_relaxed);
    stats->receiveBytes = receiveBytes_.load(std::memory_order_relaxed);
    stats->batchedReceiveCalls = batchedReceiveCalls_.load(std::memory_order_relaxed);
    stats->batchedReceiveDatagrams = batchedReceiveDatagrams_.load(std::memory_order_relaxed);
    stats->coalescedCalls = coalescedCalls_.load(std::memory_order_relaxed);
    stats->coalescedDatagrams = coalescedDatagrams_.load(std::memory_order_relaxed);
    stats->maxReceiveBatch = maxReceiveBatch_.load(std::memory_order_relaxed);
    stats->sendHookEnabled = 1;
    stats->sendSegmentationSupported =
        sendSegmentationSupported_.load(std::memory_order_relaxed) ? 1 : 0;
    stats->sendSegmentationEnabled =
        sendSegmentationEnabled_.load(std::memory_order_relaxed) ? 1 : 0;
    stats->batchHookEnabled = 1;
    stats->receiveBatchingSupported =
        receiveBatchingSupported_.load(std::memory_order_relaxed) ? 1 : 0;
    stats->receiveBatchingEnabled =
        receiveBatchingEnabled_.load(std::memory_order_relaxed) ? 1 : 0;
    stats->coalescingSupported =
        receiveCoalescingSupported_.load(std::memory_order_relaxed) ? 1 : 0;
    stats->coalescingEnabled =
        receiveCoalescingEnabled_.load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace ffl::datapath
