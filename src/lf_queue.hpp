#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <unordered_set>
#include <utility>

// Michael-Scott-style lock-free linked-list queue.
//
// The original head/tail CAS algorithm and dummy-node design are preserved.
// Hazard pointers are added only for safe node lifetime management: a node that
// has been removed from the queue is "retired" first and is deleted only after
// no thread advertises that it may still dereference that node.
template<typename T>
class LockFreeLinkedListQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};

        // Used only after this node has been logically removed from the queue.
        std::atomic<Node*> retired_next{nullptr};

        explicit Node(const T& value) : data(value) {}
        explicit Node(T&& value) : data(std::move(value)) {}
    };

    std::atomic<Node*> head{nullptr};
    std::atomic<Node*> tail{nullptr};

    // ---------- Hazard-pointer domain ----------
    // Two hazards per participating thread:
    //   hazard 0: current head/tail node being dereferenced
    //   hazard 1: head->next while a consumer reads its value
    static constexpr std::size_t MAX_HAZARD_POINTERS = 256;
    static constexpr std::size_t RECLAIM_THRESHOLD = 64;

    inline static std::array<std::atomic<Node*>, MAX_HAZARD_POINTERS> hazards_{};
    inline static std::atomic<std::size_t> next_hazard_slot_{0};

    struct HazardPair {
        std::size_t first;
        std::size_t second;
    };

    static HazardPair acquireHazardPair() {
        const std::size_t first =
            next_hazard_slot_.fetch_add(2, std::memory_order_relaxed);

        if (first + 1 >= MAX_HAZARD_POINTERS) {
            throw std::runtime_error("LockFreeLinkedListQueue: hazard slots exhausted");
        }

        return {first, first + 1};
    }

    static HazardPair hazardPairForThisThread() {
        // One pair per thread per queue T specialization. It can safely be
        // shared by multiple queue instances because a thread is executing
        // only one queue operation at a time on this call path.
        thread_local const HazardPair pair = acquireHazardPair();
        return pair;
    }

    static std::atomic<Node*>& hazard(std::size_t slot) {
        return hazards_[slot];
    }

    static void clearHazards(const HazardPair& hp) {
        hazard(hp.second).store(nullptr, std::memory_order_seq_cst);
        hazard(hp.first).store(nullptr, std::memory_order_seq_cst);
    }

    // ---------- Retired-node reclamation ----------
    // Removed nodes are pushed onto this queue's lock-free retired stack.
    // Periodically one thread scans the hazard pointers and frees only nodes
    // which no thread can still dereference.
    std::atomic<Node*> retired_head_{nullptr};
    std::atomic<std::size_t> retired_count_{0};
    std::atomic_flag reclaiming_ = ATOMIC_FLAG_INIT;

    void pushRetired(Node* node, bool update_count = true) {
        Node* old_retired = retired_head_.load(std::memory_order_relaxed);
        do {
            node->retired_next.store(old_retired, std::memory_order_relaxed);
        } while (!retired_head_.compare_exchange_weak(
            old_retired,
            node,
            std::memory_order_release,
            std::memory_order_relaxed));

        if (update_count) {
            retired_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void retire(Node* node) {
        pushRetired(node);

        if (retired_count_.load(std::memory_order_relaxed) >= RECLAIM_THRESHOLD) {
            tryReclaim();
        }
    }

    void tryReclaim() {
        // Reclamation itself does not need to run concurrently in many threads.
        // Queue push/pop remain lock-free; if another thread is already scanning,
        // this thread simply skips the maintenance pass.
        if (reclaiming_.test_and_set(std::memory_order_acquire)) {
            return;
        }

        Node* retired = retired_head_.exchange(nullptr, std::memory_order_acq_rel);
        retired_count_.store(0, std::memory_order_relaxed);

        std::unordered_set<Node*> protected_nodes;
        protected_nodes.reserve(MAX_HAZARD_POINTERS);

        for (auto& slot : hazards_) {
            if (Node* p = slot.load(std::memory_order_seq_cst)) {
                protected_nodes.insert(p);
            }
        }

        std::size_t still_protected = 0;
        while (retired != nullptr) {
            Node* next_retired =
                retired->retired_next.load(std::memory_order_relaxed);

            if (protected_nodes.find(retired) != protected_nodes.end()) {
                pushRetired(retired, false);
                ++still_protected;
            } else {
                delete retired;
            }

            retired = next_retired;
        }

        if (still_protected != 0) {
            retired_count_.fetch_add(still_protected, std::memory_order_relaxed);
        }

        reclaiming_.clear(std::memory_order_release);
    }

    void forceReclaimAll() {
        // Destruction requires that callers have already joined/stopped all
        // threads using the queue. At that point no node can legitimately be
        // dereferenced by another queue operation.
        Node* retired = retired_head_.exchange(nullptr, std::memory_order_acq_rel);
        while (retired != nullptr) {
            Node* next_retired =
                retired->retired_next.load(std::memory_order_relaxed);
            delete retired;
            retired = next_retired;
        }
        retired_count_.store(0, std::memory_order_relaxed);
    }

public:
    LockFreeLinkedListQueue() {
        Node* dummy_head = new Node(T{});
        head.store(dummy_head, std::memory_order_relaxed);
        tail.store(dummy_head, std::memory_order_relaxed);
    }

    LockFreeLinkedListQueue(const LockFreeLinkedListQueue&) = delete;
    LockFreeLinkedListQueue& operator=(const LockFreeLinkedListQueue&) = delete;

    ~LockFreeLinkedListQueue() {
        // The queue must no longer be in use when its destructor runs.
        forceReclaimAll();

        Node* node = head.load(std::memory_order_relaxed);
        while (node != nullptr) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    /**
     * Adds a node to the end of the queue.
     *
     * This is the same tail-helping CAS structure as the original queue. The
     * added hazard protects old_tail while we dereference old_tail->next.
     */
    void push(const T& value) {
        Node* new_node = new Node(value);
        const HazardPair hp = hazardPairForThisThread();

        while (true) {
            Node* old_tail = tail.load(std::memory_order_acquire);
            hazard(hp.first).store(old_tail, std::memory_order_seq_cst);

            // The tail may have changed between load() and hazard publication.
            if (old_tail != tail.load(std::memory_order_acquire)) {
                continue;
            }

            Node* tail_next = old_tail->next.load(std::memory_order_acquire);

            // Make sure tail is still the tail value we examined.
            if (old_tail != tail.load(std::memory_order_acquire)) {
                continue;
            }

            if (tail_next == nullptr) {
                // True tail: try to link our node after it.
                if (old_tail->next.compare_exchange_weak(
                        tail_next,
                        new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    // Linking the node is the linearization point. Advancing
                    // tail is only an optimization; another thread can help.
                    tail.compare_exchange_strong(
                        old_tail,
                        new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);

                    clearHazards(hp);
                    return;
                }
            } else {
                // Another producer linked a node but tail is lagging. Help it.
                tail.compare_exchange_weak(
                    old_tail,
                    tail_next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

    void push(T&& value) {
        // Keep the queue algorithm in one place. The node itself can still take
        // ownership of a move-only-friendly T if this overload is later split
        // into an emplace path; for now preserve the original copy-based API.
        push(static_cast<const T&>(value));
    }

    /**
     * Pops the first value into result. Returns false if the queue is empty.
     *
     * Two hazards are necessary: one for old_head and one for head_next. A
     * competing consumer can advance the head twice, so protecting only the old
     * dummy node is not enough while we read head_next->data.
     */
    bool pop(T& result) {
        const HazardPair hp = hazardPairForThisThread();

        while (true) {
            Node* old_head = head.load(std::memory_order_acquire);
            hazard(hp.first).store(old_head, std::memory_order_seq_cst);

            if (old_head != head.load(std::memory_order_acquire)) {
                continue;
            }

            Node* old_tail = tail.load(std::memory_order_acquire);
            Node* head_next = old_head->next.load(std::memory_order_acquire);

            // Protect next before reading its data, then validate that it is
            // still the node following the protected head.
            hazard(hp.second).store(head_next, std::memory_order_seq_cst);

            if (old_head != head.load(std::memory_order_acquire) ||
                head_next != old_head->next.load(std::memory_order_acquire)) {
                hazard(hp.second).store(nullptr, std::memory_order_seq_cst);
                continue;
            }

            if (old_head == old_tail) {
                if (head_next == nullptr) {
                    clearHazards(hp);
                    return false;
                }

                // Queue is non-empty but tail has not caught up yet. Help it.
                tail.compare_exchange_weak(
                    old_tail,
                    head_next,
                    std::memory_order_release,
                    std::memory_order_relaxed);

                hazard(hp.second).store(nullptr, std::memory_order_seq_cst);
                continue;
            }

            // Read the value while head_next is hazard-protected. If our CAS
            // loses, this value is discarded and we retry.
            T value = head_next->data;

            if (head.compare_exchange_weak(
                    old_head,
                    head_next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                result = std::move(value);

                clearHazards(hp);

                // IMPORTANT: no delete old_head here. It may still appear in
                // another thread's hazard slot. Retire it for safe reclamation.
                retire(old_head);
                return true;
            }

            hazard(hp.second).store(nullptr, std::memory_order_seq_cst);
        }
    }
};
