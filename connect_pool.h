#pragma once

#include "fwd.h"
#include "buffer.h"
#include "lock.h"
#include "util.h"

#include <atomic>
#include <optional>
#include <stdint.h>
#include <cstddef>
#include <utility>

namespace bus {

class SocketHolder {
public:
    SocketHolder(int sock): sock_(sock) {}
    SocketHolder(): SocketHolder(kInvalidSocket) {}

    SocketHolder(const SocketHolder&) = delete;

    SocketHolder(SocketHolder&& oth)
        : SocketHolder(oth.release())
    {
    }

    void operator =(SocketHolder&& oth) {
        std::swap(sock_, oth.sock_);
    }

    int get() {
        return sock_;
    }

    int release() {
        int res = sock_;
        sock_ = kInvalidSocket;
        return res;
    }


    ~SocketHolder();

private:
    int sock_ = kInvalidSocket;
    static constexpr int kInvalidSocket = -1;
};

struct ConnData {
    struct EgressData {
        // doesn't include header
        std::optional<SharedView> message;
        uint64_t offset = 0;
    };

    internal::ExclusiveWrapper<EgressData> egress_data;

    char ingress_header[internal::header_len];
    SharedView ingress_buf;
    size_t ingress_offset = 0;

    SocketHolder socket;

    int endpoint;
    uint64_t id;
};

class ConnectPool {
public:
    ConnectPool();

    size_t make_id();

    std::shared_ptr<ConnData> add(SocketHolder, uint64_t id, int endpoint);

    std::shared_ptr<ConnData> select(uint64_t);

    // makes unavailable
    std::shared_ptr<ConnData> take_available(int endpoint);

    void set_available(uint64_t);
    void set_unavailable(uint64_t);

    size_t count_connections(int endpoint);
    size_t count_connections();

    void rebind(uint64_t, int endpoint);

    void close(uint64_t);
    void close_old_conns(size_t cnt);

    ~ConnectPool();

private:
    class Impl;
    bus::internal::ExclusiveWrapper<std::unique_ptr<Impl>> impl_;

    std::atomic<uint64_t> id_ = 0;
    std::atomic<uint64_t> size_ = 0;
};

}
