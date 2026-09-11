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

#ifndef FFL_P2P_QUIC_CONNECTION_H
#define FFL_P2P_QUIC_CONNECTION_H

#include "core/Operation.h"
#if defined(FFL_P2P_DIAGNOSTICS)
#include "core/WorkerDiagnostics.h"
#endif
#include "datapath/Buffer.h"
#include "ice/JuiceBinding.h"
#include "platform/Platform.h"
#include "quic/Credentials.h"
#if defined(FFL_P2P_DIAGNOSTICS)
#include "quic/RuntimeDiagnostics.h"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ffl::core {
class Worker;
}

namespace ffl::quic {

enum class Role : uint8_t {
    Client = 0,
    Server = 1,
};

enum class ScheduleState : uint8_t {
    Idle = 0,
    Queued = 1,
    Processing = 2,
};

#if defined(FFL_P2P_DIAGNOSTICS)
struct ConnectionStats {
    uint64_t runtimeVersion{2};
    uint64_t operationsQueued{0};
    uint64_t operationsProcessed{0};
    uint64_t receiveFlushes{0};
    uint64_t receiveDatagrams{0};
    uint64_t maxReceiveBatch{0};
    uint64_t receiveBuffersProcessed{0};
    uint64_t maxReceiveBuffersPerBatch{0};
    uint64_t receivePoolSlabs{0};
    uint64_t receivePoolBuffers{0};
    uint64_t receivePoolAcquires{0};
    uint64_t receivePoolReuses{0};
    uint64_t receivePoolDynamicGrowths{0};
    uint64_t streamReceiveCallbacks{0};
    uint64_t streamReceiveFlushes{0};
    uint64_t streamReceiveBytes{0};
    uint64_t maxStreamReceiveBatchBytes{0};
    uint64_t streamReceiveBuffersProcessed{0};
    uint64_t maxStreamReceiveBuffersPerBatch{0};
    uint64_t streamReceivePoolSlabs{0};
    uint64_t streamReceivePoolBuffers{0};
    uint64_t streamReceivePoolAcquires{0};
    uint64_t streamReceivePoolReuses{0};
    uint64_t streamReceivePoolDynamicGrowths{0};
    uint64_t sendFlushes{0};
    uint64_t sendFlushBudgetRequeues{0};
    uint64_t timerExpirations{0};
    uint64_t workerIndex{0};
    uint64_t workerStackSize{0};
    uint64_t sendBufferSize{0};
    uint64_t txPackets{0};
    uint64_t txBytes{0};
    uint64_t txSendCalls{0};
    uint64_t txSendAttempts{0};
    uint64_t txSendBackpressureEvents{0};
    uint64_t txSendRetryAttempts{0};
    uint64_t txSendBackpressureBytes{0};
    uint64_t txPendingSendBytes{0};
    uint64_t txAggregateSendCalls{0};
    uint64_t txAggregatePackets{0};
    uint64_t txAggregateBytes{0};
    uint64_t maxAggregatePackets{0};
    uint64_t maxAggregateBytes{0};
    uint64_t rxPackets{0};
    uint64_t rxBytes{0};
    uint64_t txNextOffset{0};
    uint64_t txSubmittedOffset{0};
    uint64_t txAckedOffset{0};
    uint64_t rxOffset{0};
    uint64_t txFinRequested{0};
    uint64_t txFinSubmitted{0};
    uint64_t txFinAcked{0};
    uint64_t maxTxPacketSize{0};
    uint64_t maxRxPacketSize{0};
    uint64_t maxTxUDPPayload{0};
    uint64_t sendQuantum{0};
    uint64_t cwnd{0};
    uint64_t bytesInFlight{0};
    uint64_t smoothedRTTNs{0};
    uint64_t packetsLost{0};
    uint64_t bytesLost{0};
    uint64_t packetsDiscarded{0};
};
#endif

class Connection : public std::enable_shared_from_this<Connection> {
public:
    struct ProtocolState;
    static std::shared_ptr<Connection> createClient(std::string certificatePEM);
    static std::shared_ptr<Connection> createServer(const Credentials &credentials);

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection();

    void start(void *agentHandle, bool aggregatePackets, double timeoutSeconds);
    void queueDataAsync(const void *data, size_t size, bool fin);
    void close(double timeoutSeconds);

    std::vector<uint8_t> read();
    bool waitForChange(double timeoutSeconds);

    bool isHandshakeComplete() const;
    bool isHandshakeConfirmed() const;
    bool isPeerFinished() const;
    bool hasPendingWrite() const;
    uint64_t getBufferedWriteBytes() const;
    bool isWriteAcknowledged() const;
    bool isStreamClosed() const;
    bool isStopped() const;
    std::string getError() const;
    
#if defined(FFL_P2P_DIAGNOSTICS)
    ConnectionStats getStats() const;
    core::WorkerDiagnosticsSnapshot getWorkerDiagnosticsSnapshot() const;
    RuntimeDiagnosticsSnapshot getRuntimeDiagnosticsSnapshot() const;
#endif

    /* Datapath producer contract. Never touches ngtcp2. */
    void queueReceive(const void *data, size_t size);

    /* Worker contract. Only Worker calls methods in this group. */
    void beginWorkerProcessing();
    size_t processOperations(size_t maxOperations);
    void finishWorkerProcessing();
    void queueTimerExpired();
    platform::Timestamp getNextExpiry() const;
    void failFromRuntime(const std::string &message);

    /* ngtcp2 callback contract; callbacks execute on the owning Worker. */
    void appendReceivedStreamData(const uint8_t *data, size_t size);
    void markPeerFinished();
    void markHandshakeComplete();
    void markHandshakeConfirmed();
    void markStreamClosed();
    void markAcknowledged();

private:
    enum class TransportSendOutcome : uint8_t {
        Complete = 0,
        Blocked = 1,
    };

    struct PendingSend {
        std::vector<uint8_t> data;
        size_t offset{0};
        size_t segmentSize{0};
        uint64_t packetCount{0};
        uint64_t logicalBytes{0};
        bool aggregate{false};

        bool hasRemainingData() const {
            return offset < data.size();
        }

        size_t getRemainingSize() const {
            return hasRemainingData() ? data.size() - offset : 0;
        }

        void clear() {
            data.clear();
            offset = 0;
            segmentSize = 0;
            packetCount = 0;
            logicalBytes = 0;
            aggregate = false;
        }
    };

    Connection(Role role, std::string certificatePEM, std::string privateKeyPEM);

    void queueOperation(core::Operation operation);
    void createSendOperation(const void *data, size_t size, bool fin);
    void queueInternalOperation(core::OperationType type);
    void queueStreamReceiveFlush();
    void queueSendFlush();
    void scheduleIfIdle();
    void notifyApplication();
    void setError(const std::string &message);
    void completeOperation(const core::Operation &operation, const std::string &error = {});
    void drainFailedOperations(const std::string &error);

    void assertWorkerOwner() const;
    void processOperation(core::Operation &operation);
    void processStart();
    void processSend(core::Operation &operation);
    void processReceive();
    void processStreamReceive();
    void processSendFlush();
    void processTimer();
    void processShutdown();

    void initializeClient();
    void initializeServer(const uint8_t *initialPacket, size_t initialPacketLength);
    void feedPacket(const uint8_t *packet, size_t packetLength);
    bool flushPackets();
    TransportSendOutcome sendTransportBuffer(const uint8_t *data, size_t size,
                                             size_t segmentSize, uint64_t packetCount,
                                             bool aggregate);
    TransportSendOutcome flushPendingTransportSend();
    ice::SendResult sendTransportOnce(const uint8_t *data, size_t size,
                                      size_t segmentSize, bool aggregate, bool retry);
                                      
#if defined(FFL_P2P_DIAGNOSTICS)
    void recordCompletedTransportSend(uint64_t packetCount, uint64_t byteCount, bool aggregate);
#endif

    void retainPendingTransportSend(const uint8_t *data, size_t size, size_t sentSize,
                                    size_t segmentSize, uint64_t packetCount, bool aggregate);
    void scheduleSendRetry();
    void clearSendRetry();
    
#if defined(FFL_P2P_DIAGNOSTICS)
    void recordSendBackpressure(size_t deferredBytes);
    ConnectionStats collectDiagnosticsSnapshot() const;
#endif

    void publishApplicationState();

    Role role_;
    std::string certificatePEM_;
    std::string privateKeyPEM_;
    std::unique_ptr<ProtocolState> protocol_;

    core::Worker *worker_{nullptr};
    
#if defined(FFL_P2P_DIAGNOSTICS)
    RuntimeDiagnostics runtimeDiagnostics_;
#endif

    core::OperationQueue operationQueue_;
    std::atomic<ScheduleState> scheduleState_{ScheduleState::Idle};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> aggregatePackets_{true};
    ice::JuiceBinding binding_;

    mutable std::mutex receiveMutex_;
    datapath::ReceiveBatch receiveBatch_;
    datapath::StreamReceiveBatch streamReceiveBatch_;
    bool receiveOperationPending_{false};
    bool streamReceiveOperationPending_{false};
    bool peerFinishedPending_{false};
    bool sendOperationPending_{false};
    PendingSend pendingSend_;
    platform::Timestamp sendRetryExpiry_{platform::InfiniteTimestamp};

    mutable std::mutex appMutex_;
    std::condition_variable appCondition_;
    uint64_t appGeneration_{0};
    datapath::StreamReceiveBatch appReceiveBatch_;
    std::string appError_;
    bool appHandshakeComplete_{false};
    bool appHandshakeConfirmed_{false};
    bool appPeerFinished_{false};
    uint64_t appSubmittedWriteOffset_{0};
    uint64_t appAckedWriteOffset_{0};
    bool appFinSubmitted_{false};
    bool appFinAcked_{false};
    bool appStreamClosed_{false};
    
#if defined(FFL_P2P_DIAGNOSTICS)
    ConnectionStats appStats_;
#endif

    std::atomic<uint64_t> applicationWriteOffset_{0};
    std::atomic<bool> applicationFinRequested_{false};

#if defined(FFL_P2P_DIAGNOSTICS)
    std::atomic<uint64_t> operationsQueued_{0};
    std::atomic<uint64_t> operationsProcessed_{0};
    std::atomic<uint64_t> receiveFlushes_{0};
    std::atomic<uint64_t> receiveDatagrams_{0};
    std::atomic<uint64_t> maxReceiveBatch_{0};
    std::atomic<uint64_t> receiveBuffersProcessed_{0};
    std::atomic<uint64_t> maxReceiveBuffersPerBatch_{0};
    std::atomic<uint64_t> streamReceiveCallbacks_{0};
    std::atomic<uint64_t> streamReceiveFlushes_{0};
    std::atomic<uint64_t> streamReceiveBytes_{0};
    std::atomic<uint64_t> maxStreamReceiveBatchBytes_{0};
    std::atomic<uint64_t> streamReceiveBuffersProcessed_{0};
    std::atomic<uint64_t> maxStreamReceiveBuffersPerBatch_{0};
    std::atomic<uint64_t> sendFlushes_{0};
    std::atomic<uint64_t> sendFlushBudgetRequeues_{0};
    std::atomic<uint64_t> txSendAttempts_{0};
    std::atomic<uint64_t> txSendBackpressureEvents_{0};
    std::atomic<uint64_t> txSendRetryAttempts_{0};
    std::atomic<uint64_t> txSendBackpressureBytes_{0};
    std::atomic<uint64_t> timerExpirations_{0};
#endif
};

} // namespace ffl::quic

#endif
