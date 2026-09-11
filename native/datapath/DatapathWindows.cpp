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

#include "datapath/DatapathInternal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <memory>

#ifndef UDP_RECV_MAX_COALESCED_SIZE
#define UDP_RECV_MAX_COALESCED_SIZE 3
#endif
#ifndef UDP_COALESCED_INFO
#define UDP_COALESCED_INFO 3
#endif

namespace ffl::datapath {

namespace {

constexpr size_t kReceiveBufferSize = 65535;
constexpr size_t kReceiveAddressSize = JUICE_UDP_RECV_ADDRESS_STORAGE_SIZE;
constexpr size_t kReceiveControlSize = 128;
constexpr uint32_t kMaxUROPayloadLength = 65535u - 8u;
static_assert(sizeof(sockaddr_storage) <= kReceiveAddressSize,
              "juice UDP address storage too small");

class WindowsDatapathBackend final : public DatapathBackend {
public:

#if defined(FFL_P2P_DIAGNOSTICS)
    WindowsDatapathBackend(DatapathDiagnostics &diagnostics, const DatapathOptions &options)
        : DatapathBackend(diagnostics), receiveCoalescingDisabled_(options.disableReceiveCoalescing) {
#else
    explicit WindowsDatapathBackend(const DatapathOptions &options)
        : receiveCoalescingDisabled_(options.disableReceiveCoalescing) {
#endif

        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveBatchingSupported(false));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveBatchingEnabled(false));
    }

    int sendAggregate(
        uintptr_t socketHandle, int socketFamily, const uint8_t *destinationAddress,
        size_t destinationAddressSize, const char *data, size_t size, size_t segmentSize,
        size_t *sentSize, int *socketError) override {
        (void)socketHandle;
        (void)socketFamily;
        (void)destinationAddress;
        (void)destinationAddressSize;
        (void)data;
        (void)size;
        (void)segmentSize;
        (void)sentSize;
        (void)socketError;

        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordSendFallback());
        return JUICE_UDP_SEND_AGGREGATE_NOT_HANDLED;
    }

    int receiveBatch(
        uintptr_t socketHandle, juice_udp_recv_datagram_t *datagrams,
        size_t capacity, int *socketError) override {
        const SOCKET socket = static_cast<SOCKET>(socketHandle);
        initializeSocket(socket);

        const int pending = emitPending(datagrams, capacity);
        if (pending > 0) {
            return pending;
        }

        if (!receiveMsg_ || !coalescingEnabled_) {
            return receiveSingle(socket, datagrams, socketError);
        }

        return receiveCoalesced(socket, datagrams, capacity, socketError);
    }

private:
    static bool isIgnoredUDPError(int error) {
        return error == WSAECONNRESET || error == WSAENETRESET || error == WSAECONNREFUSED;
    }

    int emitPending(juice_udp_recv_datagram_t *datagrams, size_t capacity) {
        if (!pendingBytes_ || !pendingSegmentSize_) {
            return 0;
        }

        size_t count = 0;
        while (pendingBytes_ && count < capacity) {
            const size_t segmentSize = (std::min)(pendingSegmentSize_, pendingBytes_);
            juice_udp_recv_datagram_t &datagram = datagrams[count++];

            datagram.data = reinterpret_cast<const char *>(receiveBuffer_.data() + pendingOffset_);
            datagram.size = segmentSize;
            std::memcpy(datagram.source_address, pendingAddress_.data(), pendingAddressSize_);
            datagram.source_address_size = pendingAddressSize_;

            pendingOffset_ += segmentSize;
            pendingBytes_ -= segmentSize;
        }

        if (!pendingBytes_) {
            pendingOffset_ = 0;
            pendingSegmentSize_ = 0;
            pendingAddressSize_ = 0;
        }

        return static_cast<int>(count);
    }

    void initializeSocket(SOCKET socket) {
        if (initialized_ && activeSocket_ == static_cast<uintptr_t>(socket)) {
            return;
        }

        activeSocket_ = static_cast<uintptr_t>(socket);
        initialized_ = true;
        receiveMsg_ = nullptr;
        pendingBytes_ = 0;
        coalescingSupported_ = false;
        coalescingEnabled_ = false;
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingSupported(false));
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingEnabled(false));

        GUID receiveMsgGuid = WSAID_WSARECVMSG;
        DWORD bytesReturned = 0;
        if (WSAIoctl(
                socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &receiveMsgGuid, sizeof(receiveMsgGuid),
                &receiveMsg_, sizeof(receiveMsg_),
                &bytesReturned, nullptr, nullptr) != 0) {
            receiveMsg_ = nullptr;
            return;
        }

        DWORD currentCoalescedSize = 0;
        int optionLength = sizeof(currentCoalescedSize);
        if (getsockopt(
                socket, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE,
                reinterpret_cast<char *>(&currentCoalescedSize), &optionLength) != 0) {
            return;
        }

        coalescingSupported_ = true;
        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingSupported(true));

        int receiveBufferSize = INT_MAX;
        (void)setsockopt(
            socket, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char *>(&receiveBufferSize), sizeof(receiveBufferSize));

        if (receiveCoalescingDisabled_) {
            return;
        }

        DWORD maxCoalescedSize = kMaxUROPayloadLength;
        if (setsockopt(
                socket, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE,
                reinterpret_cast<const char *>(&maxCoalescedSize),
                sizeof(maxCoalescedSize)) == 0) {
            coalescingEnabled_ = true;
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.setReceiveCoalescingEnabled(true));
        }
    }

    int receiveSingle(
        SOCKET socket, juice_udp_recv_datagram_t *datagrams, int *socketError) {
        SOCKADDR_STORAGE source{};
        int sourceLength = sizeof(source);
        int received = 0;

        for (;;) {
            sourceLength = sizeof(source);
            received = recvfrom(
                socket, reinterpret_cast<char *>(receiveBuffer_.data()),
                static_cast<int>(receiveBuffer_.size()), 0,
                reinterpret_cast<sockaddr *>(&source), &sourceLength);
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveCall());

            if (received >= 0) {
                break;
            }

            const int error = WSAGetLastError();
            if (isIgnoredUDPError(error)) {
                continue;
            }

            *socketError = error;
            return -1;
        }

        if (received == 0) {
            return 0;
        }

        if (sourceLength <= 0 || static_cast<size_t>(sourceLength) > kReceiveAddressSize) {
            *socketError = WSAEMSGSIZE;
            return -1;
        }

        juice_udp_recv_datagram_t &datagram = datagrams[0];
        datagram.data = reinterpret_cast<const char *>(receiveBuffer_.data());
        datagram.size = static_cast<size_t>(received);
        std::memcpy(datagram.source_address, &source, static_cast<size_t>(sourceLength));
        datagram.source_address_size = static_cast<size_t>(sourceLength);

        FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveBatch(1, static_cast<size_t>(received), false));
        return 1;
    }

    int receiveCoalesced(
        SOCKET socket, juice_udp_recv_datagram_t *datagrams, size_t capacity,
        int *socketError) {
        SOCKADDR_STORAGE source{};
        WSABUF buffer{};
        buffer.buf = reinterpret_cast<char *>(receiveBuffer_.data());
        buffer.len = static_cast<ULONG>(receiveBuffer_.size());

        WSAMSG message{};
        message.name = reinterpret_cast<LPSOCKADDR>(&source);
        message.namelen = sizeof(source);
        message.lpBuffers = &buffer;
        message.dwBufferCount = 1;
        message.Control.buf = controlBuffer_.data();
        message.Control.len = static_cast<ULONG>(controlBuffer_.size());

        DWORD received = 0;
        for (;;) {
            std::memset(&source, 0, sizeof(source));
            std::memset(controlBuffer_.data(), 0, controlBuffer_.size());

            received = 0;
            message.namelen = sizeof(source);
            message.Control.len = static_cast<ULONG>(controlBuffer_.size());
            message.dwFlags = 0;

            const int result = receiveMsg_(socket, &message, &received, nullptr, nullptr);
            FFL_P2P_DATAPATH_DIAGNOSTIC(diagnostics_.recordReceiveCall());

            if (result != SOCKET_ERROR) {
                break;
            }

            const int error = WSAGetLastError();
            if (isIgnoredUDPError(error)) {
                continue;
            }

            *socketError = error;
            return -1;
        }

        if (received == 0) {
            return 0;
        }

        if (message.namelen <= 0 || static_cast<size_t>(message.namelen) > kReceiveAddressSize) {
            *socketError = WSAEMSGSIZE;
            return -1;
        }

        size_t segmentSize = static_cast<size_t>(received);
        for (WSACMSGHDR *control = WSA_CMSG_FIRSTHDR(&message);
             control != nullptr; control = WSA_CMSG_NXTHDR(&message, control)) {
            if (control->cmsg_level == IPPROTO_UDP &&
                control->cmsg_type == UDP_COALESCED_INFO &&
                control->cmsg_len >= WSA_CMSG_LEN(sizeof(DWORD))) {
                const DWORD value = *reinterpret_cast<const DWORD *>(WSA_CMSG_DATA(control));
                if (value > 0 && value <= received) {
                    segmentSize = static_cast<size_t>(value);
                }
            }
        }

#if defined(FFL_P2P_DIAGNOSTICS)
        const size_t totalDatagrams =
            (static_cast<size_t>(received) + segmentSize - 1) / segmentSize;
#endif

        std::memcpy(pendingAddress_.data(), &source, static_cast<size_t>(message.namelen));
        pendingAddressSize_ = static_cast<size_t>(message.namelen);
        pendingOffset_ = 0;
        pendingBytes_ = static_cast<size_t>(received);
        pendingSegmentSize_ = segmentSize;

#if defined(FFL_P2P_DIAGNOSTICS)
        diagnostics_.recordReceiveBatch(totalDatagrams, static_cast<size_t>(received), true);
#endif

        return emitPending(datagrams, capacity);
    }

    std::array<unsigned char, kReceiveBufferSize> receiveBuffer_{};
    std::array<unsigned char, kReceiveAddressSize> pendingAddress_{};
    std::array<char, kReceiveControlSize> controlBuffer_{};
    size_t pendingAddressSize_{0};
    size_t pendingOffset_{0};
    size_t pendingBytes_{0};
    size_t pendingSegmentSize_{0};
    uintptr_t activeSocket_{UINTPTR_MAX};
    LPFN_WSARECVMSG receiveMsg_{nullptr};
    bool receiveCoalescingDisabled_{false};
    bool coalescingSupported_{false};
    bool coalescingEnabled_{false};
    bool initialized_{false};
};

} // namespace

#if defined(FFL_P2P_DIAGNOSTICS)
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(
    DatapathDiagnostics &diagnostics, const DatapathOptions &options) {
    return std::make_unique<WindowsDatapathBackend>(diagnostics, options);
}
#else
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(const DatapathOptions &options) {
    return std::make_unique<WindowsDatapathBackend>(options);
}
#endif


} // namespace ffl::datapath
