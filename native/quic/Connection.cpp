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

#include "quic/Connection.h"

#include "core/Worker.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ffl::quic {

namespace {

constexpr char kALPN[] = "ffl-p2p/1";
constexpr char kServerName[] = "ffl-p2p";
constexpr size_t kCIDLength = 18;
constexpr uint64_t kInitialMaxData = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kInitialMaxStreamData = 16ULL * 1024ULL * 1024ULL;
constexpr int kMaxPacketsPerFlush = 512;
constexpr platform::Timestamp kSendRetryDelayNS = 1000ULL * 1000ULL;

void checkGnuTLS(int result, const char *operation) {
    if (result < 0)
        throw std::runtime_error(std::string(operation) + ": " + gnutls_strerror(result));
}

void checkNgtcp2(int result, const char *operation) {
    if (result != 0)
        throw std::runtime_error(std::string(operation) + ": " + ngtcp2_strerror(result));
}

#if defined(FFL_P2P_DIAGNOSTICS)
void updateMaximum(std::atomic<uint64_t> &target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

#endif
} // namespace

struct Connection::ProtocolState {
    Connection *owner{nullptr};
    bool server{false};
    ngtcp2_conn *conn{nullptr};
    gnutls_session_t tls{nullptr};
    gnutls_certificate_credentials_t credentials{nullptr};
    ngtcp2_crypto_conn_ref connRef{};

    platform::SocketAddress local;
    platform::SocketAddress peer;

    bool handshakeComplete{false};
    bool handshakeConfirmed{false};
    bool peerFinished{false};

    int64_t txStreamID{-1};
    int64_t rxStreamID{-1};
    uint64_t rxOffset{0};

    struct TxChunk {
        uint64_t offset{0};
        std::vector<uint8_t> data;
        size_t submitted{0};
    };
    std::deque<TxChunk> txChunks;
    uint64_t txNextOffset{0};
    uint64_t txSubmittedOffset{0};
    uint64_t txAckedOffset{0};
    bool txFinRequested{false};
    bool txFinSubmitted{false};
    bool txFinAcked{false};
    bool streamClosed{false};

    uint64_t aggregatePacketCount{0};
    
#if defined(FFL_P2P_DIAGNOSTICS)
    uint64_t txPacketCount{0};
    uint64_t txPacketBytes{0};
    uint64_t txSendCalls{0};
    uint64_t txAggregateSendCalls{0};
    uint64_t txAggregatePackets{0};
    uint64_t txAggregateBytes{0};
    size_t maxAggregatePackets{0};
    size_t maxAggregateBytes{0};
    uint64_t rxPacketCount{0};
    uint64_t rxPacketBytes{0};
    size_t maxTxPacketSize{0};
    size_t maxRxPacketSize{0};
#endif

    std::string aggregateWriteError;

    void releaseTransport() {
        if (conn) {
            ngtcp2_conn_del(conn);
            conn = nullptr;
        }

        if (tls) {
            gnutls_session_set_ptr(tls, nullptr);
            gnutls_deinit(tls);
            tls = nullptr;
        }

        if (credentials) {
            gnutls_certificate_free_credentials(credentials);
            credentials = nullptr;
        }
    }

    ~ProtocolState() = default;
};

namespace {

ngtcp2_conn *getConnCallback(ngtcp2_crypto_conn_ref *reference) {
    auto *state = static_cast<Connection::ProtocolState *>(reference->user_data);
    return state->conn;
}

void randomCallback(uint8_t *destination, size_t length, const ngtcp2_rand_ctx *) {
    if (gnutls_rnd(GNUTLS_RND_NONCE, destination, length) < 0)
        std::abort();
}

int newCIDCallback(ngtcp2_conn *, ngtcp2_cid *cid,
                   ngtcp2_stateless_reset_token *token, size_t cidLength, void *) {
    if (cidLength > sizeof(cid->data))
        return NGTCP2_ERR_CALLBACK_FAILURE;

    cid->datalen = cidLength;
    if (gnutls_rnd(GNUTLS_RND_RANDOM, cid->data, cidLength) < 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    if (gnutls_rnd(GNUTLS_RND_RANDOM, token->data, sizeof(token->data)) < 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    return 0;
}

int handshakeCompletedCallback(ngtcp2_conn *, void *userData) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    state->handshakeComplete = true;
    state->owner->markHandshakeComplete();
    return 0;
}

int handshakeConfirmedCallback(ngtcp2_conn *, void *userData) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    state->handshakeConfirmed = true;
    state->owner->markHandshakeConfirmed();
    return 0;
}

int receiveStreamDataCallback(ngtcp2_conn *conn, uint32_t flags, int64_t streamID,
                              uint64_t offset, const uint8_t *data, size_t dataLength,
                              void *userData, void *) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    if (state->rxStreamID == -1)
        state->rxStreamID = streamID;

    if (state->rxStreamID != streamID || offset != state->rxOffset)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    if (dataLength) {
        state->owner->appendReceivedStreamData(data, dataLength);
        state->rxOffset += dataLength;

        if (ngtcp2_conn_extend_max_stream_offset(conn, streamID, dataLength) != 0)
            return NGTCP2_ERR_CALLBACK_FAILURE;

        ngtcp2_conn_extend_max_offset(conn, dataLength);
    }

    if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
        state->peerFinished = true;
        state->owner->markPeerFinished();
    }

    return 0;
}

int acknowledgedStreamDataCallback(ngtcp2_conn *, int64_t streamID,
                                   uint64_t offset, uint64_t dataLength,
                                   void *userData, void *) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    if (state->txStreamID != -1 && streamID != state->txStreamID)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    if (dataLength == 0) {
        if (!state->txFinSubmitted || offset != state->txNextOffset)
            return NGTCP2_ERR_CALLBACK_FAILURE;

        state->txFinAcked = true;
        state->owner->markAcknowledged();
        return 0;
    }

    if (offset != state->txAckedOffset)
        return NGTCP2_ERR_CALLBACK_FAILURE;

    state->txAckedOffset = offset + dataLength;

    while (!state->txChunks.empty()) {
        const auto &chunk = state->txChunks.front();
        const uint64_t chunkEnd = chunk.offset + chunk.data.size();

        if (chunkEnd > state->txAckedOffset)
            break;

        state->txChunks.pop_front();
    }
    state->owner->markAcknowledged();

    return 0;
}

int streamCloseCallback(ngtcp2_conn *, uint32_t flags, int64_t streamID,
                        uint64_t, void *userData, void *) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    if (state->rxStreamID != -1 && streamID != state->rxStreamID &&
        state->txStreamID != -1 && streamID != state->txStreamID)
        return 0;

    if ((flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) == 0) {
        state->streamClosed = true;

        if (state->txFinRequested)
            state->txFinAcked = true;

        state->owner->markStreamClosed();
    }

    return 0;
}

ngtcp2_callbacks makeCallbacks(bool server) {
    ngtcp2_callbacks callbacks{};
    if (server) {
        callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    } else {
        callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
        callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    }

    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed = handshakeCompletedCallback;
    callbacks.handshake_confirmed = handshakeConfirmedCallback;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_stream_data = receiveStreamDataCallback;
    callbacks.acked_stream_data_offset = acknowledgedStreamDataCallback;
    callbacks.stream_close = streamCloseCallback;
    callbacks.rand = randomCallback;
    callbacks.get_new_connection_id2 = newCIDCallback;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;

    return callbacks;
}

ngtcp2_cid generateRandomCID() {
    ngtcp2_cid cid{};
    cid.datalen = kCIDLength;
    checkGnuTLS(gnutls_rnd(GNUTLS_RND_RANDOM, cid.data, cid.datalen), "gnutls_rnd(cid)");
    return cid;
}

void initializeFixedPath(Connection::ProtocolState &state) {
    state.local = platform::createLoopbackIPv4Address(state.server ? 4434 : 4433);
    state.peer = platform::createLoopbackIPv4Address(state.server ? 4433 : 4434);
}

void initializeTLS(Connection::ProtocolState &state, const std::string &certificatePEM,
                   const std::string *privateKeyPEM) {
    ensureGnuTLS();
    checkGnuTLS(gnutls_certificate_allocate_credentials(&state.credentials),
                "gnutls_certificate_allocate_credentials");

    gnutls_datum_t certificateDatum{
        reinterpret_cast<unsigned char *>(const_cast<char *>(certificatePEM.data())),
        static_cast<unsigned int>(certificatePEM.size())};

    if (state.server) {
        if (!privateKeyPEM)
            throw std::runtime_error("server private key is missing");

        gnutls_datum_t keyDatum{
            reinterpret_cast<unsigned char *>(const_cast<char *>(privateKeyPEM->data())),
            static_cast<unsigned int>(privateKeyPEM->size())};

        checkGnuTLS(gnutls_certificate_set_x509_key_mem(
                        state.credentials, &certificateDatum, &keyDatum, GNUTLS_X509_FMT_PEM),
                    "gnutls_certificate_set_x509_key_mem");
    } else {
        const int added = gnutls_certificate_set_x509_trust_mem(
            state.credentials, &certificateDatum, GNUTLS_X509_FMT_PEM);

        if (added <= 0)
            throw std::runtime_error("failed to install pinned QUIC certificate");
    }

    unsigned flags = state.server ? GNUTLS_SERVER : GNUTLS_CLIENT;
    flags |= GNUTLS_ENABLE_EARLY_DATA | GNUTLS_NO_END_OF_EARLY_DATA;
    if (state.server)
        flags |= GNUTLS_NO_AUTO_SEND_TICKET;

    checkGnuTLS(gnutls_init(&state.tls, flags), "gnutls_init");

    const char *errorPosition = nullptr;
    checkGnuTLS(gnutls_priority_set_direct(
                    state.tls,
                    "%DISABLE_TLS13_COMPAT_MODE:NORMAL:-VERS-ALL:+VERS-TLS1.3",
                    &errorPosition),
                "gnutls_priority_set_direct");

    const int configureResult = state.server
        ? ngtcp2_crypto_gnutls_configure_server_session(state.tls)
        : ngtcp2_crypto_gnutls_configure_client_session(state.tls);

    if (configureResult != 0)
        throw std::runtime_error("ngtcp2_crypto_gnutls_configure_*_session failed");

    state.connRef.get_conn = getConnCallback;
    state.connRef.user_data = &state;
    gnutls_session_set_ptr(state.tls, &state.connRef);
    checkGnuTLS(gnutls_credentials_set(state.tls, GNUTLS_CRD_CERTIFICATE, state.credentials),
                "gnutls_credentials_set");

    gnutls_datum_t alpn{
        reinterpret_cast<unsigned char *>(const_cast<char *>(kALPN)),
        static_cast<unsigned int>(sizeof(kALPN) - 1)};

    const unsigned alpnFlags = state.server
        ? (GNUTLS_ALPN_MANDATORY | GNUTLS_ALPN_SERVER_PRECEDENCE)
        : 0;

    checkGnuTLS(gnutls_alpn_set_protocols(state.tls, &alpn, 1, alpnFlags),
                "gnutls_alpn_set_protocols");

    if (!state.server) {
        checkGnuTLS(gnutls_server_name_set(
                        state.tls, GNUTLS_NAME_DNS, kServerName,
                        static_cast<unsigned int>(sizeof(kServerName) - 1)),
                    "gnutls_server_name_set");
        gnutls_session_set_verify_cert(state.tls, kServerName, 0);
    }
}

void initializeQUIC(Connection::ProtocolState &state, const ngtcp2_cid &dcid,
                    const ngtcp2_cid &scid, const ngtcp2_cid *originalDCID) {
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = platform::getCurrentTimestampNS();

    ngtcp2_transport_params parameters;
    ngtcp2_transport_params_default(&parameters);

    parameters.initial_max_data = kInitialMaxData;
    parameters.initial_max_stream_data_bidi_local = kInitialMaxStreamData;
    parameters.initial_max_stream_data_bidi_remote = kInitialMaxStreamData;
    parameters.initial_max_stream_data_uni = kInitialMaxStreamData;
    parameters.initial_max_streams_bidi = 4;
    parameters.initial_max_streams_uni = 2;

    if (state.server && originalDCID) {
        parameters.original_dcid = *originalDCID;
        parameters.original_dcid_present = 1;
    }

    ngtcp2_path_storage pathStorage;
    ngtcp2_path_storage_init(
        &pathStorage,
        reinterpret_cast<const ngtcp2_sockaddr *>(state.local.data()),
        static_cast<ngtcp2_socklen>(state.local.size()),
        reinterpret_cast<const ngtcp2_sockaddr *>(state.peer.data()),
        static_cast<ngtcp2_socklen>(state.peer.size()), nullptr);

    const auto callbacks = makeCallbacks(state.server);
    int result;
    if (state.server) {
        result = ngtcp2_conn_server_new(&state.conn, &dcid, &scid, &pathStorage.path,
                                        NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                                        &parameters, nullptr, &state);
    } else {
        result = ngtcp2_conn_client_new(&state.conn, &dcid, &scid, &pathStorage.path,
                                        NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                                        &parameters, nullptr, &state);
    }

    checkNgtcp2(result, state.server ? "ngtcp2_conn_server_new" : "ngtcp2_conn_client_new");
    ngtcp2_conn_set_tls_native_handle(state.conn, state.tls);
}

bool hasPendingWrite(const Connection::ProtocolState &state) {
    return state.txStreamID >= 0 &&
        (state.txSubmittedOffset < state.txNextOffset ||
         (state.txFinRequested && !state.txFinSubmitted));
}

Connection::ProtocolState::TxChunk *nextTransmitChunk(Connection::ProtocolState &state) {
    for (auto &chunk : state.txChunks) {
        const uint64_t submittedEnd = chunk.offset + chunk.submitted;

        if (submittedEnd == state.txSubmittedOffset && chunk.submitted < chunk.data.size())
            return &chunk;
    }
    return nullptr;
}

ngtcp2_ssize writeOnePacket(Connection::ProtocolState &state, ngtcp2_path *path,
                            ngtcp2_pkt_info *packetInfo, uint8_t *output,
                            size_t outputSize, ngtcp2_tstamp timestamp,
                            bool padToDatagramSize) {
    ngtcp2_ssize written = 0;

    if (hasPendingWrite(state)) {
        ngtcp2_ssize accepted = -1;
        auto *chunk = nextTransmitChunk(state);
        const bool allQueuedDataSubmitted = state.txSubmittedOffset == state.txNextOffset;
        const bool sendingFin = state.txFinRequested && !state.txFinSubmitted &&
                                (allQueuedDataSubmitted ||
                                 (chunk && state.txSubmittedOffset +
                                  (chunk->data.size() - chunk->submitted) == state.txNextOffset));
        uint32_t flags = padToDatagramSize ? NGTCP2_WRITE_STREAM_FLAG_PADDING
                                           : NGTCP2_WRITE_STREAM_FLAG_NONE;

        if (sendingFin)
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

        const uint8_t *data = chunk ? chunk->data.data() + chunk->submitted : nullptr;
        const size_t remaining = chunk ? chunk->data.size() - chunk->submitted : 0;

        written = ngtcp2_conn_write_stream(
            state.conn, path, packetInfo, output, outputSize, &accepted, flags,
            state.txStreamID, data, remaining, timestamp);

        if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
            written == NGTCP2_ERR_STREAM_SHUT_WR ||
            written == NGTCP2_ERR_STREAM_ID_BLOCKED)
            return 0;

        if (written < 0)
            throw std::runtime_error(std::string("ngtcp2_conn_write_stream: ") +
                                     ngtcp2_strerror(static_cast<int>(written)));

        if (accepted > 0 && chunk) {
            chunk->submitted += static_cast<size_t>(accepted);
            state.txSubmittedOffset += static_cast<uint64_t>(accepted);
        }

        const bool allSubmittedNow = state.txSubmittedOffset == state.txNextOffset;
        if (sendingFin && written > 0 && allSubmittedNow && accepted >= 0)
            state.txFinSubmitted = true;
    } else {
        written = ngtcp2_conn_write_pkt(
            state.conn, path, packetInfo, output, outputSize, timestamp);

        if (written < 0)
            throw std::runtime_error(std::string("ngtcp2_conn_write_pkt: ") +
                                     ngtcp2_strerror(static_cast<int>(written)));
    }

    if (written > 0) {
        if (padToDatagramSize)
            ++state.aggregatePacketCount;
        
#if defined(FFL_P2P_DIAGNOSTICS)
        const size_t packetSize = static_cast<size_t>(written);
        ++state.txPacketCount;
        state.txPacketBytes += packetSize;
        state.maxTxPacketSize = (std::max)(state.maxTxPacketSize, packetSize);
#endif
    }

    return written;
}

ngtcp2_ssize writeAggregatePacketCallback(ngtcp2_conn *, ngtcp2_path *path,
                                          ngtcp2_pkt_info *packetInfo, uint8_t *output,
                                          size_t outputSize, ngtcp2_tstamp timestamp,
                                          void *userData) {
    auto *state = static_cast<Connection::ProtocolState *>(userData);

    try {
        return writeOnePacket(*state, path, packetInfo, output, outputSize, timestamp, true);
    } catch (const std::exception &error) {
        state->aggregateWriteError = error.what();
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

} // namespace

std::shared_ptr<Connection> Connection::createClient(std::string certificatePEM) {
    return std::shared_ptr<Connection>(
        new Connection(Role::Client, std::move(certificatePEM), {}));
}

std::shared_ptr<Connection> Connection::createServer(const Credentials &credentials) {
    return std::shared_ptr<Connection>(
        new Connection(Role::Server, credentials.certificatePEM, credentials.privateKeyPEM));
}

Connection::Connection(Role role, std::string certificatePEM, std::string privateKeyPEM)
    : role_(role),
      certificatePEM_(std::move(certificatePEM)),
      privateKeyPEM_(std::move(privateKeyPEM)),
      protocol_(std::make_unique<ProtocolState>()),
      worker_(&core::getRuntimeWorkerPool().assignConnection()),
      receiveBatch_(worker_->getReceivePool()),
      streamReceiveBatch_(worker_->getStreamReceivePool()),
      appReceiveBatch_(worker_->getStreamReceivePool()) {
    protocol_->owner = this;
    protocol_->server = role_ == Role::Server;
    
#if defined(FFL_P2P_DIAGNOSTICS)
    appStats_.workerIndex = worker_->index();
#endif
}

Connection::~Connection() {
    binding_.detach();
}

void Connection::start(void *agentHandle, bool aggregatePackets, double timeoutSeconds) {
    if (!agentHandle)
        throw std::invalid_argument("QUIC runtime requires an ICE agent");

    if (started_.load(std::memory_order_acquire))
        throw std::runtime_error("QUIC runtime is already started");

    aggregatePackets_.store(aggregatePackets, std::memory_order_relaxed);
    binding_.attach(agentHandle, this);

    core::Operation operation;
    operation.type = core::OperationType::APIStart;
    operation.completion = std::make_shared<core::Completion>();
    const auto completion = operation.completion;
    queueOperation(std::move(operation));

    std::string operationError;
    if (!completion->wait(timeoutSeconds, operationError)) {
        binding_.detach();
        throw std::runtime_error("QUIC runtime start timed out");
    }

    if (!operationError.empty()) {
        binding_.detach();
        throw std::runtime_error(operationError);
    }
}

void Connection::createSendOperation(const void *data, size_t size, bool fin) {
    if (!started_.load(std::memory_order_acquire))
        throw std::runtime_error("QUIC runtime is not started");

    if (stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("QUIC runtime is stopped");

    core::Operation operation;
    operation.type = core::OperationType::APISend;
    operation.fin = fin;

    if (size) {
        if (!data)
            throw std::invalid_argument("QUIC send data is null");

        const auto *bytes = static_cast<const uint8_t *>(data);
        operation.data.assign(bytes, bytes + size);
        applicationWriteOffset_.fetch_add(static_cast<uint64_t>(size), std::memory_order_release);
    }

    if (fin)
        applicationFinRequested_.store(true, std::memory_order_release);

    queueOperation(std::move(operation));
}

void Connection::queueDataAsync(const void *data, size_t size, bool fin) {
    createSendOperation(data, size, fin);
}

void Connection::close(double timeoutSeconds) {
    if (stopped_.load(std::memory_order_acquire)) {
        binding_.detach();
        return;
    }

    core::Operation operation;
    operation.type = core::OperationType::Shutdown;
    operation.completion = std::make_shared<core::Completion>();
    const auto completion = operation.completion;
    queueOperation(std::move(operation));

    std::string operationError;
    const bool completed = completion->wait(timeoutSeconds, operationError);
    binding_.detach();

    if (!completed)
        throw std::runtime_error("QUIC runtime shutdown timed out");

    if (!operationError.empty())
        throw std::runtime_error(operationError);
}

datapath::StreamReceiveBatch Connection::takeReceivedStreamData() {
    datapath::StreamReceiveBatch batch(worker_->getStreamReceivePool());
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appReceiveBatch_.swap(batch);
    }
    return batch;
}

bool Connection::waitForChange(double timeoutSeconds) {
    std::unique_lock<std::mutex> lock(appMutex_);
    const uint64_t generation = appGeneration_;

    if (timeoutSeconds < 0.0) {
        appCondition_.wait(lock, [this, generation]() {
            return appGeneration_ != generation;
        });

        return true;
    }

    return appCondition_.wait_for(
        lock, std::chrono::duration<double>(timeoutSeconds),
        [this, generation]() { return appGeneration_ != generation; });
}

bool Connection::isHandshakeComplete() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appHandshakeComplete_;
}

bool Connection::isHandshakeConfirmed() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appHandshakeConfirmed_;
}

bool Connection::isPeerFinished() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appPeerFinished_;
}

bool Connection::hasPendingWrite() const {
    const uint64_t applicationOffset = applicationWriteOffset_.load(std::memory_order_acquire);
    const bool finRequested = applicationFinRequested_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> guard(appMutex_);
    return appSubmittedWriteOffset_ < applicationOffset ||
        (finRequested && !appFinSubmitted_);
}

uint64_t Connection::getBufferedWriteBytes() const {
    const uint64_t applicationOffset = applicationWriteOffset_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> guard(appMutex_);
    return applicationOffset >= appAckedWriteOffset_
        ? applicationOffset - appAckedWriteOffset_
        : 0;
}

bool Connection::isWriteAcknowledged() const {
    const uint64_t applicationOffset = applicationWriteOffset_.load(std::memory_order_acquire);
    const bool finRequested = applicationFinRequested_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> guard(appMutex_);
    if (appAckedWriteOffset_ != applicationOffset)
        return false;

    return !finRequested || appFinAcked_;
}

bool Connection::isStreamClosed() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appStreamClosed_;
}

bool Connection::isStopped() const {
    return stopped_.load(std::memory_order_acquire);
}

uint16_t Connection::workerIndex() const {
    return worker_->index();
}

std::string Connection::getError() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appError_;
}

#if defined(FFL_P2P_DIAGNOSTICS)
ConnectionStats Connection::getStats() const {
    std::lock_guard<std::mutex> guard(appMutex_);
    return appStats_;
}

core::WorkerDiagnosticsSnapshot Connection::getWorkerDiagnosticsSnapshot() const {
    return worker_->getDiagnosticsSnapshot();
}

RuntimeDiagnosticsSnapshot Connection::getRuntimeDiagnosticsSnapshot() const {
    return runtimeDiagnostics_.getSnapshot();
}

#endif

void Connection::queueReceive(const void *data, size_t size) {
    if (!data || size == 0 || stopped_.load(std::memory_order_acquire))
        return;

    bool queueFlush = false;
    {
        std::lock_guard<std::mutex> guard(receiveMutex_);
        if (stopped_.load(std::memory_order_relaxed))
            return;

        receiveBatch_.append(data, size);

        if (!receiveOperationPending_) {
            receiveOperationPending_ = true;
            queueFlush = true;
        }
    }

    if (queueFlush)
        queueInternalOperation(core::OperationType::FlushRecv);
}

void Connection::beginWorkerProcessing() {
    assertWorkerOwner();
    
#if defined(FFL_P2P_DIAGNOSTICS)
    runtimeDiagnostics_.recordWorkerStarted(platform::getCurrentTimestampNS());
#endif

    scheduleState_.store(ScheduleState::Processing, std::memory_order_release);
}

size_t Connection::processOperations(size_t maxOperations) {
    assertWorkerOwner();
    size_t processed = 0;
    core::Operation operation;

    while (processed < maxOperations && operationQueue_.pop(operation)) {
        try {
            processOperation(operation);
            completeOperation(operation);
        } catch (const std::exception &exception) {
            const std::string message = exception.what();
            completeOperation(operation, message);
            setError(message);
            protocol_->releaseTransport();
            stopped_.store(true, std::memory_order_release);
            drainFailedOperations(message);
            break;
        }

        ++processed;

#if defined(FFL_P2P_DIAGNOSTICS)
        operationsProcessed_.fetch_add(1, std::memory_order_relaxed);
#endif

        if (stopped_.load(std::memory_order_acquire)) {
            drainFailedOperations("QUIC runtime is stopped");
            break;
        }
    }

    publishApplicationState();

    return processed;
}

void Connection::finishWorkerProcessing() {
    assertWorkerOwner();
    scheduleState_.store(ScheduleState::Idle, std::memory_order_release);
    if (!operationQueue_.empty())
        scheduleIfIdle();
}

void Connection::queueTimerExpired() {
    if (!stopped_.load(std::memory_order_acquire))
        queueInternalOperation(core::OperationType::TimerExpired);
}

platform::Timestamp Connection::getNextExpiry() const {
    assertWorkerOwner();

    if (stopped_.load(std::memory_order_acquire))
        return platform::InfiniteTimestamp;

    const platform::Timestamp protocolExpiry = protocol_->conn
        ? ngtcp2_conn_get_expiry2(protocol_->conn)
        : platform::InfiniteTimestamp;

    return (std::min)(protocolExpiry, sendRetryExpiry_);
}

void Connection::failFromRuntime(const std::string &message) {
    assertWorkerOwner();
    setError(message);

    protocol_->releaseTransport();
    stopped_.store(true, std::memory_order_release);
    drainFailedOperations(message);
}

void Connection::queueOperation(core::Operation operation) {
    if (stopped_.load(std::memory_order_acquire) && operation.type != core::OperationType::Shutdown) {
        if (operation.completion)
            operation.completion->complete("QUIC runtime is stopped");

        return;
    }

    operationQueue_.push(std::move(operation));
    
#if defined(FFL_P2P_DIAGNOSTICS)
    operationsQueued_.fetch_add(1, std::memory_order_relaxed);
#endif

    scheduleIfIdle();
}

void Connection::queueInternalOperation(core::OperationType type) {
    core::Operation operation;
    operation.type = type;
    queueOperation(std::move(operation));
}

void Connection::queueStreamReceiveFlush() {
    assertWorkerOwner();

    if (streamReceiveOperationPending_)
        return;

    streamReceiveOperationPending_ = true;
    queueInternalOperation(core::OperationType::FlushStreamRecv);
}

void Connection::queueSendFlush() {
    assertWorkerOwner();
    if (sendOperationPending_)
        return;

    sendOperationPending_ = true;
    queueInternalOperation(core::OperationType::FlushSend);
}

void Connection::scheduleIfIdle() {
    ScheduleState expected = ScheduleState::Idle;
    if (!scheduleState_.compare_exchange_strong(
            expected, ScheduleState::Queued, std::memory_order_acq_rel))
        return;

#if defined(FFL_P2P_DIAGNOSTICS)
    runtimeDiagnostics_.recordScheduled(platform::getCurrentTimestampNS());
#endif

    worker_->queueConnection(shared_from_this());
}

void Connection::notifyApplication() {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::setError(const std::string &message) {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        if (appError_.empty())
            appError_ = message;

        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::completeOperation(const core::Operation &operation, const std::string &error) {
    if (operation.completion)
        operation.completion->complete(error);
}

void Connection::drainFailedOperations(const std::string &error) {
    core::Operation pending;
    while (operationQueue_.pop(pending))
        completeOperation(pending, error);
}

void Connection::assertWorkerOwner() const {
    if (!worker_ || !worker_->isCurrentThread())
        throw std::logic_error("QUIC connection state accessed outside its owning Worker");
}

void Connection::processOperation(core::Operation &operation) {
    switch (operation.type) {
    case core::OperationType::APIStart:
        processStart();
        break;
    case core::OperationType::APISend:
        processSend(operation);
        break;
    case core::OperationType::FlushRecv:
        processReceive();
        break;
    case core::OperationType::FlushStreamRecv:
        streamReceiveOperationPending_ = false;
        processStreamReceive();
        break;
    case core::OperationType::FlushSend:
        sendOperationPending_ = false;
        processSendFlush();
        break;
    case core::OperationType::TimerExpired:
        processTimer();
        break;
    case core::OperationType::Shutdown:
        processShutdown();
        break;
    }
}

void Connection::processStart() {
    if (started_.exchange(true, std::memory_order_acq_rel))
        throw std::runtime_error("QUIC runtime was started more than once");

    if (role_ == Role::Client) {
        initializeClient();
        queueSendFlush();
    }

    notifyApplication();
}

void Connection::processSend(core::Operation &operation) {
    if (!protocol_->conn)
        throw std::runtime_error("QUIC connection is not initialized");

    if (protocol_->txFinRequested)
        throw std::runtime_error("QUIC stream send side is already finishing");

    if (protocol_->txStreamID < 0) {
        if (protocol_->server) {
            if (protocol_->rxStreamID < 0)
                throw std::runtime_error("server has not received a client stream yet");

            protocol_->txStreamID = protocol_->rxStreamID;
        } else {
            int64_t streamID = -1;
            const int result = ngtcp2_conn_open_bidi_stream(protocol_->conn, &streamID, nullptr);

            if (result == NGTCP2_ERR_STREAM_ID_BLOCKED)
                throw std::runtime_error("QUIC bidirectional stream is not available yet");

            checkNgtcp2(result, "ngtcp2_conn_open_bidi_stream");
            protocol_->txStreamID = streamID;
        }
    }

    if (!operation.data.empty()) {
        ProtocolState::TxChunk chunk;
        chunk.offset = protocol_->txNextOffset;
        chunk.data = std::move(operation.data);

        protocol_->txNextOffset += chunk.data.size();
        protocol_->txChunks.push_back(std::move(chunk));
    }

    if (operation.fin)
        protocol_->txFinRequested = true;

    queueSendFlush();
    publishApplicationState();
}

void Connection::processReceive() {
    
#if defined(FFL_P2P_DIAGNOSTICS)
    const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
    receiveFlushes_.fetch_add(1, std::memory_order_relaxed);
#endif

    datapath::ReceiveBatch batch(worker_->getReceivePool());

    {
        std::lock_guard<std::mutex> guard(receiveMutex_);
        receiveBatch_.swap(batch);
        receiveOperationPending_ = false;
    }

    const uint64_t batchCount = static_cast<uint64_t>(batch.size());
    
#if defined(FFL_P2P_DIAGNOSTICS)
    const uint64_t bufferCount = static_cast<uint64_t>(batch.getBufferCount());
#endif

    if (batchCount != 0) {
        batch.forEachPacket([this](const uint8_t *data, size_t size) {
            feedPacket(data, size);
        });

#if defined(FFL_P2P_DIAGNOSTICS)
        receiveDatagrams_.fetch_add(batchCount, std::memory_order_relaxed);
        receiveBuffersProcessed_.fetch_add(bufferCount, std::memory_order_relaxed);
        updateMaximum(maxReceiveBatch_, batchCount);
        updateMaximum(maxReceiveBuffersPerBatch_, bufferCount);
#endif

        queueSendFlush();
    }

    bool queueNextFlush = false;
    {
        std::lock_guard<std::mutex> guard(receiveMutex_);
        if (!receiveBatch_.empty() && !receiveOperationPending_) {
            receiveOperationPending_ = true;
            queueNextFlush = true;
        }
    }

    if (queueNextFlush)
        queueInternalOperation(core::OperationType::FlushRecv);

#if defined(FFL_P2P_DIAGNOSTICS)
    runtimeDiagnostics_.recordReceiveFlush(
        runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt));
#endif
}

void Connection::processStreamReceive() {
    assertWorkerOwner();
    datapath::StreamReceiveBatch batch(worker_->getStreamReceivePool());
    streamReceiveBatch_.swap(batch);
    const bool publishPeerFinished = peerFinishedPending_;
    peerFinishedPending_ = false;

    const uint64_t byteCount = static_cast<uint64_t>(batch.size());
    
#if defined(FFL_P2P_DIAGNOSTICS)
    const uint64_t bufferCount = static_cast<uint64_t>(batch.getBufferCount());
#endif

    if (byteCount == 0 && !publishPeerFinished)
        return;

#if defined(FFL_P2P_DIAGNOSTICS)
    if (byteCount != 0) {
        streamReceiveFlushes_.fetch_add(1, std::memory_order_relaxed);
        streamReceiveBuffersProcessed_.fetch_add(bufferCount, std::memory_order_relaxed);
        updateMaximum(maxStreamReceiveBatchBytes_, byteCount);
        updateMaximum(maxStreamReceiveBuffersPerBatch_, bufferCount);
    }
#endif

    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appReceiveBatch_.appendBatch(batch);
        if (publishPeerFinished)
            appPeerFinished_ = true;
        ++appGeneration_;
    }

    appCondition_.notify_all();
}

void Connection::processSendFlush() {
    if (!protocol_->conn)
        return;

#if defined(FFL_P2P_DIAGNOSTICS)
    const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
    const uint64_t packetsBefore = protocol_->txPacketCount;
    const uint64_t bytesBefore = protocol_->txPacketBytes;
    const uint64_t transportCallsBefore = protocol_->txSendCalls;
    sendFlushes_.fetch_add(1, std::memory_order_relaxed);
#endif

    const bool budgetExhausted = flushPackets();

#if defined(FFL_P2P_DIAGNOSTICS)
    const uint64_t packetCount = protocol_->txPacketCount - packetsBefore;
    const uint64_t byteCount = protocol_->txPacketBytes - bytesBefore;
    const uint64_t transportCalls = protocol_->txSendCalls - transportCallsBefore;
    const bool pendingData = protocol_->txSubmittedOffset < protocol_->txNextOffset ||
        (protocol_->txFinRequested && !protocol_->txFinSubmitted);
    runtimeDiagnostics_.recordSendFlush(
        diagnosticsStartedAt, runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt),
        packetCount, byteCount, transportCalls, packetCount == 0 && pendingData);
#endif

    if (budgetExhausted) {
#if defined(FFL_P2P_DIAGNOSTICS)
        sendFlushBudgetRequeues_.fetch_add(1, std::memory_order_relaxed);
#endif
        queueSendFlush();
    }
}

void Connection::processTimer() {
#if defined(FFL_P2P_DIAGNOSTICS)
    const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
    timerExpirations_.fetch_add(1, std::memory_order_relaxed);
#endif

    const platform::Timestamp timestamp = platform::getCurrentTimestampNS();
    bool sendRetryDue = sendRetryExpiry_ <= timestamp;

    if (sendRetryDue)
        clearSendRetry();

    if (protocol_->conn && ngtcp2_conn_get_expiry2(protocol_->conn) <= timestamp) {
        const int result = ngtcp2_conn_handle_expiry(protocol_->conn, timestamp);

        if (result == NGTCP2_ERR_IDLE_CLOSE)
            throw std::runtime_error("QUIC idle timeout");

        if (result < 0)
            throw std::runtime_error(std::string("ngtcp2_conn_handle_expiry: ") +
                                     ngtcp2_strerror(result));

        sendRetryDue = true;
    }

    if (sendRetryDue)
        queueSendFlush();

#if defined(FFL_P2P_DIAGNOSTICS)
    runtimeDiagnostics_.recordTimer(runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt));
#endif
}

void Connection::processShutdown() {
    publishApplicationState();
    protocol_->releaseTransport();

    stopped_.store(true, std::memory_order_release);
    notifyApplication();
}

void Connection::initializeClient() {
    if (certificatePEM_.empty())
        throw std::runtime_error("client requires the pinned QUIC certificate");

    protocol_->server = false;
    initializeFixedPath(*protocol_);
    initializeTLS(*protocol_, certificatePEM_, nullptr);

    const auto dcid = generateRandomCID();
    const auto scid = generateRandomCID();
    initializeQUIC(*protocol_, dcid, scid, nullptr);
}

void Connection::initializeServer(const uint8_t *initialPacket, size_t initialPacketLength) {
    if (certificatePEM_.empty() || privateKeyPEM_.empty())
        throw std::runtime_error("server QUIC credentials are unavailable");

    protocol_->server = true;
    initializeFixedPath(*protocol_);

    ngtcp2_version_cid versionCID{};
    const int decodeResult = ngtcp2_pkt_decode_version_cid(
        &versionCID, initialPacket, initialPacketLength, 0);

    if (decodeResult != 0 || versionCID.version != NGTCP2_PROTO_VER_V1 ||
        versionCID.dcidlen == 0 || versionCID.dcidlen > NGTCP2_MAX_CIDLEN ||
        versionCID.scidlen == 0 || versionCID.scidlen > NGTCP2_MAX_CIDLEN)
        throw std::runtime_error("invalid QUIC Initial packet");

    ngtcp2_cid originalDCID{};
    originalDCID.datalen = versionCID.dcidlen;
    std::memcpy(originalDCID.data, versionCID.dcid, versionCID.dcidlen);

    ngtcp2_cid dcid{};
    dcid.datalen = versionCID.scidlen;
    std::memcpy(dcid.data, versionCID.scid, versionCID.scidlen);
    const auto scid = generateRandomCID();

    initializeTLS(*protocol_, certificatePEM_, &privateKeyPEM_);
    initializeQUIC(*protocol_, dcid, scid, &originalDCID);
}

void Connection::feedPacket(const uint8_t *packet, size_t packetLength) {
    if (!protocol_->conn) {
        if (role_ != Role::Server)
            throw std::runtime_error("client QUIC connection is not initialized");

        initializeServer(packet, packetLength);
    }

    ngtcp2_path_storage pathStorage;
    ngtcp2_path_storage_init(
        &pathStorage,
        reinterpret_cast<const ngtcp2_sockaddr *>(protocol_->local.data()),
        static_cast<ngtcp2_socklen>(protocol_->local.size()),
        reinterpret_cast<const ngtcp2_sockaddr *>(protocol_->peer.data()),
        static_cast<ngtcp2_socklen>(protocol_->peer.size()), nullptr);

    ngtcp2_pkt_info packetInfo{};
    const int result = ngtcp2_conn_read_pkt(
        protocol_->conn, &pathStorage.path, &packetInfo,
        packet, packetLength, platform::getCurrentTimestampNS());

    if (result < 0)
        throw std::runtime_error(std::string("ngtcp2_conn_read_pkt: ") + ngtcp2_strerror(result));

#if defined(FFL_P2P_DIAGNOSTICS)
    ++protocol_->rxPacketCount;
    protocol_->rxPacketBytes += packetLength;
    protocol_->maxRxPacketSize = (std::max)(protocol_->maxRxPacketSize, packetLength);
#endif
}

ice::SendResult Connection::sendTransportOnce(const uint8_t *data, size_t size,
                                                   size_t segmentSize, bool aggregate,
                                                   bool retry) {
#if defined(FFL_P2P_DIAGNOSTICS)
    txSendAttempts_.fetch_add(1, std::memory_order_relaxed);
    if (retry)
        txSendRetryAttempts_.fetch_add(1, std::memory_order_relaxed);
    const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
#else
    (void)retry;
#endif

    ice::SendResult result;
    if (aggregate) {
        const size_t effectiveSegmentSize = (std::min)(segmentSize, size);
        result = binding_.sendAggregate(data, size, effectiveSegmentSize);
    } else {
        result = binding_.send(data, size);
    }

#if defined(FFL_P2P_DIAGNOSTICS)
    runtimeDiagnostics_.recordTransportSend(
        runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt), result.sentSize);
#endif

    if (result.sentSize != 0)
        ngtcp2_conn_update_pkt_tx_time(protocol_->conn, platform::getCurrentTimestampNS());

    return result;
}

#if defined(FFL_P2P_DIAGNOSTICS)

void Connection::recordCompletedTransportSend(uint64_t packetCount, uint64_t byteCount,
                                              bool aggregate) {
    ++protocol_->txSendCalls;

    if (!aggregate || packetCount <= 1)
        return;

    ++protocol_->txAggregateSendCalls;
    protocol_->txAggregatePackets += packetCount;
    protocol_->txAggregateBytes += byteCount;
    protocol_->maxAggregatePackets = (std::max)(
        protocol_->maxAggregatePackets, static_cast<size_t>(packetCount));
    protocol_->maxAggregateBytes = (std::max)(
        protocol_->maxAggregateBytes, static_cast<size_t>(byteCount));
}

void Connection::recordSendBackpressure(size_t deferredBytes) {
    txSendBackpressureEvents_.fetch_add(1, std::memory_order_relaxed);
    txSendBackpressureBytes_.fetch_add(deferredBytes, std::memory_order_relaxed);
}

#endif

void Connection::scheduleSendRetry() {
    sendRetryExpiry_ = platform::getCurrentTimestampNS() + kSendRetryDelayNS;
}

void Connection::clearSendRetry() {
    sendRetryExpiry_ = platform::InfiniteTimestamp;
}

void Connection::retainPendingTransportSend(const uint8_t *data, size_t size, size_t sentSize,
                                            size_t segmentSize, uint64_t packetCount,
                                            bool aggregate) {
    if (pendingSend_.hasRemainingData())
        throw std::logic_error("QUIC transport send is already pending");

    if (sentSize >= size)
        throw std::logic_error("blocked QUIC transport send has no pending bytes");

    if (aggregate && sentSize != 0 && sentSize % segmentSize != 0)
        throw std::runtime_error("libjuice aggregate send reported non-segment-aligned progress");

    pendingSend_.data.assign(data + sentSize, data + size);
    pendingSend_.offset = 0;
    pendingSend_.segmentSize = segmentSize;
    pendingSend_.packetCount = packetCount;
    pendingSend_.logicalBytes = size;
    pendingSend_.aggregate = aggregate;

    scheduleSendRetry();
}

Connection::TransportSendOutcome Connection::sendTransportBuffer(
    const uint8_t *data, size_t size, size_t segmentSize, uint64_t packetCount, bool aggregate) {
    if (pendingSend_.hasRemainingData())
        throw std::logic_error("new QUIC transport send started while retry data is pending");

    const ice::SendResult result = sendTransportOnce(
        data, size, segmentSize, aggregate, false);

    if (result.status == ice::SendStatus::Complete) {
#if defined(FFL_P2P_DIAGNOSTICS)
        recordCompletedTransportSend(packetCount, size, aggregate);
#endif
        clearSendRetry();
        return TransportSendOutcome::Complete;
    }

#if defined(FFL_P2P_DIAGNOSTICS)
    recordSendBackpressure(size - result.sentSize);
#endif

    retainPendingTransportSend(
        data, size, result.sentSize, segmentSize, packetCount, aggregate);

    return TransportSendOutcome::Blocked;
}

Connection::TransportSendOutcome Connection::flushPendingTransportSend() {
    if (!pendingSend_.hasRemainingData())
        return TransportSendOutcome::Complete;

    const size_t remaining = pendingSend_.getRemainingSize();
    const uint8_t *data = pendingSend_.data.data() + pendingSend_.offset;
    const ice::SendResult result = sendTransportOnce(
        data, remaining, pendingSend_.segmentSize, pendingSend_.aggregate, true);

    pendingSend_.offset += result.sentSize;

    if (result.status == ice::SendStatus::Complete) {
        if (pendingSend_.hasRemainingData())
            throw std::runtime_error("libjuice completed a QUIC retry with pending bytes remaining");

#if defined(FFL_P2P_DIAGNOSTICS)
        const uint64_t packetCount = pendingSend_.packetCount;
        const uint64_t logicalBytes = pendingSend_.logicalBytes;
        const bool aggregate = pendingSend_.aggregate;
#endif

        pendingSend_.clear();
        clearSendRetry();
        
#if defined(FFL_P2P_DIAGNOSTICS)
        recordCompletedTransportSend(packetCount, logicalBytes, aggregate);
#endif

        return TransportSendOutcome::Complete;
    }

    if (!pendingSend_.hasRemainingData())
        throw std::runtime_error("libjuice blocked a QUIC retry after reporting all bytes sent");

#if defined(FFL_P2P_DIAGNOSTICS)
    recordSendBackpressure(pendingSend_.getRemainingSize());
#endif

    scheduleSendRetry();

    return TransportSendOutcome::Blocked;
}

bool Connection::flushPackets() {
    if (!protocol_->conn)
        return false;

    if (flushPendingTransportSend() == TransportSendOutcome::Blocked)
        return false;

    datapath::SendBuffer &output = worker_->getSendBuffer();
    const int maxPacketsPerFlush = kMaxPacketsPerFlush;

    if (!aggregatePackets_.load(std::memory_order_relaxed)) {
        int packetCount = 0;

        for (; packetCount < maxPacketsPerFlush;) {
            ngtcp2_path_storage pathStorage;
            ngtcp2_path_storage_zero(&pathStorage);
            ngtcp2_pkt_info packetInfo{};
            const platform::Timestamp timestamp = platform::getCurrentTimestampNS();
            
#if defined(FFL_P2P_DIAGNOSTICS)
            const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
#endif

            const ngtcp2_ssize written = writeOnePacket(
                *protocol_, &pathStorage.path, &packetInfo,
                output.data(), output.size(), timestamp, false);
                
#if defined(FFL_P2P_DIAGNOSTICS)
            runtimeDiagnostics_.recordPacketWrite(
                runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt),
                written > 0 ? 1 : 0,
                written > 0 ? static_cast<uint64_t>(written) : 0);
#endif

            if (written == 0)
                break;

            ++packetCount;

            const TransportSendOutcome outcome = sendTransportBuffer(
                output.data(), static_cast<size_t>(written), static_cast<size_t>(written), 1, false);

            if (outcome == TransportSendOutcome::Blocked)
                return false;
        }

        return packetCount == maxPacketsPerFlush;
    }

    int batchIndex = 0;
    for (; batchIndex < maxPacketsPerFlush;) {
        ngtcp2_path_storage pathStorage;
        ngtcp2_path_storage_zero(&pathStorage);
        ngtcp2_pkt_info packetInfo{};
        size_t segmentSize = 0;
        const platform::Timestamp timestamp = platform::getCurrentTimestampNS();
        const uint64_t packetCountBefore = protocol_->aggregatePacketCount;
        protocol_->aggregateWriteError.clear();
        
#if defined(FFL_P2P_DIAGNOSTICS)
        const platform::Timestamp diagnosticsStartedAt = runtimeDiagnostics_.startTiming();
#endif

        const ngtcp2_ssize written = ngtcp2_conn_write_aggregate_pkt2(
            protocol_->conn, &pathStorage.path, &packetInfo,
            output.data(), output.size(), &segmentSize,
            writeAggregatePacketCallback,
            static_cast<size_t>(maxPacketsPerFlush - batchIndex), timestamp);
        const uint64_t packetCountAfter = protocol_->aggregatePacketCount;
        
#if defined(FFL_P2P_DIAGNOSTICS)
        runtimeDiagnostics_.recordPacketWrite(
            runtimeDiagnostics_.elapsedSince(diagnosticsStartedAt),
            packetCountAfter - packetCountBefore,
            written > 0 ? static_cast<uint64_t>(written) : 0);
#endif

        if (written < 0) {
            if (!protocol_->aggregateWriteError.empty())
                throw std::runtime_error(protocol_->aggregateWriteError);

            throw std::runtime_error(std::string("ngtcp2_conn_write_aggregate_pkt2: ") +
                                     ngtcp2_strerror(static_cast<int>(written)));
        }

        const int packetCount = static_cast<int>(packetCountAfter - packetCountBefore);

        if (written == 0 || packetCount == 0)
            break;

        if (segmentSize == 0)
            segmentSize = static_cast<size_t>(written);

        batchIndex += packetCount;

        const TransportSendOutcome outcome = sendTransportBuffer(
            output.data(), static_cast<size_t>(written), segmentSize,
            static_cast<uint64_t>(packetCount), true);

        if (outcome == TransportSendOutcome::Blocked)
            return false;
    }

    return batchIndex >= maxPacketsPerFlush;
}

#if defined(FFL_P2P_DIAGNOSTICS)
ConnectionStats Connection::collectDiagnosticsSnapshot() const {
    ConnectionStats snapshot;

    snapshot.runtimeVersion = 2;
    snapshot.operationsQueued = operationsQueued_.load(std::memory_order_relaxed);
    snapshot.operationsProcessed = operationsProcessed_.load(std::memory_order_relaxed);
    snapshot.receiveFlushes = receiveFlushes_.load(std::memory_order_relaxed);
    snapshot.receiveDatagrams = receiveDatagrams_.load(std::memory_order_relaxed);
    snapshot.maxReceiveBatch = maxReceiveBatch_.load(std::memory_order_relaxed);
    snapshot.receiveBuffersProcessed = receiveBuffersProcessed_.load(std::memory_order_relaxed);
    snapshot.maxReceiveBuffersPerBatch = maxReceiveBuffersPerBatch_.load(std::memory_order_relaxed);

    const datapath::ReceiveBufferPoolStats receivePoolStats = worker_->getReceivePool().getStats();

    snapshot.receivePoolSlabs = receivePoolStats.slabsAllocated;
    snapshot.receivePoolBuffers = receivePoolStats.buffersAllocated;
    snapshot.receivePoolAcquires = receivePoolStats.acquireCount;
    snapshot.receivePoolReuses = receivePoolStats.reuseCount;
    snapshot.receivePoolDynamicGrowths = receivePoolStats.dynamicGrowths;
    snapshot.streamReceiveCallbacks = streamReceiveCallbacks_.load(std::memory_order_relaxed);
    snapshot.streamReceiveFlushes = streamReceiveFlushes_.load(std::memory_order_relaxed);
    snapshot.streamReceiveBytes = streamReceiveBytes_.load(std::memory_order_relaxed);
    snapshot.maxStreamReceiveBatchBytes = maxStreamReceiveBatchBytes_.load(std::memory_order_relaxed);
    snapshot.streamReceiveBuffersProcessed = streamReceiveBuffersProcessed_.load(std::memory_order_relaxed);
    snapshot.maxStreamReceiveBuffersPerBatch = maxStreamReceiveBuffersPerBatch_.load(std::memory_order_relaxed);

    const datapath::StreamReceiveBufferPoolStats streamReceivePoolStats =
        worker_->getStreamReceivePool().getStats();

    snapshot.streamReceivePoolSlabs = streamReceivePoolStats.slabsAllocated;
    snapshot.streamReceivePoolBuffers = streamReceivePoolStats.buffersAllocated;
    snapshot.streamReceivePoolAcquires = streamReceivePoolStats.acquireCount;
    snapshot.streamReceivePoolReuses = streamReceivePoolStats.reuseCount;
    snapshot.streamReceivePoolDynamicGrowths = streamReceivePoolStats.dynamicGrowths;
    snapshot.sendFlushes = sendFlushes_.load(std::memory_order_relaxed);
    snapshot.sendFlushBudgetRequeues = sendFlushBudgetRequeues_.load(std::memory_order_relaxed);
    snapshot.timerExpirations = timerExpirations_.load(std::memory_order_relaxed);
    snapshot.workerIndex = worker_ ? worker_->index() : 0;
    snapshot.workerStackSize = worker_ ? worker_->stackSize() : 0;
    snapshot.sendBufferSize = worker_ ? worker_->getSendBuffer().size() : 0;

    if (protocol_->conn) {
        ngtcp2_conn_info connectionInfo{};
        ngtcp2_conn_get_conn_info2(protocol_->conn, &connectionInfo);
        snapshot.cwnd = connectionInfo.cwnd;
        snapshot.bytesInFlight = connectionInfo.bytes_in_flight;
        snapshot.smoothedRTTNs = connectionInfo.smoothed_rtt;
        snapshot.packetsLost = connectionInfo.pkt_lost;
        snapshot.bytesLost = connectionInfo.bytes_lost;
        snapshot.packetsDiscarded = connectionInfo.pkt_discarded;
        snapshot.maxTxUDPPayload = ngtcp2_conn_get_path_max_tx_udp_payload_size2(protocol_->conn);
        snapshot.sendQuantum = ngtcp2_conn_get_send_quantum2(protocol_->conn);
    }

    snapshot.txPackets = protocol_->txPacketCount;
    snapshot.txBytes = protocol_->txPacketBytes;
    snapshot.txSendCalls = protocol_->txSendCalls;
    snapshot.txSendAttempts = txSendAttempts_.load(std::memory_order_relaxed);
    snapshot.txSendBackpressureEvents = txSendBackpressureEvents_.load(std::memory_order_relaxed);
    snapshot.txSendRetryAttempts = txSendRetryAttempts_.load(std::memory_order_relaxed);
    snapshot.txSendBackpressureBytes = txSendBackpressureBytes_.load(std::memory_order_relaxed);
    snapshot.txPendingSendBytes = pendingSend_.getRemainingSize();
    snapshot.txAggregateSendCalls = protocol_->txAggregateSendCalls;
    snapshot.txAggregatePackets = protocol_->txAggregatePackets;
    snapshot.txAggregateBytes = protocol_->txAggregateBytes;
    snapshot.maxAggregatePackets = protocol_->maxAggregatePackets;
    snapshot.maxAggregateBytes = protocol_->maxAggregateBytes;
    snapshot.rxPackets = protocol_->rxPacketCount;
    snapshot.rxBytes = protocol_->rxPacketBytes;
    snapshot.txNextOffset = protocol_->txNextOffset;
    snapshot.txSubmittedOffset = protocol_->txSubmittedOffset;
    snapshot.txAckedOffset = protocol_->txAckedOffset;
    snapshot.rxOffset = protocol_->rxOffset;
    snapshot.txFinRequested = protocol_->txFinRequested ? 1 : 0;
    snapshot.txFinSubmitted = protocol_->txFinSubmitted ? 1 : 0;
    snapshot.txFinAcked = protocol_->txFinAcked ? 1 : 0;
    snapshot.maxTxPacketSize = protocol_->maxTxPacketSize;
    snapshot.maxRxPacketSize = protocol_->maxRxPacketSize;

    return snapshot;
}
#endif

void Connection::publishApplicationState() {
#if defined(FFL_P2P_DIAGNOSTICS)
    const ConnectionStats diagnosticsSnapshot = collectDiagnosticsSnapshot();
#endif

    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appSubmittedWriteOffset_ = protocol_->conn ? protocol_->txSubmittedOffset : 0;
        appAckedWriteOffset_ = protocol_->conn ? protocol_->txAckedOffset : 0;
        appFinSubmitted_ = protocol_->conn && protocol_->txFinSubmitted;
        appFinAcked_ = protocol_->conn && protocol_->txFinAcked;
        appHandshakeComplete_ = protocol_->conn && protocol_->handshakeComplete;
        appHandshakeConfirmed_ = protocol_->conn && protocol_->handshakeConfirmed;
        appStreamClosed_ = protocol_->conn && protocol_->streamClosed;
        
#if defined(FFL_P2P_DIAGNOSTICS)
        appStats_ = diagnosticsSnapshot;
#endif

        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::appendReceivedStreamData(const uint8_t *data, size_t size) {
    assertWorkerOwner();

    if (!data || size == 0)
        return;

    streamReceiveBatch_.append(data, size);
    
#if defined(FFL_P2P_DIAGNOSTICS)
    streamReceiveCallbacks_.fetch_add(1, std::memory_order_relaxed);
    streamReceiveBytes_.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
#endif

    queueStreamReceiveFlush();
}

void Connection::markPeerFinished() {
    assertWorkerOwner();
    peerFinishedPending_ = true;
    queueStreamReceiveFlush();
}

void Connection::markHandshakeComplete() {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appHandshakeComplete_ = true;
        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::markHandshakeConfirmed() {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appHandshakeConfirmed_ = true;
        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::markStreamClosed() {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appStreamClosed_ = true;
        appSubmittedWriteOffset_ = protocol_->txSubmittedOffset;
        appAckedWriteOffset_ = protocol_->txAckedOffset;
        appFinSubmitted_ = protocol_->txFinSubmitted;
        appFinAcked_ = protocol_->txFinAcked;
        ++appGeneration_;
    }
    appCondition_.notify_all();
}

void Connection::markAcknowledged() {
    {
        std::lock_guard<std::mutex> guard(appMutex_);
        appSubmittedWriteOffset_ = protocol_->txSubmittedOffset;
        appAckedWriteOffset_ = protocol_->txAckedOffset;
        appFinSubmitted_ = protocol_->txFinSubmitted;
        appFinAcked_ = protocol_->txFinAcked;
        ++appGeneration_;
    }
    appCondition_.notify_all();
}

} // namespace ffl::quic
