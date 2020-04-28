#pragma once

#include "fwd.h"

#include "connect_pool.h"

#include <sys/socket.h>
#include <memory>
#include <optional>

namespace bus {

class EndpointManager {
public:
    static constexpr int unbound_v6 = -1;

    struct IncomingConnection {
        SocketHolder sock_;
        int errno_;
        int endpoint_;
    };

public:
    EndpointManager();

    int register_endpoint(std::string addr, int port) {
        return add_address(addr, port, std::nullopt);
    }

    void merge_to_endpoint(std::string addr, int port, int merge_to) {
        add_address(addr, port, merge_to);
    }

    SocketHolder socket(int endpoint);
    void async_connect(SocketHolder& sock, int endpoint);
    IncomingConnection accept(int listen_socket);

    bool transient(int endpoint) {
        return endpoint < 0;
    }

    int resolve(int sock, int port);

    ~EndpointManager();

private:
    int add_address(std::string addr, int port, std::optional<int> merge_to);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
