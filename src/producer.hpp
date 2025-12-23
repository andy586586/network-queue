#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>

#include "circular_buffer.hpp"
#include "lf_queue.hpp"

// producer ingests from circular buffer (since it has finite storage capacity) and puts it into lock free queue (has infinite storage capacity).

template<typename T>
class Producer {
    private:
        std::atomic<bool> running_;
        std::thread worker_;

        CircularBuffer<T>& ring_buffer_;
        LockFreeLinkedListQueue<T>& lock_free_queue_;

        void run_loop() {
            while (running_.load(std::memory_order_acquire)) { // mem ord acquire guarantees all the operations to read data happen after this point stay after this point.
                if (!ring_buffer_.empty()) {
                    lock_free_queue_.push(ring_buffer_.getFront());
                    ring_buffer_.pop_front();
                } else { // yield thread since no work is being done
                    std::this_thread::yield();
                }
            }
        }

    public:
        Producer(CircularBuffer<T>& ring_buffer_, LockFreeLinkedListQueue<T>& lock_free_queue_) : 
            ring_buffer_(ring_buffer_), 
            lock_free_queue_(lock_free_queue_),
            running_(false)
        {}

        void start() {
            running_.store(true, std::memory_order_release); // mem ord release guarantees all read/write ops (including init/constructors) happening before this point are published for all threads to see.
            worker_ = std::thread(&Producer::run_loop(), this); // creates parallel thread that runs the loop
        }

        /* note: store(mem ord release) and load(mem ord acquire) have a happens-before relationship. In this case, init queue and buffer/constructing the data etc. must happen before we call running loop. */
        
        void stop() {
            running_.store(false, std::memory_order_release);
            if (worker_.joinable) { worker_.join(); } // join thread = end run loop
        }
};