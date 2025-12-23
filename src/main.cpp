#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>

// your components:
#include "lf_queue.hpp"
#include "circular_buffer.hpp"
#include "producer.hpp"
#include "consumer.hpp"
#include "io_uring.hpp" 

int main(int argc, char** argv) {

    /* program flow: data comes into circular buffer, then to lf job queue to distribute to threads, then make consumer and producer threads*/

    // plan:
    // need to set up job queue (regular queue) (nodes will contain job numbers for arbitrary data for jobs to process)
    // set up lock free circular buffer
    // init network receiver backend (io_uring to simulate kernel bypass)
    // create producer class

    // done up to here
    
    // create consumer class
    // then create benchmark and CAS loop for RNW lock-free programming
    // join threads
    // cleanup and print logs

    std::cout << "Starting pipeline and network queue.\n";

    size_t num_consumers = 4;
    size_t num_producers = 4;

    // set up ciruclar buffer to simulate incoming pkts
    CircularBuffer buffer(int);

    // set up lock free queue for threads for workers to process
    LockFreeLinkedListQueue lock_free_queue(int);

    std::unique_ptr<Vector<Producer&>> producers = std::make_unique<Vector<Producer&>>(num_producers);
    std:unique_ptr<Vector<Consumer&>> consumers = std::make_unique<Vector<Consumer&>>(num_consumers);

    for (int i = 0; i < num_producers; ++i) {
        producers[i] = Producer<int>(buffer, lock_free_queue);
        producers[i].start();
    }




    for (auto& producer : producers) {
        producer.stop();
    }

    std::cout << "Done.\n";
    return 0;
}
