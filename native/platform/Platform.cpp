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

#include "platform/Platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#if defined(__COSMOPOLITAN__)
// cosmo.h enables _COSMO_SOURCE and exposes IsLinux() for APE runtime dispatch.
#include <cosmo.h>
#endif
#endif

namespace ffl::platform {

namespace {

struct ThreadStartContext {
    explicit ThreadStartContext(Thread::Function value) : function(std::move(value)) {}

    Thread::Function function;
};

#if defined(_WIN32)

unsigned __stdcall runThread(void *argument) {
    std::unique_ptr<ThreadStartContext> context(static_cast<ThreadStartContext *>(argument));
    context->function();
    return 0;
}

#else

void *runThread(void *argument) {
    std::unique_ptr<ThreadStartContext> context(static_cast<ThreadStartContext *>(argument));
    context->function();
    return nullptr;
}

#endif

} // namespace

struct Thread::Impl {
#if defined(_WIN32)
    HANDLE handle{nullptr};
#else
    pthread_t handle{};
#endif
    bool joinable{false};
    size_t stackSize{0};
};

bool isLinux() {
#if defined(_WIN32)
    return false;
#elif defined(__COSMOPOLITAN__)
    return IsLinux();
#elif defined(__linux__)
    return true;
#else
    return false;
#endif
}

Timestamp getCurrentTimestampNS() {
    using namespace std::chrono;
    return static_cast<Timestamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

const void *SocketAddress::data() const {
    return storage_.data();
}

size_t SocketAddress::size() const {
    return size_;
}

SocketAddress createLoopbackIPv4Address(uint16_t port) {
    SocketAddress address;
    sockaddr_in nativeAddress{};

    nativeAddress.sin_family = AF_INET;
    nativeAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    nativeAddress.sin_port = htons(port);

    static_assert(sizeof(nativeAddress) <= sizeof(address.storage_),
                  "platform socket address storage is too small");

    std::memcpy(address.storage_.data(), &nativeAddress, sizeof(nativeAddress));
    address.size_ = sizeof(nativeAddress);

    return address;
}

std::optional<std::string> getEnvironmentVariable(const char *name) {
    if (!name || !*name)
        return std::nullopt;

#if defined(_MSC_VER)
    char *value = nullptr;
    size_t valueLength = 0;

    if (_dupenv_s(&value, &valueLength, name) != 0 || !value)
        return std::nullopt;

    const std::unique_ptr<char, decltype(&std::free)> ownedValue(value, &std::free);
    if (valueLength <= 1)
        return std::nullopt;

    return std::string(value);
#else
    const char *value = std::getenv(name);
    if (!value || !*value)
        return std::nullopt;

    return std::string(value);
#endif
}

void Event::set() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        signaled_ = true;
    }

    condition_.notify_one();
}

void Event::reset() {
    std::lock_guard<std::mutex> guard(mutex_);
    signaled_ = false;
}

void Event::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return signaled_; });
    signaled_ = false;
}

bool Event::waitUntil(Timestamp deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (signaled_) {
        signaled_ = false;
        return true;
    }

    if (deadline == InfiniteTimestamp) {
        condition_.wait(lock, [this]() { return signaled_; });
        signaled_ = false;
        return true;
    }

    const Timestamp current = getCurrentTimestampNS();
    if (deadline <= current)
        return false;

    const auto remaining = std::chrono::nanoseconds(deadline - current);
    const bool awakened = condition_.wait_for(lock, remaining, [this]() { return signaled_; });
    if (awakened)
        signaled_ = false;

    return awakened;
}

Thread::Thread() : impl_(std::make_unique<Impl>()) {}

Thread::~Thread() {
    if (!isJoinable())
        return;

    try {
        join();
    } catch (...) {
        std::terminate();
    }
}

void Thread::start(Function function, size_t stackSize) {
    if (!function)
        throw std::invalid_argument("thread function must be callable");

    if (isJoinable())
        throw std::runtime_error("thread is already running");

    auto context = std::make_unique<ThreadStartContext>(std::move(function));

#if defined(_WIN32)
    if (stackSize > static_cast<size_t>((std::numeric_limits<unsigned>::max)()))
        throw std::invalid_argument("thread stack size exceeds Windows limit");

    unsigned threadID = 0;
    const uintptr_t handle = _beginthreadex(
        nullptr, static_cast<unsigned>(stackSize), &runThread,
        context.get(), 0, &threadID);

    if (handle == 0)
        throw std::runtime_error("unable to create thread: errno=" + std::to_string(errno));

    impl_->handle = reinterpret_cast<HANDLE>(handle);
#else
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0)
        throw std::runtime_error("pthread_attr_init failed: " + std::to_string(result));

    if (stackSize != 0) {
        result = pthread_attr_setstacksize(&attributes, stackSize);
        if (result != 0) {
            pthread_attr_destroy(&attributes);
            throw std::runtime_error(
                "pthread_attr_setstacksize failed: " + std::to_string(result));
        }
    }

    result = pthread_create(&impl_->handle, &attributes, &runThread, context.get());
    if (result != 0) {
        pthread_attr_destroy(&attributes);
        throw std::runtime_error("pthread_create failed: " + std::to_string(result));
    }

    (void)pthread_attr_destroy(&attributes);
#endif

    context.release();
    impl_->joinable = true;
    impl_->stackSize = stackSize;
}

bool Thread::isJoinable() const {
    return impl_ && impl_->joinable;
}

void Thread::join() {
    if (!isJoinable())
        return;

#if defined(_WIN32)
    const DWORD result = WaitForSingleObject(impl_->handle, INFINITE);
    if (result != WAIT_OBJECT_0)
        throw std::runtime_error("unable to join thread: " + std::to_string(GetLastError()));

    if (!CloseHandle(impl_->handle))
        throw std::runtime_error("unable to close thread handle: " + std::to_string(GetLastError()));

    impl_->handle = nullptr;
#else
    const int result = pthread_join(impl_->handle, nullptr);
    if (result != 0)
        throw std::runtime_error("pthread_join failed: " + std::to_string(result));
#endif

    impl_->joinable = false;
}

size_t Thread::stackSize() const {
    return impl_ ? impl_->stackSize : 0;
}

} // namespace ffl::platform
