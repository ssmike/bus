#pragma once

// Shared helpers for the bug regression tests (testBugs.cpp, testBugsInterpose.cpp).

#include "bus.h"
#include "proto_bus.h"
#include "util.h"

#include "service.pb.h"
#include "messages.pb.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bus_test {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// fork()-based isolation: run `body` in a child process, wait at most `timeout`.
// The child exits 0 on success; any crash / std::terminate / signal / hang is
// reported to the parent.

struct ChildResult {
    bool timed_out = false;
    int status = 0;

    bool ok() const {
        return !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    std::string describe() const {
        if (timed_out) {
            return "child timed out (hang/deadlock) and was killed";
        }
        if (WIFSIGNALED(status)) {
            return std::string("child killed by signal ") + std::to_string(WTERMSIG(status)) +
                   " (" + strsignal(WTERMSIG(status)) + ")";
        }
        if (WIFEXITED(status)) {
            return "child exited with code " + std::to_string(WEXITSTATUS(status));
        }
        return "child: unknown status";
    }
};

inline ChildResult run_in_child(std::function<void()> body, std::chrono::milliseconds timeout) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        abort();
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        try {
            body();
        } catch (const std::exception& e) {
            fprintf(stderr, "child: uncaught exception: %s\n", e.what());
            fflush(stderr);
            _exit(70);
        } catch (...) {
            fprintf(stderr, "child: uncaught non-std exception\n");
            fflush(stderr);
            _exit(71);
        }
        fflush(stdout);
        fflush(stderr);
        // _exit: skip destructors (several library destructors hang, see bug 8)
        _exit(0);
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return {false, status};
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return {true, status};
        }
        usleep(5000);
    }
}

#define CHILD_CHECK(cond, ...)                                                        \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "CHILD_CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                           \
            fprintf(stderr, "  " __VA_ARGS__);                                        \
            fprintf(stderr, "\n");                                                    \
            fflush(stderr);                                                           \
            _exit(1);                                                                 \
        }                                                                             \
    } while (0)

#define EXPECT_CHILD_OK(res) EXPECT_TRUE((res).ok()) << (res).describe()

// ---------------------------------------------------------------------------
// CPU time accounting

inline double process_cpu_seconds() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto tv = [](timeval t) { return t.tv_sec + t.tv_usec / 1e6; };
    return tv(ru.ru_utime) + tv(ru.ru_stime);
}

// ---------------------------------------------------------------------------
// Futures with timeout

template <typename T>
struct WaitState {
    std::mutex m;
    std::condition_variable cv;
    std::optional<T> v;
};

template <typename T>
std::optional<T> wait_future(bus::Future<T> f, std::chrono::milliseconds timeout) {
    auto st = std::make_shared<WaitState<T>>();
    f.subscribe([st](T& v) {
        std::unique_lock lock(st->m);
        if (!st->v) st->v.emplace(v);
        st->cv.notify_all();
    });
    std::unique_lock lock(st->m);
    st->cv.wait_for(lock, timeout, [&] { return st->v.has_value(); });
    return st->v;
}

// ---------------------------------------------------------------------------
// Raw socket helpers (blocking, independent of the library under test)

inline void write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        ssize_t r = ::send(fd, data, size, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd, POLLOUT, 0};
                poll(&p, 1, 100);
                continue;
            }
            throw std::runtime_error(std::string("raw write failed: ") + strerror(errno));
        }
        data += r;
        size -= r;
    }
}

inline std::string frame(const std::string& payload) {
    std::string out(bus::internal::header_len, '\0');
    bus::internal::write_header(payload.size(), out.data());
    return out + payload;
}

inline void write_frame(int fd, const std::string& payload) {
    auto f = frame(payload);
    write_all(fd, f.data(), f.size());
}

inline int raw_connect6(int port, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback;
        addr.sin6_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("raw connect failed");
        }
        std::this_thread::sleep_for(10ms);
    }
}

inline int raw_connect4(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("raw connect4 failed");
    }
    return fd;
}

// dual-stack listener on [::]:port
inline int raw_listen(int port, bool nonblock = true) {
    int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int zero = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 128) != 0) {
        throw std::runtime_error(std::string("raw listen failed: ") + strerror(errno));
    }
    return fd;
}

inline int accept_timeout(int lfd, int timeout_ms) {
    pollfd p{lfd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    return accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
}

inline std::string make_greeter(int port, bool force = false, uint64_t endpoint_id = 0) {
    bus::detail::Greeter g;
    g.set_port(port);
    g.set_force_endpoint(force);
    g.set_endpoint_id(endpoint_id);
    return g.SerializeAsString();
}

inline std::string make_request_batch(uint64_t seq_id, uint32_t method, const std::string& key) {
    Operation op;
    op.set_key(key);
    op.set_value("value");
    bus::detail::MessageBatch batch;
    auto* item = batch.add_item();
    item->set_seq_id(seq_id);
    item->set_type(bus::detail::Message::REQUEST);
    item->set_method(method);
    item->set_data(op.SerializeAsString());
    return batch.SerializeAsString();
}

// A raw peer emulating the client side of the ProtoBus protocol: accepts the
// server's outbound connections, skips the Greeter on each, collects messages.
class RawPeer {
public:
    explicit RawPeer(int port) : lfd_(raw_listen(port)) {}

    ~RawPeer() {
        for (auto& c : conns_) {
            if (c.owned) ::close(c.fd);
        }
        ::close(lfd_);
    }

    // Watch an already-connected socket too (no Greeter expected on it): the
    // server may answer on the inbound connection the request came from.
    void adopt(int fd) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        conns_.push_back({fd, {}, true, false});
    }

    std::optional<bus::detail::Message> wait_response(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (responses_.empty()) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return std::nullopt;
            pump(left.count());
        }
        auto r = responses_.front();
        responses_.pop_front();
        return r;
    }

private:
    struct Conn {
        int fd;
        std::string buf;
        bool greeted = false;
        bool owned = true;
    };

    void pump(int timeout_ms) {
        std::vector<pollfd> fds;
        fds.push_back({lfd_, POLLIN, 0});
        for (auto& c : conns_) fds.push_back({c.fd, POLLIN, 0});
        if (poll(fds.data(), fds.size(), std::min(timeout_ms, 50)) <= 0) return;
        if (fds[0].revents & POLLIN) {
            int fd;
            while ((fd = accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
                conns_.push_back({fd});
            }
        }
        for (auto& c : conns_) {
            char buf[65536];
            ssize_t r;
            while ((r = ::read(c.fd, buf, sizeof(buf))) > 0) {
                c.buf.append(buf, r);
            }
            while (c.buf.size() >= bus::internal::header_len) {
                size_t len = bus::internal::read_header(c.buf.data());
                if (c.buf.size() < bus::internal::header_len + len) break;
                std::string payload = c.buf.substr(bus::internal::header_len, len);
                c.buf.erase(0, bus::internal::header_len + len);
                if (!c.greeted) {
                    c.greeted = true;
                    continue;
                }
                bus::detail::MessageBatch batch;
                batch.ParseFromString(payload);
                for (auto& item : batch.item()) {
                    if (item.type() == bus::detail::Message::RESPONSE) {
                        responses_.push_back(item);
                    }
                }
            }
        }
    }

    int lfd_;
    std::vector<Conn> conns_;
    std::deque<bus::detail::Message> responses_;
};

// Full request/response round trip against a ProtoBus EchoService using raw
// sockets only. Returns the response (if any) within the timeout.
inline std::optional<bus::detail::Message> raw_roundtrip(int server_port, int client_port, uint64_t seq_id,
                                                        std::chrono::milliseconds timeout = 3000ms) {
    RawPeer peer(client_port);
    int fd = raw_connect6(server_port);
    write_frame(fd, make_greeter(client_port));
    write_frame(fd, make_request_batch(seq_id, 1, "key"));
    peer.adopt(fd);
    auto resp = peer.wait_response(timeout);
    ::close(fd);
    return resp;
}

// ProtoBus server with a mirroring handler for method 1.
class EchoService : public bus::ProtoBus {
public:
    EchoService(bus::EndpointManager& manager, int port, size_t fixed_pool_size = 1, size_t max_message_size = 4098)
        : ProtoBus({.tcp_opts = bus::TcpBus::Options{.port = port,
                                                     .fixed_pool_size = fixed_pool_size,
                                                     .max_message_size = max_message_size},
                    .batch_opts = {.max_batch = 1, .max_delay = std::chrono::seconds(1)}},
                   manager)
    {
        register_handler<Operation, Operation>(1, [](int, Operation op) -> bus::Future<Operation> {
            op.set_key(op.key() + " - mirrored");
            return bus::make_future(std::move(op));
        });
    }
};

inline bus::SharedView make_view(bus::BufferPool& pool, const std::string& s) {
    bus::SharedView v(pool, s.size());
    memcpy(v.data(), s.data(), s.size());
    return v;
}

}  // namespace bus_test
