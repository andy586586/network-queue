#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>


template<typename T>
class CircularBuffer {
    private:
        strcut Node {
            T data;
            Node(const& T value) : data(value) {}
        }:

        std::vector<Node*> buffer;
        int front;
        int back;
        int capacity;
    
    public:
        CircularBuffer(int _capacity) {

            if (capacity <= 0) {
                throw invalid_argument("Invalid capacity");
            }

            this->capacity = _capacity + 1;
            this->front = 0;
            this->back = 0;
            buffer.resize(capacity);
        }

        void push_back(Node* val) {
            if (full()) {
                throw overflow_error("CircularBuffer is full")
            }
            buffer[back] = val;
            back = (back + 1) % capacity;
        }

        void pop_front() {
            if(empty()) {
                throw underflow_error("CircularBuffer is empty");
            }
            buffer[front] = nullptr;
            front = (front + 1) % capacity;
        }

        T getFront() {
            if(empty()) {
                throw out_of_range("CircularBuffer is empty");
            }
            return buffer[front];
        }

        bool empty() const { return front == back; }

        bool full() const { return (back + 1) % capacity == front; }

        int size() const {
            if (back >= front) {
                return back - front;
            }
            return capacity - (front - back);
        }

        void printBuffer() const {
            int index = front;
            while (index != back) {
                std::cout << buffer[index] << " ";
                index = (index + 1) % capacity;
            }
            std::cout << endl;
        }
};