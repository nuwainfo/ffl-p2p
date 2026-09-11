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

#ifndef FFL_P2P_ICE_JUICE_BINDING_H
#define FFL_P2P_ICE_JUICE_BINDING_H

#include <cstddef>
#include <cstdint>

namespace ffl::quic {
class Connection;
}

namespace ffl::ice {

enum class SendStatus : uint8_t {
    Complete = 0,
    Blocked = 1,
};

struct SendResult {
    SendStatus status{SendStatus::Complete};
    size_t sentSize{0};
};

/*
 * Narrow ICE binding used by the native QUIC transport. libjuice owns ICE/STUN/path
 * selection; this class only adapts the selected data path to the QUIC worker.
 */
class JuiceBinding {
public:
    JuiceBinding() = default;
    JuiceBinding(const JuiceBinding &) = delete;
    JuiceBinding &operator=(const JuiceBinding &) = delete;
    ~JuiceBinding();

    void attach(void *agentHandle, quic::Connection *connection);
    void detach();
    bool isAttached() const;

    SendResult send(const void *data, size_t size);
    SendResult sendAggregate(const void *data, size_t size, size_t segmentSize);

private:
    static void onReceive(void *userPtr, const void *data, size_t size);

    void *agentHandle_{nullptr};
    quic::Connection *connection_{nullptr};
};

} // namespace ffl::ice

#endif
