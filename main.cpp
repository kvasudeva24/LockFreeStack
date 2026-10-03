// Test suite for LockFreeStack (hazard-pointer reclamation).
//
// Contract these tests respect (see LockFreeStack.hpp):
//   * one LockFreeStack alive at a time
//   * the main thread never calls pop() (every test body runs on workers)
//   * every thread that touched a stack is joined before the stack dies
//
// Usage: ./stack [scale] [soak_seconds]
//   scale         multiplies iteration counts (default 1)
//   soak_seconds  wall time for the randomized soak test (default 3)

#include "LockFreeStack.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_scale = 1;
int g_soakSeconds = 3;
int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "    CHECK FAILED %s:%d: %s\n", __FILE__,   \
                         __LINE__, #cond);                                   \
            ++g_failures;                                                    \
            return;                                                          \
        }                                                                    \
    } while (0)

// Runs f on a fresh worker thread and joins it, so the main thread never
// creates a TLContext.
void onWorker(const std::function<void()>& f) {
    std::thread t(f);
    t.join();
}

// Spawns n threads running f(id), releases them together, joins them all.
void runConcurrently(int n, const std::function<void(int)>& f) {
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (int i = 0; i < n; ++i) {
        ts.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            f(i);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
}

int hwThreads() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4 : static_cast<int>(n);
}

// Tracks that every value in [0, n) is popped exactly once.
struct SeenTable {
    std::unique_ptr<std::atomic<int>[]> counts;
    int n;
    explicit SeenTable(int n_) : counts(new std::atomic<int>[n_]), n(n_) {
        for (int i = 0; i < n; ++i) counts[i].store(0, std::memory_order_relaxed);
    }
    // Returns false if the value is out of range.
    bool mark(int v) {
        if (v < 0 || v >= n) return false;
        counts[v].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Returns the first value not seen exactly once, or -1.
    int firstBad() const {
        for (int i = 0; i < n; ++i)
            if (counts[i].load(std::memory_order_relaxed) != 1) return i;
        return -1;
    }
};

// ---------------------------------------------------------------- tests ---

// Empty stack behavior and LIFO order on a single thread.
void testSingleThreadBasics() {
    LockFreeStack s;
    onWorker([&] {
        CHECK(!s.pop().has_value());

        s.push(42);
        auto v = s.pop();
        CHECK(v.has_value() && *v == 42);
        CHECK(!s.pop().has_value());

        for (int i = 0; i < 100; ++i) s.push(i);
        for (int i = 99; i >= 0; --i) {
            auto r = s.pop();
            CHECK(r.has_value() && *r == i);
        }
        CHECK(!s.pop().has_value());

        // Extreme values round-trip unchanged.
        s.push(INT32_MIN);
        s.push(INT32_MAX);
        s.push(0);
        s.push(-1);
        CHECK(*s.pop() == -1);
        CHECK(*s.pop() == 0);
        CHECK(*s.pop() == INT32_MAX);
        CHECK(*s.pop() == INT32_MIN);
    });
}

// Pops far past WATERMARK on one thread so sweep() runs many times with
// only this thread's own slot in play. Leaks show up under ASan.
void testSingleThreadManySweeps() {
    LockFreeStack s;
    const int n = 20000 * g_scale;
    onWorker([&] {
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < n; ++i) s.push(i);
            for (int i = n - 1; i >= 0; --i) {
                auto r = s.pop();
                CHECK(r.has_value() && *r == i);
            }
            CHECK(!s.pop().has_value());
        }
    });
}

// Destroying a stack that still holds nodes must free them (ASan checks).
void testDestructorFreesRemainingNodes() {
    LockFreeStack s;
    onWorker([&] {
        for (int i = 0; i < 1000; ++i) s.push(i);
        for (int i = 0; i < 300; ++i) s.pop();   // leaves retired + live nodes
    });
}

// All producers push, then all consumers drain. Every value exactly once.
void testProducersThenConsumers() {
    LockFreeStack s;
    const int threads = hwThreads();
    const int perThread = 20000 * g_scale;
    const int total = threads * perThread;
    SeenTable seen(total);
    std::atomic<bool> outOfRange{false};

    runConcurrently(threads, [&](int id) {
        for (int i = 0; i < perThread; ++i) s.push(id * perThread + i);
    });
    runConcurrently(threads, [&](int) {
        while (auto r = s.pop())
            if (!seen.mark(*r)) outOfRange = true;
    });

    CHECK(!outOfRange);
    CHECK(seen.firstBad() == -1);
}

// Producers and consumers run at the same time; a final drain collects the
// rest. Every value exactly once.
void testConcurrentMixed() {
    LockFreeStack s;
    const int producers = std::max(2, hwThreads() / 2);
    const int consumers = std::max(2, hwThreads() / 2);
    const int perProducer = 30000 * g_scale;
    const int total = producers * perProducer;
    SeenTable seen(total);
    std::atomic<int> producersDone{0};
    std::atomic<bool> outOfRange{false};

    runConcurrently(producers + consumers, [&](int id) {
        if (id < producers) {
            for (int i = 0; i < perProducer; ++i) s.push(id * perProducer + i);
            producersDone.fetch_add(1);
        } else {
            while (producersDone.load() < producers) {
                if (auto r = s.pop())
                    if (!seen.mark(*r)) outOfRange = true;
            }
        }
    });
    onWorker([&] {
        while (auto r = s.pop())
            if (!seen.mark(*r)) outOfRange = true;
    });

    CHECK(!outOfRange);
    CHECK(seen.firstBad() == -1);
}

// Every thread pushes then immediately pops on a nearly empty stack.
// Threads constantly fight over the same one or two head nodes, which is
// the worst case for hazard collisions, failed CASes and use-after-free.
void testHighContentionSmallStack() {
    LockFreeStack s;
    const int threads = hwThreads() * 2;
    const int iters = 40000 * g_scale;
    std::atomic<long long> pushedSum{0}, poppedSum{0};

    runConcurrently(threads, [&](int id) {
        long long localPush = 0, localPop = 0;
        for (int i = 0; i < iters; ++i) {
            int v = id * iters + i + 1;
            s.push(v);
            localPush += v;
            if (auto r = s.pop()) localPop += *r;
        }
        pushedSum += localPush;
        poppedSum += localPop;
    });
    onWorker([&] {
        while (auto r = s.pop()) poppedSum += *r;
    });

    CHECK(pushedSum.load() == poppedSum.load());
}

// Pop-only threads hammer a stack while producers refill it in bursts, so
// sweeps on one thread race with pops that hold hazards on another.
void testPopHeavyWithBurstyRefill() {
    LockFreeStack s;
    const int poppers = std::max(3, hwThreads() - 1);
    const int bursts = 200 * g_scale;
    const int burstSize = 500;
    const int total = bursts * burstSize;
    SeenTable seen(total);
    std::atomic<bool> producing{true};
    std::atomic<bool> outOfRange{false};

    runConcurrently(poppers + 1, [&](int id) {
        if (id == 0) {
            for (int b = 0; b < bursts; ++b)
                for (int i = 0; i < burstSize; ++i) s.push(b * burstSize + i);
            producing = false;
        } else {
            while (producing.load()) {
                if (auto r = s.pop())
                    if (!seen.mark(*r)) outOfRange = true;
            }
        }
    });
    onWorker([&] {
        while (auto r = s.pop())
            if (!seen.mark(*r)) outOfRange = true;
    });

    CHECK(!outOfRange);
    CHECK(seen.firstBad() == -1);
}

// Many short-lived threads, far more in total than MAX_HAZARD_SLOTS.
// Exercises slot reuse (owner release/acquire handoff) and the zombie list
// handoff in ~TLContext. If slots were not returned, a later wave would
// throw "Exceeded MAX_HAZARD_SLOTS".
void testThreadChurnAndSlotReuse() {
    LockFreeStack s;
    const int waves = 20 * g_scale;
    const int perWave = 16;
    const int opsPerThread = 2000;
    std::atomic<long long> pushedSum{0}, poppedSum{0};
    std::atomic<int> exceptions{0};

    for (int w = 0; w < waves; ++w) {
        runConcurrently(perWave, [&](int id) {
            try {
                for (int i = 0; i < opsPerThread; ++i) {
                    int v = (w * perWave + id) * opsPerThread + i + 1;
                    s.push(v);
                    pushedSum += v;
                    if (auto r = s.pop()) poppedSum += *r;
                }
            } catch (const std::exception&) {
                exceptions++;
            }
        });
    }
    onWorker([&] {
        while (auto r = s.pop()) poppedSum += *r;
    });

    CHECK(waves * perWave > static_cast<int>(MAX_HAZARD_SLOTS));
    CHECK(exceptions.load() == 0);
    CHECK(pushedSum.load() == poppedSum.load());
}

// More simultaneous popping threads than slots: the extra threads must get
// the exception, and once everyone exits all slots must be free again.
void testSlotExhaustionAndRecovery() {
    LockFreeStack s;
    const int threads = static_cast<int>(MAX_HAZARD_SLOTS) + 8;
    std::atomic<int> claimed{0}, rejected{0};
    std::atomic<int> arrived{0};

    runConcurrently(threads, [&](int) {
        bool gotSlot = false;
        try {
            s.pop();             // first pop() constructs the TLContext
            gotSlot = true;
            claimed++;
        } catch (const std::runtime_error&) {
            rejected++;
        }
        // Hold slots until every thread has tried, so they are truly concurrent.
        arrived++;
        while (arrived.load() < threads) std::this_thread::yield();
        (void)gotSlot;
    });

    CHECK(claimed.load() == static_cast<int>(MAX_HAZARD_SLOTS));
    CHECK(rejected.load() == threads - static_cast<int>(MAX_HAZARD_SLOTS));

    // After those threads exited, every slot should be claimable again.
    std::atomic<int> claimedAgain{0};
    std::atomic<int> arrived2{0};
    const int again = static_cast<int>(MAX_HAZARD_SLOTS);
    runConcurrently(again, [&](int) {
        try {
            s.pop();
            claimedAgain++;
        } catch (const std::runtime_error&) {
        }
        arrived2++;
        while (arrived2.load() < again) std::this_thread::yield();
    });
    CHECK(claimedAgain.load() == again);
}

// Sequential stacks (never two alive at once) reuse the global hazard
// domain and static zombie list without leaking or double freeing.
void testSequentialStacks() {
    for (int k = 0; k < 5; ++k) {
        LockFreeStack s;
        std::atomic<long long> pushedSum{0}, poppedSum{0};
        runConcurrently(4, [&](int id) {
            for (int i = 0; i < 5000; ++i) {
                int v = id * 5000 + i + 1;
                s.push(v);
                pushedSum += v;
                if (i % 2 == 0)
                    if (auto r = s.pop()) poppedSum += *r;
            }
        });
        onWorker([&] {
            while (auto r = s.pop()) poppedSum += *r;
        });
        CHECK(pushedSum.load() == poppedSum.load());
    }
}


// Short-lived threads pop and exit while long-lived threads keep protecting
// the same few head nodes. An exiting thread's final sweep often finds its
// retired nodes still protected, so they must go through the zombie list
// (a missing handoff shows up as a leak under ASan).
void testExitWhileOthersHoldHazards() {
    LockFreeStack s;
    const int background = std::max(3, hwThreads());
    const int shortLived = 400 * g_scale;
    std::atomic<bool> stop{false};
    std::atomic<long long> pushedSum{0}, poppedSum{0};

    std::vector<std::thread> bg;
    for (int id = 0; id < background; ++id) {
        bg.emplace_back([&, id] {
            long long localPush = 0, localPop = 0;
            int v = id + 1;
            while (!stop.load(std::memory_order_relaxed)) {
                s.push(v);
                localPush += v;
                v += background;
                if (auto r = s.pop()) localPop += *r;
            }
            pushedSum += localPush;
            poppedSum += localPop;
        });
    }
    for (int i = 0; i < shortLived; ++i) {
        onWorker([&] {
            long long localPop = 0;
            for (int k = 0; k < 8; ++k)
                if (auto r = s.pop()) localPop += *r;
            poppedSum += localPop;
        });
    }
    stop = true;
    for (auto& t : bg) t.join();
    onWorker([&] {
        while (auto r = s.pop()) poppedSum += *r;
    });

    CHECK(pushedSum.load() == poppedSum.load());
}

// Runs a randomized push/pop mix on every thread for g_soakSeconds of wall
// time. Rare interleavings need time, not just iterations.
void testTimedSoak() {
    LockFreeStack s;
    const int threads = std::max(4, hwThreads() * 2);
    std::atomic<bool> stop{false};
    std::atomic<long long> pushedSum{0}, poppedSum{0}, ops{0};

    std::thread timer([&] {
        std::this_thread::sleep_for(std::chrono::seconds(g_soakSeconds));
        stop = true;
    });
    runConcurrently(threads, [&](int id) {
        unsigned rng = 2463534242u ^ static_cast<unsigned>(id * 7919);
        long long localPush = 0, localPop = 0, localOps = 0;
        int next = id + 1;
        while (!stop.load(std::memory_order_relaxed)) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;   // xorshift
            if (rng % 100 < 52) {          // slight push bias keeps it non-empty
                s.push(next);
                localPush += next;
                next += threads;
            } else if (auto r = s.pop()) {
                localPop += *r;
            }
            ++localOps;
        }
        pushedSum += localPush;
        poppedSum += localPop;
        ops += localOps;
    });
    timer.join();
    onWorker([&] {
        while (auto r = s.pop()) poppedSum += *r;
    });

    std::printf("    soak: %lld ops on %d threads\n", ops.load(), threads);
    CHECK(pushedSum.load() == poppedSum.load());
}

// ---------------------------------------------------------------- runner ---

struct Test {
    const char* name;
    void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_scale = std::max(1, std::atoi(argv[1]));
    if (argc > 2) g_soakSeconds = std::max(1, std::atoi(argv[2]));

    const Test tests[] = {
        {"single-thread basics", testSingleThreadBasics},
        {"single-thread many sweeps", testSingleThreadManySweeps},
        {"destructor frees remaining nodes", testDestructorFreesRemainingNodes},
        {"producers then consumers", testProducersThenConsumers},
        {"concurrent mixed push/pop", testConcurrentMixed},
        {"high contention, small stack", testHighContentionSmallStack},
        {"pop-heavy with bursty refill", testPopHeavyWithBurstyRefill},
        {"thread churn and slot reuse", testThreadChurnAndSlotReuse},
        {"slot exhaustion and recovery", testSlotExhaustionAndRecovery},
        {"exit while others hold hazards", testExitWhileOthersHoldHazards},
        {"sequential stacks", testSequentialStacks},
        {"timed soak", testTimedSoak},
    };

    std::printf("LockFreeStack tests (scale=%d, soak=%ds, hw threads=%d)\n", g_scale, g_soakSeconds, hwThreads());
    int failedTests = 0;
    for (const Test& t : tests) {
        const int before = g_failures;
        auto start = std::chrono::steady_clock::now();
        t.fn();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::printf("  [%s] %-36s %6lld ms\n", ok ? "PASS" : "FAIL", t.name,
                    static_cast<long long>(ms));
    }

    std::printf("%s: %d/%zu tests passed\n", failedTests ? "FAILED" : "OK",
                static_cast<int>(sizeof(tests) / sizeof(tests[0])) - failedTests,
                sizeof(tests) / sizeof(tests[0]));
    return failedTests ? 1 : 0;
}