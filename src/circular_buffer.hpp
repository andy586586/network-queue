#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

// Bounded single-producer / single-consumer ring buffer.
//
// This keeps the original design:
//   - fixed-size circular storage
//   - front/back indices
//   - one unused slot so front == back means empty
//   - modulo wrap-around
//
// The original header had syntax/type errors and plain int indices. The
// indices are atomic here so one ingress thread may push while one dispatcher
// thread pops without a mutex.
template<typename T>
class CircularBuffer {
private:
    std::vector<std::optional<T>> buffer_;
    const std::size_t capacity_; // physical capacity = requested capacity + 1

    // Separate cache lines to reduce producer/consumer cache-line contention.
    alignas(64) std::atomic<std::size_t> front_{0};
    alignas(64) std::atomic<std::size_t> back_{0};

public:
    explicit CircularBuffer(std::size_t requested_capacity)
        : buffer_(requested_capacity + 1),
          capacity_(requested_capacity + 1) {
        if (requested_capacity == 0) {
            throw std::invalid_argument("CircularBuffer capacity must be > 0");
        }
    }

    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    bool try_push(const T& value) {
        const std::size_t back = back_.load(std::memory_order_relaxed);
        const std::size_t next = (back + 1) % capacity_;

        if (next == front_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[back] = value;
        back_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) {
        const std::size_t back = back_.load(std::memory_order_relaxed);
        const std::size_t next = (back + 1) % capacity_;

        if (next == front_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[back] = std::move(value);
        back_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& result) {
        const std::size_t front = front_.load(std::memory_order_relaxed);

        if (front == back_.load(std::memory_order_acquire)) {
            return false;
        }

        result = std::move(*buffer_[front]);
        buffer_[front].reset();
        front_.store((front + 1) % capacity_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return front_.load(std::memory_order_acquire) ==
               back_.load(std::memory_order_acquire);
    }

    bool full() const {
        const std::size_t back = back_.load(std::memory_order_acquire);
        return ((back + 1) % capacity_) ==
               front_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        const std::size_t front = front_.load(std::memory_order_acquire);
        const std::size_t back = back_.load(std::memory_order_acquire);

        if (back >= front) {
            return back - front;
        }
        return capacity_ - (front - back);
    }

    std::size_t capacity() const {
        return capacity_ - 1;
    }
};
