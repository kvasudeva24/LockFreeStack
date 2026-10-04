# Lock-Free Stack with Hazard Pointers

> For my analysis and understanding of the project, refer to the SOLUTIONS.md in the `Solutions` folder.

A stack of `int`s that many threads can `push` and `pop` concurrently **without a mutex**.

```
LockFreeStack.hpp   <- shared base everyone receives (API + layout)
LockFreeStack.cpp   <- implementation
```

---

## 1. Requirements

- **C++17 or newer** (`inline` variables, `std::optional`). Link with `-pthread`.
- Comfortable with pointers, linked lists, and `std::thread`.
- **C++ memory ordering semantics.** If you don't know `relaxed`, `acquire`, `release`, and `seq_cst` yet, **please learn them first and come back.** Sections 4 onward depend on them.

---

## 2. Why Lock-Free?

The simple way to make a stack thread-safe is one `std::mutex` around every operation. Under load, that's a **contention** problem:

- Every thread wants the same lock, but only one can hold it.
- Threads that lose are put to sleep. Sleeping costs a context switch, and waking up costs another.
- With many threads, the program spends more time switching and waking than doing pushes and pops.
- If the lock holder gets preempted, every other thread waits on it.

A lock-free stack never makes a thread sleep while waiting on another. A thread that loses a race simply retries.

---

## 3. Lock-Free and CAS

Without a lock, we protect the stack with **atomics**. An atomic operation is indivisible: no thread ever sees it half done. The top of the stack is

```cpp
std::atomic<Node*> head_{nullptr};
```

so `head_` is always in exactly one state.

The key atomic operation is **compare-and-swap (CAS)**. It does this as one step:

```cpp
// head_.compare_exchange_weak(expected, desired)
if (head_ == expected) { head_ = desired; return true;  }
else                   { expected = head_; return false; }
```

Every lock-free operation follows the same loop:

1. Read `head_`.
2. Build the new state privately.
3. CAS `head_` from the old value to the new one.
4. If the CAS failed, someone else changed `head_` first. `expected` now holds the new value, so go back to step 2.

> **Side note: weak vs strong.**
> `compare_exchange_weak` may fail **spuriously** (return false even though the values matched). It's cheaper on some CPUs and is the right choice inside a retry loop.
> `compare_exchange_strong` never fails spuriously. Use it for a one-shot attempt, like claiming a hazard slot.

---

## 4. Release/Acquire Pairing

CAS makes the swap of `head_` atomic, but the CPU and compiler can still **reorder** the ordinary reads and writes around it. Two things must be guaranteed:

- **Writer side:** a node's fields (`value`, `next`) must be written **before** the node is published to `head_`. Otherwise another thread could see the pointer and read uninitialized fields. The publishing CAS uses **`release`**: no earlier write can move after it.
- **Reader side:** reads of a node's fields must happen **after** loading its pointer from `head_`. The load uses **`acquire`**: no later read can move before it.

When an acquire load reads a value written by a release store, the reader sees everything the writer did before the store:

```
Thread A (push):
  node->value = 42;
  node->next  = oldHead;
  head_.CAS(..., release);   // publish

Thread B (pop):
  n = head_.load(acquire);   // sees A's node
  n->value;                  // guaranteed 42
```

---

## 5. Push

Push is easy. It only writes to its **own** new node and then swings `head_`. It never dereferences a node another thread could free, so it needs no memory reclamation.

```cpp
Node* oldHead = head_.load(std::memory_order_relaxed);
do {
    newNode->next = oldHead;                       // build privately
} while (!head_.compare_exchange_weak(oldHead, newNode,
                                      std::memory_order_release,   // publish the node
                                      std::memory_order_relaxed)); // failed: retry
```

---

## 6. The Pop Problem

A naive pop CASes `head_` from `oldHead` to `oldHead->next`, then calls `delete oldHead`. Running pops concurrently on `A → B → C` can fail in two ways, depending on when T1 gets preempted.

**Use-after-free:** T1 is preempted *before* reading `A->next`.

```
T1: oldHead = A
T1: (preempted)
T2: pop A, delete A
T1: reads A->next   -> A is freed: use-after-free
```

**ABA:** T1 is preempted *after* reading `A->next`.

```
T1: oldHead = A, newHead = A->next (= B)
T1: (preempted)
T2: pop A, delete A
T2: pop B, delete B
T2: push new node; allocator reuses A's address
T1: CAS(head_: A -> B) succeeds
    -> head_ = freed B, node C is lost
```

CAS only compares addresses. `head_` holds "A" again, so T1's CAS succeeds even though it's a different node.

The root cause is that **a node was deleted while another thread still held a pointer to it.**

---

## 7. The Need for Hazard Pointers

We can't delete a popped node immediately, but we also can't leak it forever. We need a **safe memory reclamation** scheme.

**Hazard pointers** let a thread **mark a pointer as "don't delete"**:

- Before dereferencing a shared node, a thread publishes that pointer in a slot every thread can read.
- A node that's been popped is **retired**, not deleted.
- A retired node is only deleted once **no thread has it marked**.

Because a marked node can't be freed, its address can't be reused while someone holds it. That fixes both the use-after-free and ABA.

---

## 8. Hazard Pointers in This Project

### The hazard domain
All marks live in one global array:

```cpp
struct HazardSlot {
    std::atomic<std::thread::id> owner;      // empty id = slot is free
    std::atomic<void*>           hazard_ptr; // the pointer marked "don't delete"
};
inline HazardSlot StackHazardDomain[MAX_HAZARD_SLOTS];  // 128
```

### Simplifying assumptions
- **One `LockFreeStack` per program.** The hazard domain and the zombie list (section 10) are global and shared.
- **One hazard pointer per thread.** Pop only touches one node at a time.
- **At most 128 threads** call `pop` concurrently.

### Marking in `pop`: protect → fence → validate
1. Store `oldHead` into our `hazard_ptr`.
2. Issue a `seq_cst` fence, so our mark is visible before the next load. (Release/acquire can't prevent a store→load reorder; only `seq_cst` can.)
3. Load `head_` again. If it still equals `oldHead`, the node was on the stack after we marked it, so it's protected. If not, retry.

After that, the CAS, read the value, clear our mark, and retire the node.

### Reclaiming retired nodes
Each thread keeps its own list of retired nodes. When the list passes `WATERMARK` (256), the thread calls `sweep()`:

1. Issue a `seq_cst` fence (pairs with the fence in `pop`).
2. Snapshot all 128 `hazard_ptr`s.
3. Delete every retired node that isn't in the snapshot. Keep the rest for the next sweep.

Sweeping in batches spreads the cost of scanning all slots over many pops.

---

## 9. Per-Thread Storage: `TLContext`

Each popping thread needs its **own** hazard slot and its **own** retired list. Sharing those would bring contention back, so they live in `thread_local` storage:

```cpp
static TLContext& getContext() {
    thread_local TLContext ctx;   // built on first pop, destroyed when the thread exits
    return ctx;
}
```

### Members

| Member | Purpose |
|---|---|
| `slotIndex` | Index of this thread's slot in the domain (`MAX_HAZARD_SLOTS` means none). |
| `mySlot` | Pointer to that `HazardSlot`. |
| `retiredPtrs` | Nodes this thread popped but hasn't deleted yet. Private, so no lock. |
| `WATERMARK` | List size (256) that triggers a sweep. |

### Functions

| Function | Purpose |
|---|---|
| `TLContext()` | Finds a free slot and claims it with `compare_exchange_strong` on `owner`. Throws if all 128 are taken. |
| `retireNode(node)` | Adds a node to `retiredPtrs`; sweeps past the watermark. |
| `sweep()` | Deletes retired nodes no thread has marked. |
| `~TLContext()` | Cleanup when the thread exits (section 10). |

Only `pop` uses the context. `push` never needs it.

---

## 10. Exit, Cleanup, and the Rule for `main()`

### Thread exit
When a thread exits, `~TLContext()` must retire everything it holds:

1. Clear its own hazard pointer.
2. `sweep()` to delete what it can.
3. Nodes still marked by **other** threads go to the static **`zombiePtrs`** list, guarded by `zombieMtx`. A lock is fine here because this runs once per thread, not on every pop.
4. Release its slot (`owner` = empty id).

### Stack destruction
`~LockFreeStack()` deletes the nodes still on the stack **and** every zombie. This is only safe when no other thread can touch the stack, so the destructor must run single-threaded, after every worker has exited.

### The rule for `main()`
> **`main()` initializes the `LockFreeStack` but never calls any operations on it.** Worker threads do all pushes and pops, and they must be **joined** before the stack is destroyed.

Why: main's `thread_local TLContext` is destroyed **after** `main()` returns, which is after `~LockFreeStack()` has already deleted the zombies. Anything main retired would leak.

```cpp
int main() {
    LockFreeStack stack;                       // construct only
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([&stack, t] { stack.push(t); stack.pop(); });
    for (auto& w : workers) w.join();          // every TLContext cleaned up here
}                                              // ~LockFreeStack frees the rest
```