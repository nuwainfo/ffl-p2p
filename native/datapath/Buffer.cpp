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

#include "datapath/Buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ffl::datapath {

uint8_t *SendBuffer::data() {
    return payload_.data();
}

const uint8_t *SendBuffer::data() const {
    return payload_.data();
}

size_t SendBuffer::size() const {
    return payload_.size();
}

bool ReceiveBuffer::canAppend(size_t size) const {
    if (size > UINT16_MAX || size > kReceiveBufferPayloadSize)
        return false;

    if (packetCount_ >= kReceivePacketsPerBuffer)
        return false;

    return size <= kReceiveBufferPayloadSize - payloadSize_;
}

void ReceiveBuffer::append(const void *data, size_t size) {
    if (!data || size == 0)
        throw std::invalid_argument("receive packet must contain data");

    if (!canAppend(size))
        throw std::length_error("receive buffer has insufficient capacity");

    std::memcpy(payload_.data() + payloadSize_, data, size);
    packets_[packetCount_].offset = static_cast<uint16_t>(payloadSize_);
    packets_[packetCount_].length = static_cast<uint16_t>(size);
    payloadSize_ += size;
    ++packetCount_;
}

void ReceiveBuffer::reset() {
    payloadSize_ = 0;
    packetCount_ = 0;
    next_ = nullptr;
}

size_t ReceiveBuffer::getPacketCount() const {
    return packetCount_;
}

const ReceivePacket &ReceiveBuffer::getPacket(size_t index) const {
    if (index >= packetCount_)
        throw std::out_of_range("receive packet index is out of range");

    return packets_[index];
}

const uint8_t *ReceiveBuffer::getPacketData(const ReceivePacket &packet) const {
    return payload_.data() + packet.offset;
}

size_t StreamReceiveBuffer::getAvailable() const {
    return kStreamReceiveBufferPayloadSize - payloadSize_;
}

size_t StreamReceiveBuffer::size() const {
    return payloadSize_;
}

const uint8_t *StreamReceiveBuffer::data() const {
    return payload_.data();
}

size_t StreamReceiveBuffer::append(const void *data, size_t size) {
    if (!data || size == 0)
        return 0;

    const size_t copied = (std::min)(size, getAvailable());
    if (copied == 0)
        return 0;

    std::memcpy(payload_.data() + payloadSize_, data, copied);
    payloadSize_ += copied;

    return copied;
}

void StreamReceiveBuffer::reset() {
    payloadSize_ = 0;
    next_ = nullptr;
}

ReceiveBatch::ReceiveBatch(ReceiveBufferPool &pool) : pool_(&pool) {}

ReceiveBatch::~ReceiveBatch() {
    if (pool_ && head_)
        pool_->releaseChain(head_);
}

bool ReceiveBatch::empty() const {
    return packetCount_ == 0;
}

size_t ReceiveBatch::size() const {
    return packetCount_;
}

size_t ReceiveBatch::getBufferCount() const {
    return bufferCount_;
}

void ReceiveBatch::append(const void *data, size_t size) {
    if (!pool_)
        throw std::logic_error("receive batch has no buffer pool");

    if (!tail_ || !tail_->canAppend(size)) {
        ReceiveBuffer *buffer = pool_->acquire();

        if (!head_)
            head_ = buffer;
        else
            tail_->next_ = buffer;

        tail_ = buffer;
        ++bufferCount_;
    }

    tail_->append(data, size);
    ++packetCount_;
}

void ReceiveBatch::swap(ReceiveBatch &other) {
    if (pool_ != other.pool_)
        throw std::logic_error("receive batches must share the same pool");

    std::swap(head_, other.head_);
    std::swap(tail_, other.tail_);
    std::swap(packetCount_, other.packetCount_);
    std::swap(bufferCount_, other.bufferCount_);
}

StreamReceiveBatch::StreamReceiveBatch(StreamReceiveBufferPool &pool) : pool_(&pool) {}

StreamReceiveBatch::~StreamReceiveBatch() {
    if (pool_ && head_)
        pool_->releaseChain(head_);
}

bool StreamReceiveBatch::empty() const {
    return byteCount_ == 0;
}

size_t StreamReceiveBatch::size() const {
    return byteCount_;
}

size_t StreamReceiveBatch::getBufferCount() const {
    return bufferCount_;
}

void StreamReceiveBatch::append(const void *data, size_t size) {
    if (!pool_)
        throw std::logic_error("stream receive batch has no buffer pool");

    if (!data || size == 0)
        return;

    const auto *bytes = static_cast<const uint8_t *>(data);

    size_t offset = 0;
    while (offset < size) {
        if (!tail_ || tail_->getAvailable() == 0) {
            StreamReceiveBuffer *buffer = pool_->acquire();

            if (!head_)
                head_ = buffer;
            else
                tail_->next_ = buffer;

            tail_ = buffer;
            ++bufferCount_;
        }

        const size_t copied = tail_->append(bytes + offset, size - offset);
        if (copied == 0)
            throw std::logic_error("stream receive buffer made no progress");

        offset += copied;
        byteCount_ += copied;
    }
}

void StreamReceiveBatch::appendBatch(StreamReceiveBatch &other) {
    if (pool_ != other.pool_)
        throw std::logic_error("stream receive batches must share the same pool");

    if (!other.head_)
        return;

    if (!head_)
        head_ = other.head_;
    else
        tail_->next_ = other.head_;

    tail_ = other.tail_;
    byteCount_ += other.byteCount_;
    bufferCount_ += other.bufferCount_;

    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.byteCount_ = 0;
    other.bufferCount_ = 0;
}

void StreamReceiveBatch::swap(StreamReceiveBatch &other) {
    if (pool_ != other.pool_)
        throw std::logic_error("stream receive batches must share the same pool");

    std::swap(head_, other.head_);
    std::swap(tail_, other.tail_);
    std::swap(byteCount_, other.byteCount_);
    std::swap(bufferCount_, other.bufferCount_);
}

std::vector<uint8_t> StreamReceiveBatch::copyToVector() const {
    std::vector<uint8_t> result(byteCount_);
    size_t offset = 0;

    for (StreamReceiveBuffer *buffer = head_; buffer; buffer = buffer->next_) {
        if (buffer->size() == 0)
            continue;

        std::memcpy(result.data() + offset, buffer->data(), buffer->size());
        offset += buffer->size();
    }

    if (offset != byteCount_)
        throw std::logic_error("stream receive chain byte count mismatch");

    return result;
}

} // namespace ffl::datapath
