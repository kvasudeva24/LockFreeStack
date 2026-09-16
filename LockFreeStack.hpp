#include <atomic>
#include <memory>
#include <optional>



template <typename T>
class LockFreeStack {

    /**
     * Because we are using a linked list we define our own Node class.
     */
    struct Node {
        T value;
        Node* next;

        Node(T val): value(std::move(val)), next(nullptr) {};

    };

private:

    std::atomic<Node*> head_ = nullptr;


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
    LockFreeStack(LockFreeStack& other) = delete;
    LockFreeStack& operator=(LockFreeStack& other) = delete;


    /**
     * Because the structure owns the memory for the nodes, it must clean up
     * the memory.
     * 
     * It is safe to assume that in good practice, the destructor is called in a
     * SINGLE THREADED context.
     */
    ~LockFreeStack(){

    }


    void push(const T& value) {

        std::unique_ptr<Node> newNode = std::make_unique<Node>(value);

        Node* oldHead = head_.load(std::memory_order_relaxed);

        while (1) {

            newNode->next = oldHead;

            bool result = head_.compare_exchange_weak(oldHead,
                                                      newNode.get(),
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed);

            if ( result ) {
                break;
            }
        }

        //
        // We have successfully updated our head of the stack
        // Release the memory associated with the ptr we created
        //
        newNode.release();
    }


    std::optional<T> pop() {
        Node* newHead;

        Node* oldHead = head_.load(std::memory_order_relaxed);

        while (1) {

            if (oldHead == nullptr) {
                return {};
            }

            newHead = oldHead->next;

            bool result = head_.compare_exchange_weak(oldHead,
                                                      newHead,
                                                      std::memory_order_acquire,
                                                      std::memory_order_relaxed);

            if ( result == true ) {
                break;
            }
        }

        return oldHead->value;
    }

};