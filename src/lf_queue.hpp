#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>

template<typename T>
class LockFreeLinkedListQueue {
    private:
        struct Node {
            T data;
            std::atomic<Node*> next;

            // MIL constructor
            Node(const T& value) : data(value), next(nullptr) {}
        };

        // init head and tail of queue
        std::atomic<Node*>  head;
        std::atomic<Node*> tail;

    public:
        // constructor
        LockFreeQueue() {
            Node* dummyhead = new Node(T);

            //std::atomic::store() and std::atomic::load() offer safe atomic set/get ops
            head.store(dummyhead);
            tail.store(dummyhead);
        }

        // destructor
        ~LockFreeQueue() {
            while (Node* old_head = head.load()) {
                // make temp new head (next) delete old, set oldhead to newhead
                head.store(old_head->next);
                delete old_head;
            }
        }
        
        /**
         * @brief function adds a new node to end of queue with value 
         * @param value is value of the new ndoe to add
         * @return nothing, will succeed eventually
         * 
         * Utilizes atomic operations to make sure value/node is successfully added to end of queue
         * Mechanism/plan: make sure tail is true tail of the queue, and that there is no node after the tail (in case queue tail hasn't been updated yet)
         */
        void push(const T& value) {
            Node* = new_node = new Node(value);
            Node* old_tail;
            
            // CAS (check and set) loop
            while (true) {
                old_tail = tail.load();
                Node* tail_next = old_tail->next;
                

                // check if tail hasn't changed from past
                // protects against case where another thread fully adds a new node AND updates queue tail at this time of execution in current thread
                if (old_tail == tail.load()) {
                    // check if tail next is actually last node
                    // protects against case where antoher thread fully adds a new node, BUT hasn't yet updated the tail of the queue 
                    if (tail_next == nullptr) {
                        if (old_tail->next.compare_exchange_weak(tail_next, new_node)) {
                            break; // success: added new node to tail_next
                            // this.compare_exchange_weak(expected, desired) compares this and expected.
                        } else {
                            ;
                            // in case of compare_exchange_weak failure, it will write *this value into expected (tail_next)
                            // since tail_next = old_tail->next, this will lead to tail_next != nullptr in next iteration,
                            // then tail will be moved forward

                        }
                    } else {
                        // tail is lagging, advance tail one node towards true tail
                        tail.compare_exchange_weak(old_tail, tail_next);
                    }
                }
                
                // once finished adding new node, update queue tail ptr to reflect new tail
                tail.compare_exchange_weak(old_tail, new_node);
                std::cout << "Pushed " << value << " into queue." << std::endl;
            }

        }

        /**
         * @brief pops the first value from the queue, returns it in result
         * @param result is a mutable reference to an initialized but no-value var in caller of type T
         * @return a boolean to represent success or failure (empty queue) of the pop operation
         * Mechanism/plan: If queue is empty, return false. If queue is not, implement
         */

        // return value: true or false. if method return value is true, then we also successfully update output param in caller with the result for value we got from queue
        bool pop(T& result) {

            while(true) {
                Node* old_head = head.load(); // dummyhead
                Node* old_tail = tail.load();
                Node* head_next = old_head->next;

                // make sure head hasn't been updated
                if (old_head == head.load()) {

                    // if head == tail, then queue possible empty (aka only node in q is dummyhead)
                    // however, could be non-empty if push was called and added node but hasn't updated tail yet
                    if (old_head == old_tail) {

                        // truly check if only dummy node in queue (empty)
                        if (head_next == nullptr) {
                            std::cout << "queue is empty (only dummyhead in queue)" << std::endl;
                            return false;
                        } else {
                            // means queue is not empy, but tail hasn't been updated to be caught up yet
                            tail.compare_exchange_weak(old_tail, head_next);
                        }
                    } else { // queue is not empty
                        if (head.compare_exchange_weak(old_head, head_next)) { // next node becomes dummy node from now on
                            result = head_next->data; // next node after dummy node is the data you want
                            delete old_head; // old dummy head deleted
                            std::cout << "popped " << result << " from queue." << std::endl;
                            return true;
                            // dummy head now contains the data you just returned, but treated as dummy head
                        }
                    }
                }
            }
        }

};