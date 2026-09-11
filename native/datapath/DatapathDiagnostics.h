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

#ifndef FFL_P2P_DATAPATH_DIAGNOSTICS_H
#define FFL_P2P_DATAPATH_DIAGNOSTICS_H

#include "datapath/Datapath.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFLP2PDatapathStats {
    uint64_t sendBatchCalls;
    uint64_t sendCalls;
    uint64_t sendDatagrams;
    uint64_t sendBytes;
    uint64_t segmentedCalls;
    uint64_t segmentedDatagrams;
    uint64_t sendFallbacks;
    uint64_t sendDurationNS;
    uint64_t maxSendDurationNS;
    uint32_t maxSendBatch;
    uint64_t receiveBatchCalls;
    uint64_t receiveCalls;
    uint64_t receiveDatagrams;
    uint64_t receiveBytes;
    uint64_t batchedReceiveCalls;
    uint64_t batchedReceiveDatagrams;
    uint64_t coalescedCalls;
    uint64_t coalescedDatagrams;
    uint32_t maxReceiveBatch;
    int sendHookEnabled;
    int sendSegmentationSupported;
    int sendSegmentationEnabled;
    int batchHookEnabled;
    int receiveBatchingSupported;
    int receiveBatchingEnabled;
    int coalescingSupported;
    int coalescingEnabled;
} FFLP2PDatapathStats;

void getFFLP2PDatapathStats(FFLP2PDatapath *datapath, FFLP2PDatapathStats *stats);

#ifdef __cplusplus
}

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ffl::datapath {

class DatapathDiagnostics {
public:
    void recordSendBatchCall() noexcept;
    void recordSendCall(size_t datagramCount, size_t byteCount, uint64_t durationNS,
                        bool segmented) noexcept;
    void recordSendFallback() noexcept;
    void recordReceiveBatchCall() noexcept;
    void recordReceiveCall() noexcept;
    void recordReceiveBatch(size_t datagramCount, size_t byteCount, bool coalesced) noexcept;

    void setSendSegmentationSupported(bool supported) noexcept;
    void setSendSegmentationEnabled(bool value) noexcept;
    void setReceiveBatchingSupported(bool supported) noexcept;
    void setReceiveBatchingEnabled(bool value) noexcept;
    void setReceiveCoalescingSupported(bool supported) noexcept;
    void setReceiveCoalescingEnabled(bool value) noexcept;

    void getSnapshot(FFLP2PDatapathStats *stats) const noexcept;

private:
    static void updateMaximum(std::atomic<uint32_t> &value, uint32_t candidate) noexcept;
    static void updateMaximum(std::atomic<uint64_t> &value, uint64_t candidate) noexcept;

    std::atomic<uint64_t> sendBatchCalls_{0};
    std::atomic<uint64_t> sendCalls_{0};
    std::atomic<uint64_t> sendDatagrams_{0};
    std::atomic<uint64_t> sendBytes_{0};
    std::atomic<uint64_t> segmentedCalls_{0};
    std::atomic<uint64_t> segmentedDatagrams_{0};
    std::atomic<uint64_t> sendFallbacks_{0};
    std::atomic<uint64_t> sendDurationNS_{0};
    std::atomic<uint64_t> maxSendDurationNS_{0};
    std::atomic<uint32_t> maxSendBatch_{0};
    std::atomic<uint64_t> receiveBatchCalls_{0};
    std::atomic<uint64_t> receiveCalls_{0};
    std::atomic<uint64_t> receiveDatagrams_{0};
    std::atomic<uint64_t> receiveBytes_{0};
    std::atomic<uint64_t> batchedReceiveCalls_{0};
    std::atomic<uint64_t> batchedReceiveDatagrams_{0};
    std::atomic<uint64_t> coalescedCalls_{0};
    std::atomic<uint64_t> coalescedDatagrams_{0};
    std::atomic<uint32_t> maxReceiveBatch_{0};
    std::atomic<bool> sendSegmentationSupported_{false};
    std::atomic<bool> sendSegmentationEnabled_{false};
    std::atomic<bool> receiveBatchingSupported_{false};
    std::atomic<bool> receiveBatchingEnabled_{false};
    std::atomic<bool> receiveCoalescingSupported_{false};
    std::atomic<bool> receiveCoalescingEnabled_{false};
};

} // namespace ffl::datapath
#endif

#endif
