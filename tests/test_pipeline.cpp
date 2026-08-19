#include "adaptive_scheduler.hpp"
#include "circular_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    constexpr std::size_t TASK_COUNT = 20'000;
    constexpr std::size_t BUFFER_CAPACITY = 256;
    constexpr std::size_t WORKERS = 4;

    CircularBuffer<Task> ingress(BUFFER_CAPACITY);
    AdaptiveScheduler scheduler(WORKERS);

    std::atomic<bool> input_done{false};
    std::atomic<std::uint64_t> executed{0};
    std::atomic<std::uint64_t> sum{0};

    // One input thread: the sole producer of the SPSC circular buffer.
    std::thread input([&] {
        for (std::uint64_t i = 1; i <= TASK_COUNT; ++i) {
            Task task([&, i] {
                sum.fetch_add(i, std::memory_order_relaxed);
                executed.fetch_add(1, std::memory_order_relaxed);

                // Heterogeneous work so adaptive scheduling/stealing have
                // something meaningful to react to.
                if (i % 1000 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });

            while (!ingress.try_push(std::move(task))) {
                std::this_thread::yield();
            }
        }
        input_done.store(true, std::memory_order_release);
    });

    // One dispatcher thread: the sole consumer of the ring buffer.
    std::thread dispatcher([&] {
        Task task;
        while (!input_done.load(std::memory_order_acquire) || !ingress.empty()) {
            if (ingress.try_pop(task)) {
                scheduler.submit(std::move(task));
            } else {
                std::this_thread::yield();
            }
        }
    });

    input.join();
    dispatcher.join();
    scheduler.waitUntilIdle();

    const std::uint64_t expected_sum =
        static_cast<std::uint64_t>(TASK_COUNT) * (TASK_COUNT + 1) / 2;

    std::cout << "submitted=" << TASK_COUNT << '\n';
    std::cout << "executed=" << executed.load() << '\n';
    std::cout << "sum=" << sum.load() << '\n';
    std::cout << "expected_sum=" << expected_sum << '\n';
    std::cout << "scheduler_completed=" << scheduler.completed() << '\n';
    std::cout << "steals=" << scheduler.steals() << '\n';

    const bool ok = executed.load() == TASK_COUNT &&
                    sum.load() == expected_sum &&
                    scheduler.completed() == TASK_COUNT;

    scheduler.stop();

    if (!ok) {
        std::cerr << "pipeline test FAILED\n";
        return 1;
    }

    std::cout << "SPSC ingress -> adaptive scheduler -> LFQ workers PASSED\n";
    return 0;
}
