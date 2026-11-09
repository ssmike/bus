// Regression tests for known logic bugs. Each test is registered as its own
// CTest entry (see CMakeLists.txt). On the current code most of them are
// EXPECTED TO FAIL; they should pass once the corresponding bug is fixed.
//
// Tests that can crash / hang / get killed by a signal run their scenario in
// a fork()ed child (run_in_child) with a timeout.
//
// Ports: test for bug N uses 15000 + 20*N .. 15000 + 20*N + 19.

#include "test_helpers.h"

#include "action_map.h"
#include "buffer.h"
#include "connect_pool.h"
#include "endpoint_manager.h"

#include <dirent.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>

#include <random>
#include <set>

using namespace bus;
using namespace bus_test;
using namespace std::chrono_literals;

namespace {

constexpr int port_for(int bug, int idx = 0) {
    return 15000 + 20 * bug + idx;
}

bool overlaps(const SharedView& a, const SharedView& b) {
    return a.data() < b.data() + b.size() && b.data() < a.data() + a.size();
}

// loop thread helper for raw TcpBus usage inside a child
struct LoopThread {
    TcpBus& bus;
    std::thread t;
    explicit LoopThread(TcpBus& b) : bus(b), t([this] { bus.loop(); }) {}
    ~LoopThread() {
        bus.to_break();
        t.join();
    }
};

}  // namespace

// ===========================================================================
// Bug 1: BufferPool hands out memory overlapping live slices.

TEST(Bug01, BufferPoolOverlapSingleThread) {
    BufferPool pool(100);
    { SharedView a(pool, 60); }  // head buffer fully released -> pushed to free_ while still head_
    SharedView b(pool, 30);      // carved from head buffer at [60, 90)
    SharedView c(pool, 70);      // head is full -> pops the same buffer from free_, offset reset to 0
    memset(b.data(), 'b', b.size());
    memset(c.data(), 'c', c.size());
    EXPECT_FALSE(overlaps(b, c)) << "two live SharedViews share memory";
    EXPECT_EQ(std::string(b.data(), b.size()), std::string(b.size(), 'b')) << "live view was overwritten";
}

TEST(Bug01, BufferPoolPatternIntegrityStress) {
    BufferPool pool(4096);
    std::atomic<size_t> corruptions = 0;
    std::atomic<size_t> alloc_failures = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::deque<std::pair<SharedView, uint64_t>> live;
            auto check = [&](const SharedView& v, uint64_t tag) {
                for (size_t i = 0; i < v.size(); ++i) {
                    if (static_cast<unsigned char>(v.data()[i]) != ((tag + i) & 0xff)) {
                        corruptions.fetch_add(1);
                        return;
                    }
                }
            };
            for (uint64_t it = 0; it < 400000; ++it) {
                size_t size = 1 + rng() % 2000;
                SharedView v;
                try {
                    v = SharedView(pool, size);
                } catch (const std::exception&) {
                    // take() of size <= buffer size must never fail; it does when a
                    // buffer is recycled while other threads still carve from it
                    alloc_failures.fetch_add(1);
                    continue;
                }
                uint64_t tag = (uint64_t(t) << 40) ^ (it * 2654435761u);
                for (size_t i = 0; i < size; ++i) v.data()[i] = static_cast<char>((tag + i) & 0xff);
                live.emplace_back(std::move(v), tag);
                if (live.size() > 1) {
                    check(live.front().first, live.front().second);
                    live.pop_front();
                }
            }
            for (auto& [v, tag] : live) check(v, tag);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(alloc_failures.load(), 0u) << "BufferPool::take threw 'bad buffer alloc' for a fitting size";
    EXPECT_EQ(corruptions.load(), 0u) << "views were overwritten by other allocations";
}

// SharedView::operator= between views of the same pool used to skip ref/unref
// even when they pointed at different buffers: the old buffer leaked a ref and
// the new one got an extra put, dropping the head to zero refs. The head was
// then pushed to free_ with term zeroed, so every following take() spun forever.
TEST(Bug01, SharedViewAssignAcrossBuffers) {
    auto res = run_in_child([] {
        BufferPool pool(100);
        char* x_base;
        {
            SharedView a(pool, 60);  // buffer X at [0, 60)
            x_base = a.data();
            SharedView b(pool, 60);  // doesn't fit -> buffer Y becomes head
            a = b;                   // must release X and add a ref to Y
            CHILD_CHECK(a.data() == b.data(), "assignment didn't copy the view");
        }                            // X is free, Y keeps only its head ref
        SharedView c(pool, 30);      // from head Y at [60, 90)
        SharedView d(pool, 70);      // doesn't fit in Y -> recycles X
        memset(c.data(), 'c', c.size());
        memset(d.data(), 'd', d.size());
        CHILD_CHECK(!overlaps(c, d), "two live SharedViews share memory");
        CHILD_CHECK(std::string(c.data(), c.size()) == std::string(c.size(), 'c'), "live view was overwritten");
        CHILD_CHECK(d.data() == x_base, "buffer released by operator= was not recycled (leaked ref)");
    }, 5s);
    EXPECT_CHILD_OK(res);
}

// A try_take() that loses the race against a head swap bumps the refcount of a
// buffer that may already sit in free_, then unrefs it. When unref decided to
// free on fetch_sub() == 1 and only afterwards zeroed term, a concurrent take()
// could pop and re-head the buffer in between: the stale unref then zeroed the
// new head's term and pushed it to free_ again, and every take() spun forever.
// Sizes 60/30 on a 100-byte pool force a head swap on nearly every take().
TEST(Bug01, StaleUnrefDuringBufferReuse) {
    auto res = run_in_child([] {
        BufferPool pool(100);
        std::atomic<size_t> corruptions = 0;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                for (size_t i = 0; i < 1000000; ++i) {
                    size_t size = (i & 1) ? 60 : 30;
                    SharedView v(pool, size);
                    char tag = static_cast<char>(t * 31 + i);
                    memset(v.data(), tag, size);
                    for (size_t k = 0; k < size; ++k) {
                        if (v.data()[k] != tag) {
                            corruptions.fetch_add(1);
                            break;
                        }
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        CHILD_CHECK(corruptions.load() == 0, "%zu views were overwritten by other allocations", corruptions.load());
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// Minor: SharedView::operator= on a non-empty view leaks a reference, so the
// buffer is never recycled and every iteration allocates a new one.
TEST(BugMinor, SharedViewAssignLeaksRef) {
    BufferPool pool(100);
    std::set<char*> buffer_bases;
    for (int i = 0; i < 100; ++i) {
        SharedView a(pool, 60);  // starts a fresh/recycled buffer -> offset 0 == buffer base
        buffer_bases.insert(a.data());
        SharedView b(pool, 30);
        a = b;
    }
    EXPECT_LE(buffer_bases.size(), 4u) << "buffers are never recycled after SharedView::operator=";
}

// ===========================================================================
// Bug 2: see testBugsInterpose.cpp (needs libc interposition to be deterministic).

// ===========================================================================
// Bug 3: request is flushed before its promise is registered -> fast response dropped.
// The sending thread runs with SCHED_IDLE on a single CPU, so the server and the
// client loop thread preempt it right after the write.

TEST(Bug03, FastResponseDropped) {
    auto res = run_in_child([] {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sched_getcpu(), &set);
        sched_setaffinity(0, sizeof(set), &set);

        EndpointManager manager;
        EchoService server(manager, port_for(3), 1);
        server.start();
        ProtoBus client({.tcp_opts = TcpBus::Options{.port = port_for(3, 1), .fixed_pool_size = 1},
                         .batch_opts = {.max_batch = 1}},
                        manager);
        client.start();
        int endpoint = manager.register_endpoint("::1", port_for(3));

        Operation op;
        op.set_key("key");
        // warmup with normal priority (connections established)
        wait_future(client.send<Operation, Operation>(op, endpoint, 1, 2s), 3s);

        sched_param param{};
        CHILD_CHECK(pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) == 0, "SCHED_IDLE");

        std::atomic<size_t> ok = 0, failed = 0;
        constexpr size_t kRequests = 200;
        for (size_t i = 0; i < kRequests; ++i) {
            client.send<Operation, Operation>(op, endpoint, 1, 1s).subscribe([&](ErrorT<Operation>& r) {
                (r ? ok : failed).fetch_add(1);
            });
        }
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (ok + failed < kRequests && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        CHILD_CHECK(ok.load() == kRequests, "%zu ok, %zu failed (timed out) of %zu requests", ok.load(),
                    failed.load(), kRequests);
    }, 30s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 4: seq_id truncated to uint32 in handler dispatch.

TEST(Bug04, SeqIdTruncated) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(4));
        server.start();
        const uint64_t seq = (1ull << 32) + 5;
        auto resp = raw_roundtrip(port_for(4), port_for(4, 1), seq);
        CHILD_CHECK(resp.has_value(), "no response");
        CHILD_CHECK(resp->seq_id() == seq, "response seq_id %llu != request seq_id %llu",
                    (unsigned long long)resp->seq_id(), (unsigned long long)seq);
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 5: a peer can crash the process.

TEST(Bug05, OversizedHeaderFromPeer) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(5));
        server.start();
        {
            int fd = raw_connect6(port_for(5));
            char header[internal::header_len];
            internal::write_header(1ull << 40, header);
            write_all(fd, header, sizeof(header));
            std::this_thread::sleep_for(300ms);
            ::close(fd);
        }
        auto resp = raw_roundtrip(port_for(5), port_for(5, 1), 7);
        CHILD_CHECK(resp && resp->seq_id() == 7, "server stopped serving after bogus peer");
    }, 15s);
    EXPECT_CHILD_OK(res);
}

TEST(Bug05, InvalidMethodFromPeer) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(5, 2));
        server.start();
        {
            int fd = raw_connect6(port_for(5, 2));
            write_frame(fd, make_greeter(port_for(5, 3)));
            write_frame(fd, make_request_batch(1, 999, "bogus"));
            std::this_thread::sleep_for(300ms);
            ::close(fd);
        }
        auto resp = raw_roundtrip(port_for(5, 2), port_for(5, 4), 7);
        CHILD_CHECK(resp && resp->seq_id() == 7, "server stopped serving after bogus peer");
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// Sender side: payload > max_message_size must be rejected locally (error
// result or exception), not shipped to the peer where it kills the receiver.
TEST(Bug05, SenderOversizedPayloadNotRejected) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(5, 5));
        server.start();
        ProtoBus client({.tcp_opts = TcpBus::Options{.port = port_for(5, 6), .fixed_pool_size = 1},
                         .batch_opts = {.max_batch = 1}},
                        manager);
        client.start();
        int endpoint = manager.register_endpoint("::1", port_for(5, 5));

        Operation op;
        op.set_key("key");
        op.set_value(std::string(4098 + 500, 'v'));  // > max_message_size, < pool buffer size
        bool rejected = false;
        try {
            auto r = wait_future(client.send<Operation, Operation>(op, endpoint, 1, 5s), 1s);
            rejected = r.has_value() && !*r;
        } catch (const std::exception&) {
            rejected = true;
        }
        CHILD_CHECK(rejected, "oversized request was not rejected by the sender");
        auto resp = raw_roundtrip(port_for(5, 5), port_for(5, 7), 7);
        CHILD_CHECK(resp && resp->seq_id() == 7, "server stopped serving");
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 6: no SIGPIPE handling; writing to a reset peer kills the process.

TEST(Bug06, SigpipeOnResetPeer) {
    auto res = run_in_child([] {
        std::atomic<bool> stop = false;
        int lfd = raw_listen(port_for(6), /*nonblock*/ false);
        std::thread peer([&] {
            while (!stop) {
                int fd = accept_timeout(lfd, 50);
                if (fd < 0) continue;
                pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 50) > 0) {
                    char buf[256];
                    (void)!::read(fd, buf, sizeof(buf));
                }
                ::close(fd);  // unread data pending -> RST
            }
        });

        EndpointManager manager;
        BufferPool pool(1 << 16);
        TcpBus sender(TcpBus::Options{.port = port_for(6, 1), .fixed_pool_size = 2}, pool, manager);
        sender.start([](auto, SharedView) {});
        LoopThread loop(sender);
        int endpoint = manager.register_endpoint("::1", port_for(6));
        std::string payload(2000, 'x');

        std::vector<std::thread> threads;
        auto deadline = std::chrono::steady_clock::now() + 2s;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (std::chrono::steady_clock::now() < deadline) {
                    try {
                        sender.send(endpoint, make_view(pool, payload));
                    } catch (const std::exception&) {
                    }
                    sender.clear_queue(endpoint);
                }
            });
        }
        for (auto& t : threads) t.join();
        stop = true;
        peer.join();
    }, 20s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 7: ConnectPool::close_old_conns self-deadlocks; ignores cnt.

TEST(Bug07, CloseOldConnsDeadlock) {
    auto res = run_in_child([] {
        ConnectPool pool;
        for (int i = 0; i < 3; ++i) {
            pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), pool.make_id(), i);
        }
        pool.close_old_conns(1);
    }, 3s);
    EXPECT_CHILD_OK(res);
}

TEST(Bug07, CloseOldConnsHonoursCount) {
    auto res = run_in_child([] {
        ConnectPool pool;
        for (int i = 0; i < 3; ++i) {
            pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), pool.make_id(), i);
        }
        pool.close_old_conns(1);
        size_t left = 0;
        for (int i = 0; i < 3; ++i) left += pool.count_connections(i);
        CHILD_CHECK(left == 2, "close_old_conns(1) left %zu of 3 connections", left);
    }, 3s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 8: ~ProtoBus never returns.

TEST(Bug08, ProtoBusDestructorHangs) {
    auto res = run_in_child([] {
        // Single CPU + SCHED_IDLE destroying thread: the loop thread woken by
        // to_break() runs before ~PeriodicExecutor sets shot_down_, so the period-0
        // PeriodicExecutor re-enters bus_.loop() and the destructor waits forever.
        // (Without this the race is lost most of the time and the dtor returns.)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sched_getcpu(), &set);
        sched_setaffinity(0, sizeof(set), &set);

        EndpointManager manager;
        {
            ProtoBus bus({.tcp_opts = TcpBus::Options{.port = port_for(8)}}, manager);
            bus.start();
            std::this_thread::sleep_for(100ms);
            sched_param param{};
            CHILD_CHECK(pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) == 0, "SCHED_IDLE");
        }
    }, 5s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 9: flusher_ PeriodicExecutor and timed_flush_batch both reschedule, so the
// number of timed flush chains grows by one every max_delay. Not observable via
// the public API directly; measured as CPU time of an otherwise idle ProtoBus
// with max_delay = 1ms (correct code: ~1000 cheap wakeups/s; buggy: O(t/max_delay)
// flushes per period, quickly saturating the loop thread).

TEST(Bug09, FlushChainsGrow) {
    auto res = run_in_child([] {
        EndpointManager manager;
        ProtoBus bus({.tcp_opts = TcpBus::Options{.port = port_for(9)},
                      .batch_opts = {.max_batch = 1000, .max_delay = 1ms}},
                     manager);
        bus.start();
        std::this_thread::sleep_for(500ms);
        double before = process_cpu_seconds();
        std::this_thread::sleep_for(2s);
        double used = process_cpu_seconds() - before;
        CHILD_CHECK(used < 0.4, "idle ProtoBus burnt %.2fs CPU in 2s (flush chains accumulate)", used);
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 10: timed-out requests are never erased from sent_requests_.
// The map is private; the leak is observed as heap growth (mallinfo2) across a
// second round of N timed-out requests (first round warms up pools/maps).

TEST(Bug10, TimedOutRequestsLeak) {
    auto res = run_in_child([] {
        EndpointManager manager;
        BufferPool sink_pool(1 << 16);
        TcpBus sink(TcpBus::Options{.port = port_for(10)}, sink_pool, manager);
        sink.start([](auto, SharedView) {});  // swallows requests, never answers
        LoopThread sl(sink);

        ProtoBus client({.tcp_opts = TcpBus::Options{.port = port_for(10, 1), .fixed_pool_size = 1},
                         .batch_opts = {.max_batch = 1}},
                        manager);
        client.start();
        int endpoint = manager.register_endpoint("::1", port_for(10));

        constexpr size_t N = 20000;
        Operation op;
        op.set_key("key");
        auto round = [&] {
            std::atomic<size_t> done = 0;
            for (size_t i = 0; i < N; ++i) {
                client.send<Operation, Operation>(op, endpoint, 1, 1ms).subscribe([&](auto&) { done.fetch_add(1); });
            }
            auto deadline = std::chrono::steady_clock::now() + 10s;
            while (done.load() < N && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
            CHILD_CHECK(done.load() == N, "only %zu of %zu requests completed", done.load(), N);
            std::this_thread::sleep_for(200ms);
        };
        round();
        size_t before = mallinfo2().uordblks;
        round();
        size_t after = mallinfo2().uordblks;
        long growth = long(after) - long(before);
        CHILD_CHECK(growth < long(N * 64), "heap grew by %ld bytes (%.0f B/request) after %zu timed-out requests",
                    growth, double(growth) / N, N);
    }, 40s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 12: the to_spin path runs the action map head without checking its deadline.

TEST(Bug12, SpinPathRunsActionEarly) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        EndpointManager manager;
        BufferPool pool(4096);
        TcpBus bus(TcpBus::Options{.port = port_for(12, attempt)}, pool, manager);
        bus.start([](auto, SharedView) {});

        std::atomic<bool> fired = false;
        std::atomic<int64_t> fired_after_ms = -1;
        auto start = std::chrono::steady_clock::now();
        bus.schedule([] {}, 0ms);
        bus.schedule([&] {
            fired_after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            fired = true;
        }, 500ms);
        {
            LoopThread loop(bus);
            auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!fired && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(fired.load());
        EXPECT_GE(fired_after_ms.load(), 490) << "action scheduled for +500ms ran after " << fired_after_ms.load() << "ms";
    }
}

// ===========================================================================
// Bug 13: take_available never marks the connection unavailable.

TEST(Bug13, TakeAvailableReturnsSameConnTwice) {
    ConnectPool pool;
    uint64_t id1 = pool.make_id(), id2 = pool.make_id();
    pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), id1, 0);
    pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), id2, 0);
    pool.set_available(id1);
    pool.set_available(id2);
    auto first = pool.take_available(0);
    auto second = pool.take_available(0);
    auto third = pool.take_available(0);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first->id, second->id) << "take_available handed out the same connection twice";
    EXPECT_FALSE(third) << "no available connections should be left";
}

// ===========================================================================
// Bug 14: ConnectPool::close never decrements size_.

TEST(Bug14, CloseDoesNotDecrementSize) {
    ConnectPool pool;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(pool.make_id());
        pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), ids.back(), i);
    }
    EXPECT_EQ(pool.count_connections(), 3u);
    for (auto id : ids) pool.close(id);
    EXPECT_EQ(pool.count_connections(), 0u);
}

// ===========================================================================
// Bug 15: rebind on an unknown id dereferences null.

TEST(Bug15, RebindUnknownId) {
    auto res = run_in_child([] {
        ConnectPool pool;
        try {
            pool.rebind(12345, 1);
        } catch (const std::exception&) {
        }
        uint64_t id = pool.make_id();
        pool.add(SocketHolder(::socket(AF_INET6, SOCK_STREAM, 0)), id, 0);
        pool.close(id);
        try {
            pool.rebind(id, 1);  // closed id
        } catch (const std::exception&) {
        }
    }, 3s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 16: reconnect busy-loop to a dead peer.

TEST(Bug16, ReconnectBusyLoop) {
    auto res = run_in_child([] {
        EndpointManager manager;
        BufferPool pool(4096);
        TcpBus bus(TcpBus::Options{.port = port_for(16, 1), .fixed_pool_size = 2}, pool, manager);
        bus.start([](auto, SharedView) {});
        LoopThread loop(bus);
        int endpoint = manager.register_endpoint("::1", port_for(16));  // nothing listens here
        try {
            bus.send(endpoint, make_view(pool, "hello"));
        } catch (const std::exception& e) {
            fprintf(stderr, "send threw: %s\n", e.what());
        }
        std::this_thread::sleep_for(100ms);
        double before = process_cpu_seconds();
        std::this_thread::sleep_for(1s);
        double used = process_cpu_seconds() - before;
        CHILD_CHECK(used < 0.25, "burnt %.2fs CPU in 1s reconnecting to a dead peer", used);
    }, 10s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 17: fix_pool_size check-then-act race -> more than fixed_pool_size connections.

TEST(Bug17, PoolSizeRace) {
    auto res = run_in_child([] {
        int lfd = raw_listen(port_for(17), /*nonblock*/ true);
        constexpr size_t kPool = 2;
        size_t worst = 0;
        for (int round = 0; round < 10; ++round) {
            EndpointManager manager;
            BufferPool pool(1 << 16);
            TcpBus bus(TcpBus::Options{.port = port_for(17, 1 + round), .fixed_pool_size = kPool}, pool, manager);
            bus.start([](auto, SharedView) {});
            int endpoint = manager.register_endpoint("::1", port_for(17));

            std::atomic<int> ready = 0;
            std::atomic<bool> go = false;
            std::vector<std::thread> threads;
            for (int t = 0; t < 16; ++t) {
                threads.emplace_back([&] {
                    ready.fetch_add(1);
                    while (!go) {
                    }
                    try {
                        bus.send(endpoint, make_view(pool, "x"));
                    } catch (const std::exception&) {
                    }
                });
            }
            while (ready.load() < 16) {
            }
            go = true;
            for (auto& t : threads) t.join();
            std::this_thread::sleep_for(100ms);

            std::vector<int> accepted;
            int fd;
            while ((fd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC)) >= 0) accepted.push_back(fd);
            worst = std::max(worst, accepted.size());
            for (int a : accepted) ::close(a);
        }
        CHILD_CHECK(worst <= kPool, "opened %zu connections with fixed_pool_size=%zu", worst, kPool);
    }, 30s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 18: async_connect bounds check is off by one (endpoint == size passes).

TEST(Bug18, AsyncConnectOffByOne) {
    EndpointManager manager;
    int e0 = manager.register_endpoint("::1", port_for(18));
    ASSERT_EQ(e0, 0);
    SocketHolder sock = manager.socket(1);
    try {
        manager.async_connect(sock, 1);  // only endpoint 0 exists
        FAIL() << "async_connect to endpoint == number of endpoints did not throw";
    } catch (const BusError& e) {
        EXPECT_STREQ(e.what(), "invalid endpoint") << "threw only because the out-of-bounds sockaddr was garbage";
    }
}

// ===========================================================================
// Bug 19: register_endpoint with an IPv4 name fails (no AI_V4MAPPED).
// (The "only first addrinfo used" half is tested in testBugsInterpose.)

TEST(Bug19, RegisterIpv4Literal) {
    EndpointManager manager;
    int endpoint = -1;
    ASSERT_NO_THROW(endpoint = manager.register_endpoint("127.0.0.1", port_for(19))) << "IPv4 literal rejected";
    // an IPv4 client (seen as ::ffff:127.0.0.1 on a dual-stack socket) must resolve to it
    int lfd = raw_listen(port_for(19, 1), false);
    int c = raw_connect4(port_for(19, 1));
    int s = accept_timeout(lfd, 1000);
    ASSERT_GE(s, 0);
    EXPECT_EQ(manager.resolve(s, port_for(19)), endpoint);
    ::close(s);
    ::close(c);
    ::close(lfd);
}

// ===========================================================================
// Bug 20: peer-controlled force_endpoint rebinds to a nonexistent endpoint;
// the reply then crashes the server.

TEST(Bug20, ForcedBogusEndpoint) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(20));
        server.start();
        {
            int fd = raw_connect6(port_for(20));
            write_frame(fd, make_greeter(port_for(20, 1), /*force*/ true, /*endpoint_id*/ 1000000));
            write_frame(fd, make_request_batch(1, 1, "bogus"));
            std::this_thread::sleep_for(300ms);
            ::close(fd);
        }
        auto resp = raw_roundtrip(port_for(20), port_for(20, 2), 7);
        CHILD_CHECK(resp && resp->seq_id() == 7, "server stopped serving after bogus peer");
    }, 15s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Minor: ~TcpBus leaks breakfd_/timerfd_/timerctlfd_.

static size_t count_open_fds() {
    size_t n = 0;
    DIR* d = opendir("/proc/self/fd");
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    closedir(d);
    return n;
}

TEST(BugMinor, TcpBusLeaksFds) {
    EndpointManager manager;
    BufferPool pool(4096);
    size_t before = count_open_fds();
    for (int i = 0; i < 3; ++i) {
        TcpBus bus(TcpBus::Options{.port = port_for(21, i)}, pool, manager);
        bus.start([](auto, SharedView) {});
    }
    EXPECT_EQ(count_open_fds(), before) << "file descriptors leaked by ~TcpBus";
}

// ===========================================================================
// Minor: inbound sockets lack TCP_NODELAY.

TEST(BugMinor, InboundNoDelay) {
    EndpointManager manager;
    BufferPool pool(4096);
    TcpBus bus(TcpBus::Options{.port = port_for(22)}, pool, manager);
    std::atomic<int> nodelay = -1;
    bus.start([&](TcpBus::ConnHandle h, SharedView) {
        int v = 0;
        socklen_t len = sizeof(v);
        getsockopt(h.socket, IPPROTO_TCP, TCP_NODELAY, &v, &len);
        nodelay = v ? 1 : 0;
    });
    {
        LoopThread loop(bus);
        int fd = raw_connect6(port_for(22));
        write_frame(fd, "hello");
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (nodelay.load() < 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        ::close(fd);
    }
    ASSERT_GE(nodelay.load(), 0) << "message not received";
    EXPECT_EQ(nodelay.load(), 1) << "accepted socket has no TCP_NODELAY";
}

// ===========================================================================
// Sanity: the raw protocol helpers work against the current code (should PASS).

TEST(Sanity, RawRoundtrip) {
    auto res = run_in_child([] {
        EndpointManager manager;
        EchoService server(manager, port_for(23));
        server.start();
        auto resp = raw_roundtrip(port_for(23), port_for(23, 1), 7);
        CHILD_CHECK(resp.has_value(), "no response");
        CHILD_CHECK(resp->seq_id() == 7, "bad seq_id");
        Operation op;
        op.ParseFromString(resp->data());
        CHILD_CHECK(op.key() == "key - mirrored", "bad payload");
    }, 15s);
    EXPECT_CHILD_OK(res);
}
