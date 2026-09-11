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

#ifndef FFL_P2P_PLATFORM_PLATFORM_H
#define FFL_P2P_PLATFORM_PLATFORM_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace ffl::platform {

using Timestamp = uint64_t;
constexpr Timestamp InfiniteTimestamp = UINT64_MAX;

Timestamp getCurrentTimestampNS();
bool isLinux();
bool isEnvironmentEnabled(const char *name);

class SocketAddress {
public:
    SocketAddress() = default;

    const void *data() const;
    size_t size() const;

private:
    friend SocketAddress createLoopbackIPv4Address(uint16_t port);

    alignas(std::max_align_t) std::array<unsigned char, 128> storage_{};
    size_t size_{0};
};

SocketAddress createLoopbackIPv4Address(uint16_t port);

class Event {
public:
    Event() = default;
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    void set();
    void reset();
    void wait();
    bool waitUntil(Timestamp deadline);

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_{false};
};

class Thread {
public:
    using Function = std::function<void()>;

    Thread();
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    ~Thread();

    void start(Function function, size_t stackSize = 0);
    bool isJoinable() const;
    void join();
    size_t stackSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ffl::platform

#endif
