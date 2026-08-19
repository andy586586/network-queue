#pragma once

#include "lf_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

struct Task {
    std::function<void()> fn;

    Task() = default;
    explicit Task(std::function<void()> work) : fn(std::move(work)) {}
};

class AdaptiveScheduler {
private:
    using Clock = std::chrono::steady_clock;

    struct Worker {
        LockFreeLinkedListQueue<Task> queue;
        std::thread thread;
        std::atomic<std::size_t> queued{0};
        std::atomic<std::uint64_t> avg_runtime_ns{100'000};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<bool> busy{false};
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{true};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::uint64_t> steals_{0};
    std::atomic<std::uint64_t> chooser_{0};

    std::size_t chooseWorker() {
        if (workers_.size() == 1) return 0;

        const std::uint64_t x = chooser_.fetch_add(0x9E3779B97F4A7C15ULL,
                                                   std::memory_order_relaxed);
        std::size_t a = static_cast<std::size_t>(x % workers_.size());
        std::size_t b = static_cast<std::size_t>(((x >> 32) ^ (x * 0xBF58476D1CE4E5B9ULL))
                                                 % workers_.size());
        if (a == b) b = (b + 1) % workers_.size();

        auto load_of = [this](std::size_t i) {
            const auto queued = workers_[i]->queued.load(std::memory_order_relaxed);
            const auto runtime = workers_[i]->avg_runtime_ns.load(std::memory_order_relaxed);
            const auto busy = workers_[i]->busy.load(std::memory_order_relaxed) ? 1ULL : 0ULL;
            return (static_cast<std::uint64_t>(queued) + busy) * runtime;
        };

        return load_of(a) <= load_of(b) ? a : b;
    }

    bool popOwn(std::size_t id, Task& task) {
        if (!workers_[id]->queue.pop(task)) {
            return false;
        }
        workers_[id]->queued.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    bool steal(std::size_t thief, Task& task) {
        if (workers_.size() <= 1) return false;

        const std::uint64_t seed = chooser_.fetch_add(0xD1B54A32D192ED03ULL,
                                                      std::memory_order_relaxed);

        for (int attempt = 0; attempt < 2; ++attempt) {
            std::size_t victim =
                static_cast<std::size_t>(((seed >> (attempt * 24)) ^ (seed * (attempt + 1)))
                                         % workers_.size());

            if (victim == thief) {
                victim = (victim + 1) % workers_.size();
            }

            if (workers_[victim]->queued.load(std::memory_order_relaxed) == 0) {
                continue;
            }

            if (workers_[victim]->queue.pop(task)) {
                workers_[victim]->queued.fetch_sub(1, std::memory_order_relaxed);
                steals_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }

        return false;
    }

    static void updateRuntime(Worker& worker, std::uint64_t runtime_ns) {
        std::uint64_t old = worker.avg_runtime_ns.load(std::memory_order_relaxed);
        while (true) {
            const std::uint64_t updated = (old * 4 + runtime_ns) / 5;
            if (worker.avg_runtime_ns.compare_exchange_weak(
                    old, updated, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void runWorker(std::size_t id) {
        Worker& worker = *workers_[id];

        while (running_.load(std::memory_order_acquire) ||
               worker.queued.load(std::memory_order_acquire) != 0) {
            Task task;
            if (!popOwn(id, task) && !steal(id, task)) {
                std::this_thread::yield();
                continue;
            }

            worker.busy.store(true, std::memory_order_release);
            active_.fetch_add(1, std::memory_order_relaxed);
            const auto start = Clock::now();

            if (task.fn) task.fn();

            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count();
            updateRuntime(worker, static_cast<std::uint64_t>(elapsed));
            worker.completed.fetch_add(1, std::memory_order_relaxed);
            active_.fetch_sub(1, std::memory_order_relaxed);
            worker.busy.store(false, std::memory_order_release);
        }
    }

public:
    explicit AdaptiveScheduler(std::size_t worker_count) {
        if (worker_count == 0) worker_count = 1;
        workers_.reserve(worker_count);

        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.push_back(std::make_unique<Worker>());
        }
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_[i]->thread = std::thread([this, i] { runWorker(i); });
        }
    }

    ~AdaptiveScheduler() { stop(); }

    AdaptiveScheduler(const AdaptiveScheduler&) = delete;
    AdaptiveScheduler& operator=(const AdaptiveScheduler&) = delete;

    void submit(Task task) {
        const std::size_t id = chooseWorker();
        workers_[id]->queued.fetch_add(1, std::memory_order_relaxed);
        workers_[id]->queue.push(std::move(task));
    }

    bool idle() const {
        if (active_.load(std::memory_order_acquire) != 0) return false;
        for (const auto& worker : workers_) {
            if (worker->queued.load(std::memory_order_acquire) != 0) return false;
        }
        return true;
    }

    void waitUntilIdle() const {
        while (!idle()) {
            std::this_thread::yield();
        }
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) worker->thread.join();
        }
    }

    std::uint64_t steals() const {
        return steals_.load(std::memory_order_relaxed);
    }

    std::uint64_t completed() const {
        std::uint64_t result = 0;
        for (const auto& worker : workers_) {
            result += worker->completed.load(std::memory_order_relaxed);
        }
        return result;
    }
};
