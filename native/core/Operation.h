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

#ifndef FFL_P2P_CORE_OPERATION_H
#define FFL_P2P_CORE_OPERATION_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ffl::core {

enum class OperationType : uint8_t {
    APIStart = 0,
    APISend = 1,
    FlushRecv = 2,
    FlushStreamRecv = 3,
    FlushSend = 4,
    TimerExpired = 5,
    Shutdown = 6,
};

class Completion {
public:
    void complete(std::string error = {});
    bool wait(double timeoutSeconds, std::string &error);

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_{false};
    std::string error_;
};

struct Operation {
    OperationType type{OperationType::FlushSend};
    std::vector<uint8_t> data;
    bool fin{false};
    std::shared_ptr<Completion> completion;
};

class OperationQueue {
public:
    void push(Operation operation);
    bool pop(Operation &operation);
    bool empty() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<Operation> operations_;
};

} // namespace ffl::core

#endif
