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

#ifndef FFL_P2P_CORE_TIMER_WHEEL_H
#define FFL_P2P_CORE_TIMER_WHEEL_H

#include "platform/Platform.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace ffl::quic {
class Connection;
}

namespace ffl::core {

/* Worker-owned deadline index. Each connection publishes one earliest QUIC
 * deadline, and only its owning Worker mutates or consumes that deadline. */
class TimerWheel {
public:
    void update(const std::shared_ptr<quic::Connection> &connection,
                platform::Timestamp expiry);
    void remove(quic::Connection *connection);
    platform::Timestamp getNextExpiry() const;
    std::vector<std::shared_ptr<quic::Connection>> takeExpired(platform::Timestamp now);
    size_t size() const;

private:
    struct Entry {
        std::weak_ptr<quic::Connection> connection;
        platform::Timestamp expiry{platform::InfiniteTimestamp};
    };

    std::unordered_map<quic::Connection *, Entry> entries_;
};

} // namespace ffl::core

#endif
