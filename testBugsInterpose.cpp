// Bug regression tests that need libc interposition, kept in a separate binary
// so the interposed symbols cannot affect other tests.
//
//  * clock_gettime(CLOCK_REALTIME) can be quantized to whole seconds, which makes
//    the "interval is a whole number of seconds" case of TcpBus::rearm_timer
//    deterministic (bug 11). libstdc++'s system_clock::now() calls clock_gettime
//    through the PLT, so the definition here takes precedence.
//  * getaddrinfo() returns two AF_INET6 results for the magic host name
//    kMultiHost (bug 19: EndpointManager only maps the first addrinfo).
//  * pthread_mutex_trylock/unlock, sendmsg and epoll_wait are wrapped so the
//    bug 2 tests can park a thread inside TcpBus::handle_write at the exact
//    point where a concurrent wakeup gets dropped. libbus.a is linked
//    statically into this binary, so its calls bind to these definitions.
//    All wrappers are pass-through unless a test arms them.

#include "test_helpers.h"

#include "endpoint_manager.h"

#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>

#include <set>

using namespace bus;
using namespace bus_test;
using namespace std::chrono_literals;

static std::atomic<bool> g_quantize_realtime = false;

extern "C" int clock_gettime(clockid_t clk, struct timespec* ts) {
    int r = syscall(SYS_clock_gettime, clk, ts);
    if (r == 0 && clk == CLOCK_REALTIME && g_quantize_realtime.load(std::memory_order_relaxed)) {
        ts->tv_nsec = 0;
    }
    return r;
}

static constexpr const char* kMultiHost = "bus-test-multi-address";

namespace {
struct FakeAddrInfo {
    addrinfo ai[2];
    sockaddr_in6 sa[2];
};
}  // namespace

static std::mutex g_fake_mutex;
static std::set<addrinfo*> g_fake_lists;

extern "C" int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    if (node && strcmp(node, kMultiHost) == 0) {
        auto* f = new FakeAddrInfo{};
        const char* addrs[2] = {"::1", "::ffff:127.0.0.1"};
        for (int i = 0; i < 2; ++i) {
            f->sa[i].sin6_family = AF_INET6;
            inet_pton(AF_INET6, addrs[i], &f->sa[i].sin6_addr);
            f->ai[i].ai_family = AF_INET6;
            f->ai[i].ai_socktype = SOCK_STREAM;
            f->ai[i].ai_protocol = IPPROTO_TCP;
            f->ai[i].ai_addrlen = sizeof(sockaddr_in6);
            f->ai[i].ai_addr = reinterpret_cast<sockaddr*>(&f->sa[i]);
            f->ai[i].ai_next = i == 0 ? &f->ai[1] : nullptr;
        }
        *res = &f->ai[0];
        std::lock_guard lock(g_fake_mutex);
        g_fake_lists.insert(*res);
        return 0;
    }
    using Fn = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "getaddrinfo"));
    return real(node, service, hints, res);
}

extern "C" void freeaddrinfo(addrinfo* ai) {
    {
        std::lock_guard lock(g_fake_mutex);
        if (g_fake_lists.erase(ai)) {
            delete reinterpret_cast<FakeAddrInfo*>(ai);
            return;
        }
    }
    using Fn = void (*)(addrinfo*);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "freeaddrinfo"));
    real(ai);
}

// ---------------------------------------------------------------------------
// Bug 2 instrumentation.

namespace {

using MutexFn = int (*)(pthread_mutex_t*);

// No function-local statics here: their init guard may itself take a mutex.
MutexFn real_mutex_fn(std::atomic<MutexFn>& slot, const char* name) {
    MutexFn fn = slot.load(std::memory_order_acquire);
    if (!fn) {
        fn = reinterpret_cast<MutexFn>(dlsym(RTLD_NEXT, name));
        slot.store(fn, std::memory_order_release);
    }
    return fn;
}
std::atomic<MutexFn> g_real_trylock = nullptr;
std::atomic<MutexFn> g_real_unlock = nullptr;

enum class Role { None, GatedWriter, LateSender, EagainWriter };
thread_local Role t_role = Role::None;

// mutexes the GatedWriter thread currently holds through a successful trylock
// (in TcpBus that is only ConnData::egress_data, taken by handle_write)
thread_local pthread_mutex_t* t_trylocked[8];
thread_local int t_trylocked_cnt = 0;

std::atomic<bool> g_unlock_gate_armed = false;
std::atomic<bool> g_unlock_gate_hit = false;
std::atomic<int> g_late_trylock_busy = 0;

std::atomic<bool> g_eagain_gate_armed = false;
std::atomic<bool> g_eagain_gate_hit = false;
std::atomic<uint64_t> g_tick = 0;
std::atomic<uint64_t> g_epollout_tick = 0;  // last epoll_wait return reporting EPOLLOUT
std::atomic<uint64_t> g_enter_tick = 0;     // last epoll_wait entry
std::function<void()> g_on_eagain;           // runs inside the EAGAIN gate

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        usleep(100);
    }
    return true;
}

}  // namespace

extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) noexcept {
    int r = real_mutex_fn(g_real_trylock, "pthread_mutex_trylock")(m);
    if (t_role == Role::GatedWriter && r == 0 && t_trylocked_cnt < 8) {
        t_trylocked[t_trylocked_cnt++] = m;
    } else if (t_role == Role::LateSender && r == EBUSY) {
        g_late_trylock_busy.fetch_add(1);
    }
    return r;
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t* m) noexcept {
    if (t_role == Role::GatedWriter) {
        for (int i = 0; i < t_trylocked_cnt; ++i) {
            if (t_trylocked[i] != m) continue;
            t_trylocked[i] = t_trylocked[--t_trylocked_cnt];
            // About to drop the egress lock at the end of handle_write: the queue
            // was found empty and pending_messages_ is already released. Hold here
            // until a concurrent sender has queued a message and failed its own
            // trylock (or give up after a while, e.g. if the sender blocks instead).
            if (g_unlock_gate_armed.exchange(false)) {
                g_unlock_gate_hit = true;
                wait_for([] { return g_late_trylock_busy.load() > 0; }, 1000ms);
            }
            break;
        }
    }
    return real_mutex_fn(g_real_unlock, "pthread_mutex_unlock")(m);
}

extern "C" ssize_t sendmsg(int fd, const msghdr* msg, int flags) {
    using Fn = ssize_t (*)(int, const msghdr*, int);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "sendmsg"));
    ssize_t r = real(fd, msg, flags);
    if (r < 0 && errno == EAGAIN && t_role == Role::EagainWriter && g_eagain_gate_armed.exchange(false)) {
        // The socket is full and this thread still holds the egress lock.
        // Let the peer drain it, then wait until the loop thread has received the
        // resulting EPOLLOUT edge and gone back to epoll_wait (i.e. its
        // handle_write() call is over) before reporting EAGAIN to the caller.
        uint64_t start = g_tick.load();
        g_eagain_gate_hit = true;
        g_on_eagain();
        wait_for([&] {
            uint64_t out = g_epollout_tick.load();
            return out > start && g_enter_tick.load() > out;
        }, 1000ms);
        errno = EAGAIN;
    }
    return r;
}

extern "C" int epoll_wait(int epfd, epoll_event* events, int maxevents, int timeout) {
    using Fn = int (*)(int, epoll_event*, int, int);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "epoll_wait"));
    g_enter_tick = g_tick.fetch_add(1) + 1;
    int r = real(epfd, events, maxevents, timeout);
    int saved = errno;
    for (int i = 0; i < r; ++i) {
        if (events[i].events & EPOLLOUT) {
            g_epollout_tick = g_tick.fetch_add(1) + 1;
            break;
        }
    }
    errno = saved;
    return r;
}

namespace {

// Raw TCP peer that counts received bytes; reading can be paused.
struct ByteSink {
    int lfd;
    std::atomic<size_t> bytes = 0;
    std::atomic<bool> pause = false;
    std::atomic<bool> paused = false;
    std::atomic<bool> stop = false;
    std::thread t;

    explicit ByteSink(int port) : lfd(raw_listen(port, true)) {
        t = std::thread([this] {
            std::vector<int> fds;
            char buf[1 << 16];
            while (!stop) {
                if (pause) {
                    paused = true;
                    usleep(200);
                    continue;
                }
                paused = false;
                std::vector<pollfd> p{{lfd, POLLIN, 0}};
                for (int fd : fds) p.push_back({fd, POLLIN, 0});
                if (poll(p.data(), p.size(), 5) <= 0) continue;
                if (p[0].revents & POLLIN) {
                    int fd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (fd >= 0) fds.push_back(fd);
                }
                for (size_t i = 1; i < p.size(); ++i) {
                    ssize_t r;
                    while ((r = ::read(p[i].fd, buf, sizeof(buf))) > 0) bytes.fetch_add(r);
                }
            }
        });
    }

    void set_paused(bool v) {
        pause = v;
        wait_for([&] { return paused.load() == v; }, 1000ms);
    }

    bool wait_bytes(size_t n, std::chrono::milliseconds timeout) {
        return wait_for([&] { return bytes.load() >= n; }, timeout);
    }

    ~ByteSink() {
        stop = true;
        t.join();
    }
};

struct LoopThread {
    TcpBus& bus;
    std::thread t;
    explicit LoopThread(TcpBus& b) : bus(b), t([this] { bus.loop(); }) {}
    ~LoopThread() {
        bus.to_break();
        t.join();
    }
};

constexpr int kBug02Port = 15000 + 20 * 2 + 10;

}  // namespace

// ===========================================================================
// Bug 2: handle_write takes the egress lock with try_get() and silently returns
// when it is busy, assuming the current holder will pick the work up. Nothing
// guarantees that: the holder may already have decided it is done.

// Writer A finds the queue empty, releases pending_messages_ and is about to
// release the egress lock. Sender B queues a message, finds the connection
// available, loses try_get() to A and returns. A returns too: B's message sits
// in the queue until some unrelated send() or EPOLLOUT edge comes along.
TEST(Bug02, LostWakeupConcurrentSenders) {
    auto res = run_in_child([] {
        ByteSink sink(kBug02Port);
        EndpointManager manager;
        BufferPool pool(1 << 16);
        TcpBus sender(TcpBus::Options{.port = kBug02Port + 1, .min_pool_size = 1}, pool, manager);
        sender.start([](auto, SharedView) {});
        LoopThread loop(sender);
        int endpoint = manager.register_endpoint("::1", kBug02Port);
        const std::string payload(100, 'x');
        const size_t frame_len = internal::header_len + payload.size();

        // establish the connection; the loop thread marks it available
        sender.send(endpoint, make_view(pool, payload));
        CHILD_CHECK(sink.wait_bytes(frame_len, 3000ms), "warm-up message not delivered");

        g_unlock_gate_armed = true;
        std::thread writer([&] {
            t_role = Role::GatedWriter;
            sender.send(endpoint, make_view(pool, payload));
            t_role = Role::None;
        });
        if (!wait_for([] { return g_unlock_gate_hit.load(); }, 2000ms)) {
            fprintf(stderr, "note: unlock gate never hit (handle_write no longer uses trylock?)\n");
        }
        t_role = Role::LateSender;
        sender.send(endpoint, make_view(pool, payload));
        t_role = Role::None;
        writer.join();

        sink.wait_bytes(3 * frame_len, 2000ms);
        CHILD_CHECK(sink.bytes.load() == 3 * frame_len,
                    "received %zu of %zu bytes: message stuck in queue (late sender lost try_get: %d)",
                    sink.bytes.load(), 3 * frame_len, g_late_trylock_busy.load());
    }, 20s);
    EXPECT_CHILD_OK(res);
}

// A sender thread hits EAGAIN while holding the egress lock. Before it returns,
// the peer drains the socket and the loop thread gets the (edge-triggered)
// EPOLLOUT, loses try_get() and drops it. No further edge will come, so the
// rest of the data never leaves the process.
TEST(Bug02, LostEpolloutDuringEagain) {
    auto res = run_in_child([] {
        ByteSink sink(kBug02Port + 2);
        EndpointManager manager;
        BufferPool pool(1 << 20);
        TcpBus sender(TcpBus::Options{.port = kBug02Port + 3, .min_pool_size = 1, .max_message_size = 1 << 20},
                      pool, manager);
        sender.start([](auto, SharedView) {});
        LoopThread loop(sender);
        int endpoint = manager.register_endpoint("::1", kBug02Port + 2);

        const std::string small(100, 'x');
        size_t expected = internal::header_len + small.size();
        sender.send(endpoint, make_view(pool, small));
        CHILD_CHECK(sink.wait_bytes(expected, 3000ms), "warm-up message not delivered");

        sink.set_paused(true);
        g_on_eagain = [&] { sink.pause = false; };
        g_eagain_gate_armed = true;
        const std::string big(1 << 16, 'y');
        t_role = Role::EagainWriter;
        for (int i = 0; i < 4000 && !g_eagain_gate_hit; ++i) {
            sender.send(endpoint, make_view(pool, big));
            expected += internal::header_len + big.size();
        }
        t_role = Role::None;
        CHILD_CHECK(g_eagain_gate_hit.load(), "sender never hit EAGAIN");
        sink.set_paused(false);

        sink.wait_bytes(expected, 3000ms);
        CHILD_CHECK(sink.bytes.load() == expected, "received %zu of %zu bytes: data stuck after lost EPOLLOUT",
                    sink.bytes.load(), expected);
    }, 30s);
    EXPECT_CHILD_OK(res);
}

// ===========================================================================
// Bug 11: rearm_timer checks only the sub-second remainder, so an action due in
// exactly N whole seconds fires immediately.

TEST(Bug11, WholeSecondDeadlineFiresImmediately) {
    EndpointManager manager;
    BufferPool pool(4096);
    TcpBus bus(TcpBus::Options{.port = 15000 + 20 * 11}, pool, manager);
    bus.start([](auto, SharedView) {});
    std::thread loop([&] { bus.loop(); });

    g_quantize_realtime = true;
    std::atomic<bool> fired = false;
    auto start = std::chrono::steady_clock::now();
    std::atomic<int64_t> fired_after_ms = -1;
    // with the quantized clock, "now" seen by rearm_timer equals this "now"
    // (unless a second boundary is crossed), so the interval is exactly 2s
    bus.schedule_point([&] {
            fired_after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            fired = true;
        }, std::chrono::system_clock::now() + 2s);

    auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!fired && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    g_quantize_realtime = false;
    bus.to_break();
    loop.join();

    ASSERT_TRUE(fired.load()) << "action never fired";
    // the fake wall clock reaches the deadline between 1s and 2s of real time later
    EXPECT_GE(fired_after_ms.load(), 900) << "action due in 2s fired after " << fired_after_ms.load() << "ms";
}

// ===========================================================================
// Bug 19: add_address iterates with `i` but reads `info`, so only the first
// address of a multi-address name is mapped to the endpoint.

TEST(Bug19, AllAddrinfoResultsMapped) {
    EndpointManager manager;
    const int port = 15000 + 20 * 19 + 10;
    int endpoint = manager.register_endpoint(kMultiHost, port);

    int lfd = raw_listen(15000 + 20 * 19 + 11, false);
    int c6 = raw_connect6(15000 + 20 * 19 + 11);
    int s6 = accept_timeout(lfd, 1000);
    int c4 = raw_connect4(15000 + 20 * 19 + 11);  // peer appears as ::ffff:127.0.0.1
    int s4 = accept_timeout(lfd, 1000);
    ASSERT_GE(s6, 0);
    ASSERT_GE(s4, 0);

    EXPECT_EQ(manager.resolve(s6, port), endpoint) << "first address (::1) not mapped";
    EXPECT_EQ(manager.resolve(s4, port), endpoint) << "second address (::ffff:127.0.0.1) not mapped";

    for (int fd : {c6, s6, c4, s4, lfd}) ::close(fd);
}
