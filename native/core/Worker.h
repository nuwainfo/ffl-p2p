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

#ifndef FFL_P2P_CORE_WORKER_H
#define FFL_P2P_CORE_WORKER_H

#include "core/TimerWheel.h"
#if defined(FFL_P2P_DIAGNOSTICS)
#include "core/WorkerDiagnostics.h"
#endif
#include "datapath/Buffer.h"
#include "platform/Platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace ffl::quic {
class Connection;
}

namespace ffl::core {

class Worker {
public:
    explicit Worker(uint16_t index);
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    ~Worker();

    void start();
    void stop();
    void queueConnection(const std::shared_ptr<quic::Connection> &connection);

    uint16_t index() const;
#if defined(FFL_P2P_DIAGNOSTICS)
    uint64_t getProcessedConnections() const;
    uint64_t getProcessedOperations() const;
    uint64_t getTimerExpirations() const;
    WorkerDiagnosticsSnapshot getDiagnosticsSnapshot() const;
    size_t stackSize() const;
#endif
    datapath::SendBuffer &getSendBuffer();
    datapath::ReceiveBufferPool &getReceivePool();
    datapath::StreamReceiveBufferPool &getStreamReceivePool();
    bool isCurrentThread() const;

private:
    void run();
    std::shared_ptr<quic::Connection> popConnection();
    void processTimers(platform::Timestamp now);

    uint16_t index_{0};
    std::atomic<bool> running_{false};
    platform::Event ready_;
    platform::Thread thread_;
    mutable std::mutex queueMutex_;
    std::deque<std::shared_ptr<quic::Connection>> connections_;
    TimerWheel timerWheel_;
    datapath::SendBuffer sendBuffer_;
    datapath::ReceiveBufferPool receivePool_;
    datapath::StreamReceiveBufferPool streamReceivePool_;
#if defined(FFL_P2P_DIAGNOSTICS)
    std::atomic<uint64_t> processedConnections_{0};
    std::atomic<uint64_t> processedOperations_{0};
    std::atomic<uint64_t> timerExpirations_{0};
    WorkerDiagnostics diagnostics_;
#endif
};

class WorkerPool {
public:
    explicit WorkerPool(uint16_t workerCount);
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    ~WorkerPool();

    Worker &assignConnection();
    uint16_t size() const;

private:
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<uint32_t> nextWorker_{0};
};

WorkerPool &getRuntimeWorkerPool();

} // namespace ffl::core

#endif
