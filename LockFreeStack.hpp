#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

constexpr size_t MAX_HAZARD_SLOTS{128};

struct HazardSlot {
    std::atomic<std::thread::id> owner{std::thread::id()};
    std::atomic<void*> hazard_ptr{nullptr};
};

inline HazardSlot StackHazardDomain[MAX_HAZARD_SLOTS];



class LockFreeStack {
private:
    /**
     * Because we are using a linked list we define our own Node class.
     */
    struct Node {
        int value;
        Node* next;

        Node(int val) : value(val), next(nullptr) {}
        ~Node() = default;
    };

    std::atomic<Node*> head_{nullptr};

public:
    LockFreeStack() = default;

    /**
     * Copying a structure that is denoted to be lock free has lots of
     * correctness problems for obvious reasons.
     * 
     * Copying a large lock free stack is non-atomic and thus other threads
     * can change the stack from the moment you issued the copy to the moment
     * it completes.
     */
    LockFreeStack(const LockFreeStack& other) = delete;
    LockFreeStack& operator=(const LockFreeStack& other) = delete;

    /**
     * Because the structure owns the memory for the nodes, it must clean up
     * the memory.
     * 
     * It is safe to assume that in good practice, the destructor is called in a
     * SINGLE THREADED context.
     */
    ~LockFreeStack();

    void push(const int& value);
    std::optional<int> pop();
};