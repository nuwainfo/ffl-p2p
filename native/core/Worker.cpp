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

#include "core/Worker.h"

#include "core/RuntimeConfiguration.h"
#include "quic/Connection.h"

#include <stdexcept>

namespace ffl::core {

namespace {

constexpr size_t kConnectionOperationBudget = 64;
constexpr size_t kWorkerStackSize = 1024 * 1024;
thread_local Worker *currentWorker = nullptr;

} // namespace

Worker::Worker(uint16_t index) : index_(index) {}

Worker::~Worker() {
    stop();
}

void Worker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;

    try {
        thread_.start([this]() { run(); }, kWorkerStackSize);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void Worker::stop() {
    if (!running_.exchange(false))
        return;

    ready_.set();
    thread_.join();
}

void Worker::queueConnection(const std::shared_ptr<quic::Connection> &connection) {
    if (!connection)
        return;

    {
        std::lock_guard<std::mutex> guard(queueMutex_);
        connections_.push_back(connection);
    }

    ready_.set();
}

uint16_t Worker::index() const {
    return index_;
}

#if defined(FFL_P2P_DIAGNOSTICS)
uint64_t Worker::getProcessedConnections() const {
    return processedConnections_.load(std::memory_order_relaxed);
}

uint64_t Worker::getProcessedOperations() const {
    return processedOperations_.load(std::memory_order_relaxed);
}

uint64_t Worker::getTimerExpirations() const {
    return timerExpirations_.load(std::memory_order_relaxed);
}

WorkerDiagnosticsSnapshot Worker::getDiagnosticsSnapshot() const {
    return diagnostics_.getSnapshot();
}

size_t Worker::stackSize() const {
    return thread_.stackSize();
}

#endif

datapath::SendBuffer &Worker::getSendBuffer() {
    return sendBuffer_;
}

datapath::ReceiveBufferPool &Worker::getReceivePool() {
    return receivePool_;
}

datapath::StreamReceiveBufferPool &Worker::getStreamReceivePool() {
    return streamReceivePool_;
}

bool Worker::isCurrentThread() const {
    return currentWorker == this;
}

std::shared_ptr<quic::Connection> Worker::popConnection() {
    std::lock_guard<std::mutex> guard(queueMutex_);
    if (connections_.empty())
        return {};

    auto connection = std::move(connections_.front());
    connections_.pop_front();

    return connection;
}

void Worker::processTimers(platform::Timestamp now) {
    auto expired = timerWheel_.takeExpired(now);
    if (expired.empty())
        return;

#if defined(FFL_P2P_DIAGNOSTICS)
    timerExpirations_.fetch_add(expired.size(), std::memory_order_relaxed);
#endif

    for (auto &connection : expired)
        connection->queueTimerExpired();
}

void Worker::run() {
    currentWorker = this;
    while (running_.load(std::memory_order_acquire)) {
        const platform::Timestamp now = platform::getCurrentTimestampNS();
        if (timerWheel_.getNextExpiry() <= now)
            processTimers(now);

        std::shared_ptr<quic::Connection> connection = popConnection();
        if (connection) {
#if defined(FFL_P2P_DIAGNOSTICS)
            const platform::Timestamp processingStartedAt = diagnostics_.startTiming();
#endif

            connection->beginWorkerProcessing();

#if defined(FFL_P2P_DIAGNOSTICS)
            const size_t processed = connection->processOperations(kConnectionOperationBudget);
            processedConnections_.fetch_add(1, std::memory_order_relaxed);
            processedOperations_.fetch_add(processed, std::memory_order_relaxed);
#else
            connection->processOperations(kConnectionOperationBudget);
#endif

            if (connection->isStopped()) {
                timerWheel_.remove(connection.get());
            } else {
                timerWheel_.update(connection, connection->getNextExpiry());
            }
            connection->finishWorkerProcessing();

#if defined(FFL_P2P_DIAGNOSTICS)
            diagnostics_.recordConnectionRun(
                processed, diagnostics_.elapsedSince(processingStartedAt));
#endif

            continue;
        }

        const platform::Timestamp deadline = timerWheel_.getNextExpiry();

#if defined(FFL_P2P_DIAGNOSTICS)
        const bool eventSignaled = ready_.waitUntil(deadline);
        diagnostics_.recordWake(eventSignaled, deadline, platform::getCurrentTimestampNS());
#else
        ready_.waitUntil(deadline);
#endif
    }

    for (;;) {
        std::shared_ptr<quic::Connection> connection = popConnection();
        if (!connection)
            break;

        connection->failFromRuntime("QUIC worker stopped");
    }
    currentWorker = nullptr;
}

WorkerPool::WorkerPool(uint16_t workerCount) {
    if (workerCount == 0) {
        throw std::invalid_argument("workerCount must be positive");
    }

    workers_.reserve(workerCount);

    for (uint16_t index = 0; index < workerCount; ++index) {
        auto worker = std::make_unique<Worker>(index);
        worker->start();
        workers_.push_back(std::move(worker));
    }
}

WorkerPool::~WorkerPool() {
    for (auto &worker : workers_) {
        worker->stop();
    }
}

Worker &WorkerPool::assignConnection() {
    const uint32_t index = nextWorker_.fetch_add(1, std::memory_order_relaxed);
    return *workers_[index % workers_.size()];
}

uint16_t WorkerPool::size() const {
    return static_cast<uint16_t>(workers_.size());
}

WorkerPool &getRuntimeWorkerPool() {
    static const RuntimeConfiguration configuration;
    static WorkerPool pool(configuration.workerCount());

    return pool;
}

} // namespace ffl::core
