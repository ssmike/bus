// Bug regression tests that need libc interposition, kept in a separate binary
// so the interposed symbols cannot affect other tests.
//
//  * clock_gettime(CLOCK_REALTIME) can be quantized to whole seconds, which makes
//    the "interval is a whole number of seconds" case of TcpBus::rearm_timer
//    deterministic (bug 11). libstdc++'s system_clock::now() calls clock_gettime
//    through the PLT, so the definition here takes precedence.
//  * getaddrinfo() returns two AF_INET6 results for the magic host name
//    kMultiHost (bug 19: EndpointManager only maps the first addrinfo).

#include "test_helpers.h"

#include "endpoint_manager.h"

#include <dlfcn.h>
#include <netdb.h>
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
