#include "LockFreeStack.hpp"
#include <stdexcept>

LockFreeStack::~LockFreeStack() {
    Node* iter = head_.load(std::memory_order_relaxed);
    Node* next = nullptr;

    while (iter != nullptr) {
        next = iter->next;

        delete iter;
        iter = next;
    }
}

void LockFreeStack::push(const int& value) {
    std::unique_ptr<Node> newNode = std::make_unique<Node>(value);

    Node* oldHead = head_.load(std::memory_order_relaxed);

    while (1) {
        newNode->next = oldHead;

        bool result = head_.compare_exchange_weak(oldHead,
                                                  newNode.get(),
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed);

        if (result) {
            break;
        }
    }

    //
    // We have successfully updated our head of the stack
    // Release the memory associated with the ptr we created
    //
    newNode.release();
}

// std::optional<int> LockFreeStack::pop() {
//     Node* newHead;

//     Node* oldHead = head_.load(std::memory_order_relaxed);

//     while (1) {
//         if (oldHead == nullptr) {
//             return {};
//         }

//         newHead = oldHead->next;

//         bool result = head_.compare_exchange_weak(oldHead,
//                                                   newHead,
//                                                   std::memory_order_acquire,
//                                                   std::memory_order_relaxed);

//         if (result == true) {
//             break;
//         }
//     }

//     return oldHead->value;
// }

LockFreeStack::TLContext::TLContext() {
    size_t slot{};

    for ( ; slot < MAX_HAZARD_SLOTS; slot++ ) {
        if ( StackHazardDomain[slot].owner.load(std::memory_order_relaxed) == std::thread::id()) {

            auto expected{std::thread::id()};

            bool result = StackHazardDomain[slot].owner.compare_exchange_strong(expected,
                                                                                std::this_thread::get_id(),
                                                                                std::memory_order_acquire,
                                                                                std::memory_order_relaxed);
            
            if ( result ) {
                slotIndex = slot;
                mySlot = &StackHazardDomain[slot];
                return;
            }

        }
    }

    // if slot is MAX_HAZARD_SLOTS we need to fail gracefully
    throw std::runtime_error("Exceeded MAX_HAZARD_SLOTS");
}

void LockFreeStack::TLContext::retireNode(Node* node) {
    retiredPtrs.push_back(node);
    if ( retiredPtrs.size() > WATERMARK ) {
        sweep();
    }
}

void LockFreeStack::TLContext::sweep() {
    
}