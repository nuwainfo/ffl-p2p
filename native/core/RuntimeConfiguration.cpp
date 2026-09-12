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

#include "core/RuntimeConfiguration.h"

#include "platform/Platform.h"

#include <charconv>
#include <stdexcept>
#include <string>

namespace ffl::core {

namespace {

constexpr char kWorkerCountEnvironment[] = "FFL_P2P_QUIC_WORKERS";
constexpr char kDisableGSOEnvironment[] = "FFL_P2P_QUIC_DISABLE_GSO";
constexpr char kDisableReceiveBatchEnvironment[] =
    "FFL_P2P_QUIC_DISABLE_RECV_BATCH";
constexpr char kDisableReceiveCoalescingEnvironment[] =
    "FFL_P2P_QUIC_DISABLE_RECV_COALESCING";

constexpr uint16_t kDefaultWorkerCount = 2;
constexpr uint16_t kMaximumWorkerCount = 16;

bool isEnvironmentEnabled(const char *name) {
    const auto value = platform::getEnvironmentVariable(name);
    if (!value) {
        return false;
    }

    return *value == "1"
        || *value == "true"
        || *value == "yes"
        || *value == "on";
}

uint16_t parseWorkerCount(const std::string &value) {
    unsigned int parsed = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);

    const bool invalidInteger = result.ec != std::errc() || result.ptr != end;
    const bool outsideRange = parsed == 0 || parsed > kMaximumWorkerCount;
    if (invalidInteger || outsideRange) {
        throw std::invalid_argument(
            "FFL_P2P_QUIC_WORKERS must be an integer between 1 and 16");
    }

    return static_cast<uint16_t>(parsed);
}

} // namespace

uint16_t RuntimeConfiguration::workerCount() const {
    const auto configured = platform::getEnvironmentVariable(
        kWorkerCountEnvironment);
    if (!configured) {
        return kDefaultWorkerCount;
    }

    return parseWorkerCount(*configured);
}

bool RuntimeConfiguration::isSendSegmentationDisabled() const {
    return isEnvironmentEnabled(kDisableGSOEnvironment);
}

bool RuntimeConfiguration::isReceiveBatchingDisabled() const {
    return isEnvironmentEnabled(kDisableReceiveBatchEnvironment);
}

bool RuntimeConfiguration::isReceiveCoalescingDisabled() const {
    return isEnvironmentEnabled(kDisableReceiveCoalescingEnvironment);
}

} // namespace ffl::core
