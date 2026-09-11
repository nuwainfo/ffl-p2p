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

#include "core/Operation.h"

#include <chrono>
#include <utility>

namespace ffl::core {

void Completion::complete(std::string error) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (completed_)
            return;
        
        error_ = std::move(error);
        completed_ = true;
    }
    condition_.notify_all();
}

bool Completion::wait(double timeoutSeconds, std::string &error) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!completed_) {
        if (timeoutSeconds < 0.0) {
            condition_.wait(lock, [this]() { return completed_; });
        } else {
            const auto timeout = std::chrono::duration<double>(timeoutSeconds);
            if (!condition_.wait_for(lock, timeout, [this]() { return completed_; }))
                return false;
        }
    }
    error = error_;
    return true;
}

void OperationQueue::push(Operation operation) {
    std::lock_guard<std::mutex> guard(mutex_);
    operations_.push_back(std::move(operation));
}

bool OperationQueue::pop(Operation &operation) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (operations_.empty())
        return false;
    
    operation = std::move(operations_.front());
    operations_.pop_front();
    return true;
}

bool OperationQueue::empty() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return operations_.empty();
}

size_t OperationQueue::size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return operations_.size();
}

} // namespace ffl::core
