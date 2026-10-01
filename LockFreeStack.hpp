#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <optional>

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

    private:
        size_t slotIndex{MAX_HAZARD_SLOTS};
        std::vector<Node*> retiredPtrs{};
        HazardSlot* mySlot{nullptr};
        static constexpr size_t WATERMARK{256};
        
        /**
         * The domainPtr is not needed in this design as we make a simplfying
         * assumption that there is only one LockFreeStack among the whole
         * program. Realistically, this is not the case and we need the domainPtr
         * to point back to the Hazard Area (the memory that all threads will
         * share to determine if it is safe to delete).
         */
        // HazardSlot* domainPtr{nullptr};

        void sweep();

        TLContext();

        void retireNode(Node* node);

        ~TLContext();

    
        friend class LockFreeStack;

    };


    TLContext& getContext();

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