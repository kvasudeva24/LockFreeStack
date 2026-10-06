# Lock-Free Stack with Hazard Pointers

My notes on how a lock-free stack works, the problems that come up with concurrent pops, and how hazard pointers and memory ordering solve them.

## Why lock-free?

Imagine a stack that multiple threads want to share. With a lock, if the thread holding it gets preempted, every other thread waits. Under contention we also pay for context switches, putting threads to sleep and waking them back up when the lock is dropped.

A lock-free stack avoids this. If one thread's CAS fails, it's because another thread's CAS succeeded, so the system as a whole always makes progress.

## Atomics and CAS

Lock-free programming is built on atomics and CAS (compare-and-swap). A CAS checks the value in memory against what we expect it to be, and if it matches, swaps in the new value. The compare and the swap happen as one atomic operation. If the CAS fails, we retry until we succeed.

The downside is that all threads compete for one cache line (the head). Under high contention, cache coherence traffic is very high, and many threads sit in a busy loop burning CPU while only some of them make progress.

## The problem: concurrent pops

Pop can be called concurrently, so two threads may pop at once. There are two cases.

**Case 1: use-after-free**

1. Thread A reads the current head (`x`) and gets preempted.
2. Thread B pops `x` and deletes it.
3. Thread A resumes and dereferences `x->next` to find the new head. This is a use-after-free.

**Case 2: ABA**

1. Thread A reads the current head (`x`), reads `x->next` (`y`), then gets preempted.
2. Thread B pops `x` and deletes it, then pops `y` and deletes it.
3. Thread B pushes two new nodes, `x'` and `y'`, where `y'` reuses the memory address of the original `x`. The stack is now `y' -> x' -> z`.
4. Thread A resumes. Its CAS compares head to `x`, and the comparison succeeds because `y'` has the same address. Head gets set to `y`, which is a deleted node. This is undefined behavior.

Push doesn't have these problems. It never dereferences head, it only links the new node to it.

## Hazard pointers

Hazard pointers are global slots (one per thread) where a thread publishes its intent to dereference a node.

Pop works like this:

1. Load head (`oldHead`).
2. Publish `oldHead` to my hazard pointer slot.
3. Reload head (`curHead`) to make sure what I loaded is still valid. If `oldHead` and `curHead` differ, retry from step 2 with the new head.
4. Now it's safe to dereference `oldHead` to get the next node, then try the CAS.
5. If the CAS fails, go through the whole loop again.
6. Once the CAS succeeds, clear my hazard pointer. Otherwise my own hazard would point at the node I popped and it could never be freed.
7. Retire the node.

## Retiring and sweeping

When we retire a node, we add it to a thread-local retired list. If the list's size goes over a watermark, we sweep it to free memory.

During a sweep, if any hazard pointer holds the same address as a retired node, we **cannot** free it. It has to wait until no hazard pointer holds that address. This makes sure the two cases above never happen.

The cost of sweeping is amortized O(1) per retire, as long as the watermark scales with the total number of hazard pointers H (for example R = 2H). At most H nodes can be protected, so each sweep frees at least R - H nodes.

## Memory ordering

The biggest thing to worry about is the compiler and the CPU reordering instructions. Compilers and different CPUs can aggressively reorder instructions, so we have to say what ordering we need.

- **Release:** earlier reads and writes can't be reordered after the release. Later instructions are allowed to move before it.
- **Acquire:** later reads and writes can't be reordered before the acquire. Earlier instructions are allowed to move after it.

A release/acquire pair only synchronizes when the acquire load reads the value written by the release store.

### Push and pop: release/acquire pair

When we push, all data about the node (its value, its `next`) must be globally visible before the node is published as head. So the CAS in push is a **release**. When we pop, the load of head is an **acquire**, so reading the node's `next` and value sees what the pusher wrote.

### Racing pops: the store buffer litmus test

Two racing pops form the classic store buffer pattern:

| Thread A (protecting `x`) | Thread B (popped `x`) |
|---|---|
| store `hazard = x` | CAS `head: x -> y` |
| load `head` (validate) | load hazard slots (sweep) |

Release/acquire does not stop a later load from being reordered before an earlier store (StoreLoad reordering). Without a stronger ordering, Thread A can read `head == x` before its hazard write is globally visible, while Thread B's sweep reads the hazard slots before its CAS is visible. Thread B sees no hazard and frees `x`, while Thread A goes on to dereference it. Use-after-free.

Both sides need a full fence (`seq_cst`), so A's reload of head happens after its hazard write is globally visible, and B's read of the hazard slots happens after its CAS.

### Overwriting a hazard pointer needs release

In pop:

```
oldHead = A
hazardPtr = A
fence
curHead = A        // validation succeeds
next = A->next     // dereference A
CAS fails

oldHead = B
hazardPtr = B      // overwrites the hazard on A
```

If the store `hazardPtr = B` is reordered before the read of `A->next` completes, another thread could sweep, see that no one is protecting A, and free it while we are still reading it. Use-after-free.

So every overwrite of a hazard pointer slot, including setting it to `nullptr`, needs **release** semantics. Per the C++ standard, this pairs with an **acquire** when the sweep reads the hazard slots. (A `seq_cst` store is already a release, so publishing with `seq_cst` covers the overwrite case. The `nullptr` clear needs an explicit release.)