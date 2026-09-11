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

#include "platform/DatagramBatch.h"

#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstddef>
#include <sys/socket.h>
#include <sys/uio.h>

#if defined(__COSMOPOLITAN__)
/*
 * Cosmopolitan 4.0.2 exports the low-level sys_recvmmsg syscall stub but does
 * not expose GNU's struct mmsghdr or recvmmsg() wrapper in its public socket
 * headers. Keep the Linux userspace ABI declaration private to this adapter.
 *
 * The call is made only when platform::isLinux() is true. On other APE hosts
 * this symbol is never invoked.
 */
extern "C" long sys_recvmmsg(
    int socketHandle, void *messages, unsigned int messageCount,
    unsigned int flags, void *timeout);
#endif

namespace ffl::platform {

namespace {

constexpr size_t kLinuxUserMessageHeaderSize = 56;
constexpr size_t kLinuxMultiMessageHeaderSize = 64;

#if defined(__COSMOPOLITAN__)

struct LinuxUserMessageHeader {
    void *name;
    int nameLength;
    unsigned int namePadding;
    struct iovec *ioVectors;
    size_t ioVectorCount;
    void *control;
    size_t controlLength;
    unsigned int flags;
    unsigned int flagsPadding;
};

struct LinuxMultiMessageHeader {
    LinuxUserMessageHeader header;
    unsigned int length;
    unsigned int lengthPadding;
};

static_assert(sizeof(void *) == 8,
              "Cosmopolitan recvmmsg adapter requires a 64-bit Linux ABI");
static_assert(sizeof(LinuxUserMessageHeader) == kLinuxUserMessageHeaderSize,
              "unexpected Linux user msghdr ABI size");
static_assert(offsetof(LinuxUserMessageHeader, ioVectors) == 16,
              "unexpected Linux user msghdr iovec offset");
static_assert(offsetof(LinuxUserMessageHeader, flags) == 48,
              "unexpected Linux user msghdr flags offset");
static_assert(sizeof(LinuxMultiMessageHeader) == kLinuxMultiMessageHeaderSize,
              "unexpected Linux mmsghdr ABI size");
static_assert(offsetof(LinuxMultiMessageHeader, length) == kLinuxUserMessageHeaderSize,
              "unexpected Linux mmsghdr length offset");

int normalizeSyscallError(long result) {
    if (result == -1) {
        return errno;
    }

    if (result < 0 && result >= -4095) {
        return static_cast<int>(-result);
    }

    return EIO;
}

#endif

} // namespace

struct DatagramBatchReceiver::Impl {
    std::array<struct iovec, MaxBatchSize> ioVectors{};

#if defined(__linux__)
    std::array<struct mmsghdr, MaxBatchSize> messages{};
#elif defined(__COSMOPOLITAN__)
    std::array<LinuxMultiMessageHeader, MaxBatchSize> messages{};
#endif
};

DatagramBatchReceiver::DatagramBatchReceiver() : impl_(std::make_unique<Impl>()) {}
DatagramBatchReceiver::~DatagramBatchReceiver() = default;

bool DatagramBatchReceiver::isSupported() const {
#if defined(__linux__) || defined(__COSMOPOLITAN__)
    return platform::isLinux();
#else
    return false;
#endif
}

int DatagramBatchReceiver::receive(
    int socketHandle, DatagramReceiveBuffer *buffers,
    size_t capacity, int *socketError) {
    if (!buffers || !socketError || capacity == 0 || capacity > MaxBatchSize) {
        if (socketError) {
            *socketError = EINVAL;
        }
        return -1;
    }

    if (!isSupported()) {
        *socketError = ENOSYS;
        return -1;
    }

#if defined(__linux__)
    for (size_t index = 0; index < capacity; ++index) {
        DatagramReceiveBuffer &buffer = buffers[index];
        if (!buffer.data || buffer.dataCapacity == 0 ||
            !buffer.sourceAddress || buffer.sourceAddressCapacity == 0 ||
            buffer.sourceAddressCapacity >
                static_cast<size_t>((std::numeric_limits<socklen_t>::max)())) {
            *socketError = EINVAL;
            return -1;
        }

        std::memset(&impl_->messages[index], 0, sizeof(impl_->messages[index]));
        impl_->ioVectors[index].iov_base = buffer.data;
        impl_->ioVectors[index].iov_len = buffer.dataCapacity;

        struct msghdr &header = impl_->messages[index].msg_hdr;
        header.msg_name = buffer.sourceAddress;
        header.msg_namelen = static_cast<socklen_t>(buffer.sourceAddressCapacity);
        header.msg_iov = &impl_->ioVectors[index];
        header.msg_iovlen = 1;
    }

    const int result = recvmmsg(
        socketHandle, impl_->messages.data(), static_cast<unsigned int>(capacity), 0, nullptr);
    if (result < 0) {
        *socketError = errno;
        return -1;
    }

    for (int index = 0; index < result; ++index) {
        const struct mmsghdr &message = impl_->messages[static_cast<size_t>(index)];
        DatagramReceiveBuffer &buffer = buffers[static_cast<size_t>(index)];
        buffer.dataSize = static_cast<size_t>(message.msg_len);
        buffer.sourceAddressSize = static_cast<size_t>(message.msg_hdr.msg_namelen);
        buffer.flags = message.msg_hdr.msg_flags;
    }

    return result;
#elif defined(__COSMOPOLITAN__)
    for (size_t index = 0; index < capacity; ++index) {
        DatagramReceiveBuffer &buffer = buffers[index];
        if (!buffer.data || buffer.dataCapacity == 0 ||
            !buffer.sourceAddress || buffer.sourceAddressCapacity == 0 ||
            buffer.sourceAddressCapacity >
                static_cast<size_t>((std::numeric_limits<int>::max)())) {
            *socketError = EINVAL;
            return -1;
        }

        LinuxMultiMessageHeader &message = impl_->messages[index];
        std::memset(&message, 0, sizeof(message));
        impl_->ioVectors[index].iov_base = buffer.data;
        impl_->ioVectors[index].iov_len = buffer.dataCapacity;

        message.header.name = buffer.sourceAddress;
        message.header.nameLength = static_cast<int>(buffer.sourceAddressCapacity);
        message.header.ioVectors = &impl_->ioVectors[index];
        message.header.ioVectorCount = 1;
    }

    const long result = sys_recvmmsg(
        socketHandle, impl_->messages.data(), static_cast<unsigned int>(capacity), 0, nullptr);
    if (result < 0) {
        *socketError = normalizeSyscallError(result);
        return -1;
    }

    if (result > static_cast<long>(capacity)) {
        *socketError = EIO;
        return -1;
    }

    for (long index = 0; index < result; ++index) {
        const LinuxMultiMessageHeader &message =
            impl_->messages[static_cast<size_t>(index)];
        DatagramReceiveBuffer &buffer = buffers[static_cast<size_t>(index)];
        buffer.dataSize = static_cast<size_t>(message.length);
        buffer.sourceAddressSize = static_cast<size_t>(message.header.nameLength);
        buffer.flags = static_cast<int>(message.header.flags);
    }

    return static_cast<int>(result);
#else
    (void)socketHandle;
    *socketError = ENOSYS;
    return -1;
#endif
}

} // namespace ffl::platform
