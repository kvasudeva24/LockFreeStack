#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <optional>
#include <mutex>

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


    class TLContext {

        size_t slotIndex{MAX_HAZARD_SLOTS};
        std::vector<Node*> retiredPtrs{};
        HazardSlot* mySlot{nullptr};
        static constexpr size_t WATERMARK{256};

        TLContext();
        void sweep();
        void retireNode(Node* node);
        ~TLContext();

        friend class LockFreeStack;
    };


    static TLContext& getContext();

    std::atomic<Node*> head_{nullptr};

    /**
     * When a thread is exiting somre retired pointers may still be in other 
     * threads hazard slots. In that case we cannot retire them and because we
     * make the simplifying assumption that the main() thread will never act on
     * the LockFreeStack and all threads that act on the stacks are reclaimed
     * before destruction, we have zombiePtrs that will free unlinked nodes
     * before exiting.
     */
    inline static std::vector<Node*> zombiePtrs{};
    inline static std::mutex zombieMtx{}; //This protects the zombiePtrs

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