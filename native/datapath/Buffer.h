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

#ifndef FFL_P2P_DATAPATH_BUFFER_H
#define FFL_P2P_DATAPATH_BUFFER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ffl::datapath {

constexpr size_t kSendBufferPayloadSize = 64 * 1024;
constexpr size_t kReceiveBufferPayloadSize = 64 * 1024;
constexpr size_t kReceivePacketsPerBuffer = 64;
constexpr size_t kInitialReceiveBufferCount = 32;
constexpr size_t kReceiveBufferGrowCount = 16;
constexpr size_t kStreamReceiveBufferPayloadSize = 64 * 1024;
constexpr size_t kInitialStreamReceiveBufferCount = 32;
constexpr size_t kStreamReceiveBufferGrowCount = 16;

template <typename BufferType, size_t InitialCount, size_t GrowCount>
class BufferPool;

#if defined(FFL_P2P_DIAGNOSTICS)
struct BufferPoolStats {
    uint64_t slabsAllocated{0};
    uint64_t buffersAllocated{0};
    uint64_t acquireCount{0};
    uint64_t reuseCount{0};
    uint64_t dynamicGrowths{0};
    uint64_t freeBuffers{0};
};
#endif

class SendBuffer {
public:
    SendBuffer() = default;
    SendBuffer(const SendBuffer &) = delete;
    SendBuffer &operator=(const SendBuffer &) = delete;

    uint8_t *data();
    const uint8_t *data() const;
    size_t size() const;

private:
    std::array<uint8_t, kSendBufferPayloadSize> payload_{};
};

struct ReceivePacket {
    uint16_t offset{0};
    uint16_t length{0};
};

static_assert(kReceiveBufferPayloadSize <= static_cast<size_t>(UINT16_MAX) + 1,
              "receive buffer offsets must fit in uint16_t");

class ReceiveBuffer {
public:
    ReceiveBuffer() = default;
    ReceiveBuffer(const ReceiveBuffer &) = delete;
    ReceiveBuffer &operator=(const ReceiveBuffer &) = delete;

    bool canAppend(size_t size) const;
    void append(const void *data, size_t size);
    void reset();

    size_t getPacketCount() const;
    const ReceivePacket &getPacket(size_t index) const;
    const uint8_t *getPacketData(const ReceivePacket &packet) const;

private:
    template <typename BufferType, size_t InitialCount, size_t GrowCount>
    friend class BufferPool;
    friend class ReceiveBatch;

    std::array<uint8_t, kReceiveBufferPayloadSize> payload_{};
    std::array<ReceivePacket, kReceivePacketsPerBuffer> packets_{};
    size_t payloadSize_{0};
    size_t packetCount_{0};
    ReceiveBuffer *next_{nullptr};
#if defined(FFL_P2P_DIAGNOSTICS)
    bool acquiredBefore_{false};
#endif
};

class StreamReceiveBuffer {
public:
    StreamReceiveBuffer() = default;
    StreamReceiveBuffer(const StreamReceiveBuffer &) = delete;
    StreamReceiveBuffer &operator=(const StreamReceiveBuffer &) = delete;

    size_t getAvailable() const;
    size_t size() const;
    const uint8_t *data() const;
    size_t append(const void *data, size_t size);
    void reset();

private:
    template <typename BufferType, size_t InitialCount, size_t GrowCount>
    friend class BufferPool;
    friend class StreamReceiveBatch;

    std::array<uint8_t, kStreamReceiveBufferPayloadSize> payload_{};
    size_t payloadSize_{0};
    StreamReceiveBuffer *next_{nullptr};
#if defined(FFL_P2P_DIAGNOSTICS)
    bool acquiredBefore_{false};
#endif
};

template <typename BufferType, size_t InitialCount, size_t GrowCount>
class BufferPool {
public:
    BufferPool() {
        slabs_.reserve(4);
        grow(InitialCount, false);
    }
    BufferPool(const BufferPool &) = delete;
    BufferPool &operator=(const BufferPool &) = delete;

    BufferType *acquire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!freeHead_)
            grow(GrowCount, true);

        BufferType *buffer = freeHead_;
        freeHead_ = buffer->next_;
        buffer->reset();
        
#if defined(FFL_P2P_DIAGNOSTICS)
        --freeBuffers_;
        ++acquireCount_;

        if (buffer->acquiredBefore_)
            ++reuseCount_;
        else
            buffer->acquiredBefore_ = true;
#endif

        return buffer;
    }

    void release(BufferType *buffer) {
        if (!buffer)
            return;

        std::lock_guard<std::mutex> guard(mutex_);
        buffer->reset();
        buffer->next_ = freeHead_;
        freeHead_ = buffer;
        
#if defined(FFL_P2P_DIAGNOSTICS)
        ++freeBuffers_;
#endif
    }

    void releaseChain(BufferType *head) {
        if (!head)
            return;

        std::lock_guard<std::mutex> guard(mutex_);
        BufferType *buffer = head;
        while (buffer) {
            BufferType *next = buffer->next_;
            buffer->reset();
            buffer->next_ = freeHead_;
            freeHead_ = buffer;
            
#if defined(FFL_P2P_DIAGNOSTICS)
            ++freeBuffers_;
#endif
            buffer = next;
        }
    }

#if defined(FFL_P2P_DIAGNOSTICS)
    BufferPoolStats getStats() const {
        std::lock_guard<std::mutex> guard(mutex_);
        BufferPoolStats result;

        result.slabsAllocated = slabsAllocated_;
        result.buffersAllocated = buffersAllocated_;
        result.acquireCount = acquireCount_;
        result.reuseCount = reuseCount_;
        result.dynamicGrowths = dynamicGrowths_;
        result.freeBuffers = freeBuffers_;
        return result;
    }
#endif

private:
    void grow(size_t count, bool dynamicGrowth) {
        auto slab = std::make_unique<BufferType[]>(count);

        for (size_t index = 0; index < count; ++index) {
            BufferType *buffer = &slab[index];
            buffer->next_ = freeHead_;
            freeHead_ = buffer;
        }

        slabs_.push_back(std::move(slab));
        
#if defined(FFL_P2P_DIAGNOSTICS)
        ++slabsAllocated_;
        buffersAllocated_ += count;
        freeBuffers_ += count;

        if (dynamicGrowth)
            ++dynamicGrowths_;
#else
        (void)dynamicGrowth;
#endif
    }

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<BufferType[]>> slabs_;
    BufferType *freeHead_{nullptr};
    
#if defined(FFL_P2P_DIAGNOSTICS)
    uint64_t slabsAllocated_{0};
    uint64_t buffersAllocated_{0};
    uint64_t acquireCount_{0};
    uint64_t reuseCount_{0};
    uint64_t dynamicGrowths_{0};
    uint64_t freeBuffers_{0};
#endif
};

using ReceiveBufferPool = BufferPool<
    ReceiveBuffer, kInitialReceiveBufferCount, kReceiveBufferGrowCount>;
using StreamReceiveBufferPool = BufferPool<
    StreamReceiveBuffer, kInitialStreamReceiveBufferCount, kStreamReceiveBufferGrowCount>;
    
#if defined(FFL_P2P_DIAGNOSTICS)
using ReceiveBufferPoolStats = BufferPoolStats;
using StreamReceiveBufferPoolStats = BufferPoolStats;
#endif

class ReceiveBatch {
public:
    explicit ReceiveBatch(ReceiveBufferPool &pool);
    ReceiveBatch(const ReceiveBatch &) = delete;
    ReceiveBatch &operator=(const ReceiveBatch &) = delete;
    ~ReceiveBatch();

    bool empty() const;
    size_t size() const;
    size_t getBufferCount() const;
    void append(const void *data, size_t size);
    void swap(ReceiveBatch &other);

    template <typename Callback>
    void forEachPacket(Callback &&callback) const {
        for (ReceiveBuffer *buffer = head_; buffer; buffer = buffer->next_) {
            for (size_t index = 0; index < buffer->getPacketCount(); ++index) {
                const ReceivePacket &packet = buffer->getPacket(index);
                callback(buffer->getPacketData(packet), static_cast<size_t>(packet.length));
            }
        }
    }

private:
    ReceiveBufferPool *pool_{nullptr};
    ReceiveBuffer *head_{nullptr};
    ReceiveBuffer *tail_{nullptr};
    size_t packetCount_{0};
    size_t bufferCount_{0};
};

class StreamReceiveBatch {
public:
    explicit StreamReceiveBatch(StreamReceiveBufferPool &pool);
    StreamReceiveBatch(const StreamReceiveBatch &) = delete;
    StreamReceiveBatch &operator=(const StreamReceiveBatch &) = delete;
    StreamReceiveBatch(StreamReceiveBatch &&other) noexcept;
    StreamReceiveBatch &operator=(StreamReceiveBatch &&other) = delete;
    ~StreamReceiveBatch();

    bool empty() const;
    size_t size() const;
    size_t getBufferCount() const;
    void append(const void *data, size_t size);
    void appendBatch(StreamReceiveBatch &other);
    void swap(StreamReceiveBatch &other);
    void copyTo(void *destination, size_t capacity) const;

private:
    StreamReceiveBufferPool *pool_{nullptr};
    StreamReceiveBuffer *head_{nullptr};
    StreamReceiveBuffer *tail_{nullptr};
    size_t byteCount_{0};
    size_t bufferCount_{0};
};

} // namespace ffl::datapath

#endif
