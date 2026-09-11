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

#include "core/TimerWheel.h"

#include "quic/Connection.h"

namespace ffl::core {

void TimerWheel::update(const std::shared_ptr<quic::Connection> &connection,
                        platform::Timestamp expiry) {
    if (!connection)
        return;
    
    if (expiry == platform::InfiniteTimestamp) {
        remove(connection.get());
        return;
    }
    
    entries_[connection.get()] = Entry{connection, expiry};
}

void TimerWheel::remove(quic::Connection *connection) {
    if (connection)
        entries_.erase(connection);
}

platform::Timestamp TimerWheel::getNextExpiry() const {
    platform::Timestamp next = platform::InfiniteTimestamp;
    
    for (const auto &item : entries_) {
        if (item.second.connection.expired())
            continue;
        if (item.second.expiry < next)
            next = item.second.expiry;
    }
    
    return next;
}

std::vector<std::shared_ptr<quic::Connection>> TimerWheel::takeExpired(platform::Timestamp now) {
    std::vector<std::shared_ptr<quic::Connection>> expired;
    
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        std::shared_ptr<quic::Connection> connection = iterator->second.connection.lock();
        if (!connection) {
            iterator = entries_.erase(iterator);
            continue;
        }
        
        if (iterator->second.expiry > now) {
            ++iterator;
            continue;
        }
        
        expired.push_back(std::move(connection));
        iterator = entries_.erase(iterator);
    }
    return expired;
}

size_t TimerWheel::size() const {
    return entries_.size();
}

} // namespace ffl::core
