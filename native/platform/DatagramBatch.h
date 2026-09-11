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

#ifndef FFL_P2P_PLATFORM_DATAGRAM_BATCH_H
#define FFL_P2P_PLATFORM_DATAGRAM_BATCH_H

#include <cstddef>
#include <memory>

namespace ffl::platform {

struct DatagramReceiveBuffer {
    void *data{nullptr};
    size_t dataCapacity{0};
    void *sourceAddress{nullptr};
    size_t sourceAddressCapacity{0};
    size_t dataSize{0};
    size_t sourceAddressSize{0};
    int flags{0};
};

/**
 * Thin platform adapter for receiving multiple UDP datagrams per kernel call.
 *
 * The datapath owns packet buffers and policy. This class owns only the
 * platform-specific message descriptors needed to express one batched receive.
 * In particular, Cosmopolitan's Linux recvmmsg ABI details stay isolated here.
 */
class DatagramBatchReceiver {
public:
    static constexpr size_t MaxBatchSize = 64;

    DatagramBatchReceiver();
    ~DatagramBatchReceiver();

    DatagramBatchReceiver(const DatagramBatchReceiver &) = delete;
    DatagramBatchReceiver &operator=(const DatagramBatchReceiver &) = delete;

    bool isSupported() const;

    int receive(
        int socketHandle, DatagramReceiveBuffer *buffers,
        size_t capacity, int *socketError);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ffl::platform

#endif
