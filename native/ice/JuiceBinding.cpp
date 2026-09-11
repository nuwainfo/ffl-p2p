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

#include "ice/JuiceBinding.h"

#include "Agent.h"
#include "quic/Connection.h"

#include <juice/juice.h>

#include <stdexcept>
#include <string>

namespace ffl::ice {

JuiceBinding::~JuiceBinding() {
    detach();
}

void JuiceBinding::attach(void *agentHandle, quic::Connection *connection) {
    if (!agentHandle || !connection)
        throw std::invalid_argument("ICE binding requires agent and connection");

    if (agentHandle_)
        throw std::runtime_error("ICE binding is already attached");

    if (setFFLP2PAgentNativeDataSink(agentHandle, &JuiceBinding::onReceive, this) != 0)
        throw std::runtime_error("unable to attach QUIC runtime to ICE agent");

    agentHandle_ = agentHandle;
    connection_ = connection;
}

void JuiceBinding::detach() {
    if (!agentHandle_)
        return;

    clearFFLP2PAgentNativeDataSink(agentHandle_, &JuiceBinding::onReceive, this);

    agentHandle_ = nullptr;
    connection_ = nullptr;
}

bool JuiceBinding::isAttached() const {
    return agentHandle_ != nullptr;
}

namespace {

SendResult translateSendResult(int result, size_t size, size_t sentSize, const char *operation) {
    if (sentSize > size)
        throw std::runtime_error(std::string(operation) + " reported invalid send progress");

    if (result == JUICE_ERR_SUCCESS) {
        if (sentSize != size)
            throw std::runtime_error(std::string(operation) + " completed with a partial send");

        return {SendStatus::Complete, sentSize};
    }

    if (result == JUICE_ERR_AGAIN)
        return {SendStatus::Blocked, sentSize};

    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

} // namespace

SendResult JuiceBinding::send(const void *data, size_t size) {
    if (!agentHandle_)
        throw std::runtime_error("ICE binding is detached");

    const int result = sendFFLP2PAgentNative(agentHandle_, data, size);
    const size_t sentSize = result == JUICE_ERR_SUCCESS ? size : 0;

    return translateSendResult(result, size, sentSize, "libjuice QUIC send");
}

SendResult JuiceBinding::sendAggregate(const void *data, size_t size, size_t segmentSize) {
    if (!agentHandle_)
        throw std::runtime_error("ICE binding is detached");

    size_t sentSize = 0;
    const int result = sendFFLP2PAgentNativeAggregateProgress(
        agentHandle_, data, size, segmentSize, &sentSize);

    return translateSendResult(result, size, sentSize, "libjuice QUIC aggregate send");
}

void JuiceBinding::onReceive(void *userPtr, const void *data, size_t size) {
    auto *binding = static_cast<JuiceBinding *>(userPtr);
    if (!binding || !binding->connection_)
        return;

    binding->connection_->queueReceive(data, size);
}

} // namespace ffl::ice
