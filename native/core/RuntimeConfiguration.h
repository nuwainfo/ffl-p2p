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

#ifndef FFL_P2P_CORE_RUNTIME_CONFIGURATION_H
#define FFL_P2P_CORE_RUNTIME_CONFIGURATION_H

#include <cstdint>

namespace ffl::core {

class RuntimeConfiguration {
public:
    uint16_t workerCount() const;
    bool isSendSegmentationDisabled() const;
    bool isReceiveBatchingDisabled() const;
    bool isReceiveCoalescingDisabled() const;
};

} // namespace ffl::core

#endif
