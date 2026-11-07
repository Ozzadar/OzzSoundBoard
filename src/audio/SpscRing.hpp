#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace ozzsb {

// Wait-free single-producer single-consumer ring buffer. Capacity rounds up to a power of two.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class SpscRing {
public:
    explicit SpscRing(std::size_t minCapacity)
        : buffer_(std::bit_ceil(std::max<std::size_t>(minCapacity, 2))), mask_(buffer_.size() - 1) {}

    std::size_t capacity() const { return buffer_.size(); }

    std::size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    bool push(const T& value) { return write(std::span(&value, 1)) == 1; }

    bool pop(T& value) { return read(std::span(&value, 1)) == 1; }

    // Producer side. Returns how many items were written.
    std::size_t write(std::span<const T> items) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t count = std::min(items.size(), capacity() - (head - tail));
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[(head + i) & mask_] = items[i];
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // Consumer side. Returns how many items were read.
    std::size_t read(std::span<T> out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t count = std::min(out.size(), head - tail);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = buffer_[(tail + i) & mask_];
        }
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Consumer side. Drops up to `count` of the oldest items.
    std::size_t discard(std::size_t count) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        count = std::min(count, head - tail);
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

private:
    std::vector<T> buffer_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace ozzsb
