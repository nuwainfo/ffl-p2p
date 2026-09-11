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

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "datapath/DatapathInternal.h"

#include "platform/DatagramBatch.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Cosmopolitan APEs are not compiled with __linux__, even when they execute on
 * Linux. TX GSO only needs the ordinary sendmsg/cmsg ABI that Cosmopolitan does
 * expose, so keep that code compiled into APEs and gate it at runtime with
 * platform::isLinux(). RX batching is handled separately by DatagramBatchReceiver.
 */
#if defined(__linux__) || defined(__COSMOPOLITAN__)
#define FFL_P2P_HAS_LINUX_GSO 1
#else
#define FFL_P2P_HAS_LINUX_GSO 0
#endif

#if FFL_P2P_HAS_LINUX_GSO
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#endif

namespace ffl::datapath {

namespace {

constexpr size_t kReceiveDatagramSize = 4096;
constexpr size_t kReceiveAddressSize = JUICE_UDP_RECV_ADDRESS_STORAGE_SIZE;

#if FFL_P2P_HAS_LINUX_GSO
constexpr size_t kMaxGSOSegments = 64;
constexpr size_t kProbeSegmentSize = 1200;
constexpr size_t kProbeSegmentCount = 2;
#endif

static_assert(sizeof(sockaddr_storage) <= kReceiveAddressSize,
              "juice UDP address storage too small");
static_assert(JUICE_UDP_RECV_BATCH_CAPACITY <= platform::DatagramBatchReceiver::MaxBatchSize,
              "juice receive batch exceeds platform receiver capacity");

class ScopedSocket {
public:
    explicit ScopedSocket(int handle = -1) : handle_(handle) {}

    ~ScopedSocket() {
        if (handle_ >= 0) {
            close(handle_);
        }
    }

    ScopedSocket(const ScopedSocket &) = delete;
    ScopedSocket &operator=(const ScopedSocket &) = delete;

    int handle() const {
        return handle_;
    }

    bool isValid() const {
        return handle_ >= 0;
    }

private:
    int handle_;
};

class POSIXDatapathBackend final : public DatapathBackend {
public:

#if defined(FFL_P2P_DIAGNOSTICS)
    POSIXDatapathBackend(DatapathDiagnostics &diagnostics, const DatapathOptions &options)
        : DatapathBackend(diagnostics),
          sendSegmentationDisabled_(options.disableSendSegmentation),
          receiveBatchingDisabled_(options.disableReceiveBatching) {
#else
    explicit POSIXDatapathBackend(const DatapathOptions &options)
        : sendSegmentationDisabled_(options.disableSendSegmentation),
          receiveBatchingDisabled_(options.disableReceiveBatching) {
#endif
        initializeCapabilities();
    }

    int sendAggregate(
        uintptr_t socketHandle, int socketFamily, const uint8_t *destinationAddress,
        size_t destinationAddressSize, const char *data, size_t size, size_t segmentSize,
        size_t *sentSize, int *socketError) override {
        (void)socketFamily;

#if FFL_P2P_HAS_LINUX_GSO
        if (!sendSegmentationEnabled_ || size <= segmentSize ||
            segmentSize > static_cast<size_t>((std::numeric_limits<uint16_t>::max)())) {
            return sendFallback();
        }

        const size_t datagramCount = (size + segmentSize - 1) / segmentSize;
        if (datagramCount > kMaxGSOSegments || destinationAddressSize > sizeof(sockaddr_storage)) {
            return sendFallback();
        }

        struct iovec ioVector{};
        ioVector.iov_base = const_cast<char *>(data);
        ioVector.iov_len = size;

        std::array<unsigned char, CMSG_SPACE(sizeof(uint16_t))> controlBuffer{};
        struct msghdr message{};
        message.msg_name = const_cast<uint8_t *>(destinationAddress);
        message.msg_namelen = static_cast<socklen_t>(destinationAddressSize);
        message.msg_iov = &ioVector;
        message.msg_iovlen = 1;
        message.msg_control = controlBuffer.data();
        message.msg_controllen = controlBuffer.size();

        struct cmsghdr *control = CMSG_FIRSTHDR(&message);
        if (!control) {
            *socketError = EINVAL;
            return JUICE_UDP_SEND_AGGREGATE_FAILED;
        }

        control->cmsg_level = SOL_UDP;
        control->cmsg_type = UDP_SEGMENT;
        control->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        *reinterpret_cast<uint16_t *>(CMSG_DATA(control)) = static_cast<uint16_t>(segmentSize);

#if defined(FFL_P2P_DIAGNOSTICS)
        const uint64_t startedAt = platform::getCurrentTimestampNS();
#endif

        const ssize_t result = sendmsg(static_cast<int>(socketHandle), &message, 0);
        
#if defined(FFL_P2P_DIAGNOSTICS)
        const uint64_t durationNS = platform::getCurrentTimestampNS() - startedAt;
#endif

        if (result < 0) {
            const int error = errno;
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordSendCall(0, 0, durationNS, false));

            if (isSegmentationCapabilityError(error)) {
                disableSendSegmentation();
                return sendFallback();
            }

            *socketError = error;
            return JUICE_UDP_SEND_AGGREGATE_FAILED;
        }

        if (static_cast<size_t>(result) != size) {
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordSendCall(0, 0, durationNS, false));
            *socketError = EIO;
            return JUICE_UDP_SEND_AGGREGATE_FAILED;
        }

        *sentSize = size;
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordSendCall(datagramCount, size, durationNS, true));
        return JUICE_UDP_SEND_AGGREGATE_HANDLED;
#else
        (void)socketHandle;
        (void)destinationAddress;
        (void)destinationAddressSize;
        (void)data;
        (void)size;
        (void)segmentSize;
        (void)sentSize;
        (void)socketError;
        return sendFallback();
#endif
    }

    int receiveBatch(
        uintptr_t socketHandle, juice_udp_recv_datagram_t *datagrams,
        size_t capacity, int *socketError) override {
        if (receiveBatchingEnabled_) {
            return receiveMessages(
                static_cast<int>(socketHandle), datagrams, capacity, socketError);
        }

        return receiveSingle(static_cast<int>(socketHandle), datagrams, socketError);
    }

private:
    void initializeCapabilities() {
        const bool linuxRuntime = platform::isLinux();

#if FFL_P2P_HAS_LINUX_GSO
        const bool segmentationSupported = linuxRuntime && probeSendSegmentation();
#else
        const bool segmentationSupported = false;
#endif
        const bool receiveBatchingSupported =
            linuxRuntime && batchReceiver_.isSupported();

        sendSegmentationEnabled_ = segmentationSupported && !sendSegmentationDisabled_;
        receiveBatchingEnabled_ =
            receiveBatchingSupported && !receiveBatchingDisabled_;

        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setSendSegmentationSupported(segmentationSupported));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setSendSegmentationEnabled(sendSegmentationEnabled_));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveBatchingSupported(receiveBatchingSupported));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveBatchingEnabled(receiveBatchingEnabled_));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingSupported(false));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingEnabled(false));
    }

#if FFL_P2P_HAS_LINUX_GSO
    static bool isSegmentationCapabilityError(int error) {
        return error == EINVAL || error == ENOPROTOOPT || error == EOPNOTSUPP ||
               error == ENOSYS;
    }

    static bool probeSendSegmentation() {
        if (!platform::isLinux()) {
            return false;
        }

        ScopedSocket sendSocket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        ScopedSocket receiveSocket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!sendSocket.isValid() || !receiveSocket.isValid()) {
            return false;
        }

        sockaddr_in receiveAddress{};
        receiveAddress.sin_family = AF_INET;
        receiveAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        receiveAddress.sin_port = 0;

        if (bind(
                receiveSocket.handle(), reinterpret_cast<const sockaddr *>(&receiveAddress),
                sizeof(receiveAddress)) != 0) {
            return false;
        }

        socklen_t addressSize = sizeof(receiveAddress);
        if (getsockname(
                receiveSocket.handle(), reinterpret_cast<sockaddr *>(&receiveAddress),
                &addressSize) != 0) {
            return false;
        }

        std::array<unsigned char, kProbeSegmentSize * kProbeSegmentCount> payload{};
        struct iovec ioVector{};
        ioVector.iov_base = payload.data();
        ioVector.iov_len = payload.size();

        std::array<unsigned char, CMSG_SPACE(sizeof(uint16_t))> controlBuffer{};
        struct msghdr message{};
        message.msg_name = &receiveAddress;
        message.msg_namelen = addressSize;
        message.msg_iov = &ioVector;
        message.msg_iovlen = 1;
        message.msg_control = controlBuffer.data();
        message.msg_controllen = controlBuffer.size();

        struct cmsghdr *control = CMSG_FIRSTHDR(&message);
        if (!control) {
            return false;
        }

        control->cmsg_level = SOL_UDP;
        control->cmsg_type = UDP_SEGMENT;
        control->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        *reinterpret_cast<uint16_t *>(CMSG_DATA(control)) =
            static_cast<uint16_t>(kProbeSegmentSize);

        const ssize_t result = sendmsg(sendSocket.handle(), &message, 0);
        return result == static_cast<ssize_t>(payload.size());
    }

    void disableSendSegmentation() {
        sendSegmentationEnabled_ = false;
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setSendSegmentationEnabled(false));
    }
#endif

    int receiveMessages(
        int socketHandle, juice_udp_recv_datagram_t *datagrams,
        size_t capacity, int *socketError) {
        const size_t messageCapacity = (std::min)({
            capacity,
            static_cast<size_t>(JUICE_UDP_RECV_BATCH_CAPACITY),
            platform::DatagramBatchReceiver::MaxBatchSize,
        });

        for (size_t index = 0; index < messageCapacity; ++index) {
            std::memset(&receiveAddresses_[index], 0, sizeof(receiveAddresses_[index]));

            platform::DatagramReceiveBuffer &buffer = receiveBuffers_[index];
            buffer.data = receivePayloads_[index].data();
            buffer.dataCapacity = receivePayloads_[index].size();
            buffer.sourceAddress = &receiveAddresses_[index];
            buffer.sourceAddressCapacity = sizeof(receiveAddresses_[index]);
            buffer.dataSize = 0;
            buffer.sourceAddressSize = 0;
            buffer.flags = 0;
        }

        int receivedCount = 0;
        for (;;) {
            receivedCount = batchReceiver_.receive(
                socketHandle, receiveBuffers_.data(), messageCapacity, socketError);
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveCall());

            if (receivedCount >= 0) {
                break;
            }

            const int error = *socketError;
            if (isIgnoredUDPError(error)) {
                continue;
            }

            if (error == EAGAIN || error == EWOULDBLOCK) {
                return 0;
            }

            if (error == ENOSYS || error == EOPNOTSUPP) {
                disableReceiveBatching();
                return receiveSingle(socketHandle, datagrams, socketError);
            }

            return -1;
        }

        if (receivedCount == 0) {
            return 0;
        }

#if defined(FFL_P2P_DIAGNOSTICS)
        size_t receivedBytes = 0;
#endif

        for (int index = 0; index < receivedCount; ++index) {
            const platform::DatagramReceiveBuffer &buffer =
                receiveBuffers_[static_cast<size_t>(index)];
            if ((buffer.flags & MSG_TRUNC) != 0 ||
                buffer.dataSize > kReceiveDatagramSize ||
                buffer.sourceAddressSize == 0 ||
                buffer.sourceAddressSize > kReceiveAddressSize) {
                *socketError = EMSGSIZE;
                return -1;
            }

            juice_udp_recv_datagram_t &datagram = datagrams[static_cast<size_t>(index)];
            datagram.data = reinterpret_cast<const char *>(
                receivePayloads_[static_cast<size_t>(index)].data());
            datagram.size = buffer.dataSize;
            std::memcpy(
                datagram.source_address,
                &receiveAddresses_[static_cast<size_t>(index)],
                buffer.sourceAddressSize);
            datagram.source_address_size = buffer.sourceAddressSize;

#if defined(FFL_P2P_DIAGNOSTICS)
            receivedBytes += datagram.size;
#endif
        }

#if defined(FFL_P2P_DIAGNOSTICS)
        diagnostics_.recordReceiveBatch(
            static_cast<size_t>(receivedCount), receivedBytes, false);
#endif

        return receivedCount;
    }

    void disableReceiveBatching() {
        receiveBatchingEnabled_ = false;
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveBatchingEnabled(false));
    }

    static bool isIgnoredUDPError(int error) {
        return error == ECONNRESET || error == ENETRESET || error == ECONNREFUSED;
    }

    int sendFallback() {
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordSendFallback());
        return JUICE_UDP_SEND_AGGREGATE_NOT_HANDLED;
    }

    int receiveSingle(
        int socketHandle, juice_udp_recv_datagram_t *datagrams, int *socketError) {
        sockaddr_storage source{};
        socklen_t sourceLength = sizeof(source);
        ssize_t received = 0;

        for (;;) {
            sourceLength = sizeof(source);
            received = recvfrom(
                socketHandle, receiveSingleBuffer_.data(), receiveSingleBuffer_.size(), 0,
                reinterpret_cast<sockaddr *>(&source), &sourceLength);
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveCall());

            if (received >= 0) {
                break;
            }

            const int error = errno;
            if (isIgnoredUDPError(error)) {
                continue;
            }

            *socketError = error;
            return -1;
        }

        if (received == 0) {
            return 0;
        }

        if (sourceLength == 0 || static_cast<size_t>(sourceLength) > kReceiveAddressSize) {
            *socketError = EMSGSIZE;
            return -1;
        }

        juice_udp_recv_datagram_t &datagram = datagrams[0];
        datagram.data = reinterpret_cast<const char *>(receiveSingleBuffer_.data());
        datagram.size = static_cast<size_t>(received);
        std::memcpy(datagram.source_address, &source, static_cast<size_t>(sourceLength));
        datagram.source_address_size = static_cast<size_t>(sourceLength);

        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveBatch(1, static_cast<size_t>(received), false));
        return 1;
    }

    bool sendSegmentationDisabled_{false};
    bool receiveBatchingDisabled_{false};
    bool sendSegmentationEnabled_{false};
    bool receiveBatchingEnabled_{false};
    platform::DatagramBatchReceiver batchReceiver_;
    std::array<unsigned char, kReceiveDatagramSize> receiveSingleBuffer_{};
    std::array<std::array<unsigned char, kReceiveDatagramSize>, JUICE_UDP_RECV_BATCH_CAPACITY>
        receivePayloads_{};
    std::array<sockaddr_storage, JUICE_UDP_RECV_BATCH_CAPACITY> receiveAddresses_{};
    std::array<platform::DatagramReceiveBuffer, JUICE_UDP_RECV_BATCH_CAPACITY>
        receiveBuffers_{};
};

} // namespace

#if defined(FFL_P2P_DIAGNOSTICS)
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(
    DatapathDiagnostics &diagnostics, const DatapathOptions &options) {
    return std::make_unique<POSIXDatapathBackend>(diagnostics, options);
}
#else
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(const DatapathOptions &options) {
    return std::make_unique<POSIXDatapathBackend>(options);
}
#endif


} // namespace ffl::datapath
