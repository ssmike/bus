#pragma once

#include "buffer.h"
#include "endpoint_manager.h"
#include "fwd.h"

#include <functional>
#include <memory>

namespace bus {

class TcpBus {
public:
    struct Options {
        int port = 80;
        size_t fixed_pool_size = 6;
        size_t listener_backlog = 60;
        size_t max_message_size = 4098;
        std::optional<size_t> max_pending_messages;
    };

    struct ConnHandle {
        int endpoint;
        int socket;
        uint64_t conn_id;
    };

public:
    TcpBus(Options, BufferPool&, EndpointManager&);

    void start(std::function<void(ConnHandle, SharedView)>);

    void clear_queue(int endpoint);
    bool send(int endpoint, SharedView);
    void answer(uint64_t conn_id, SharedView);

    // greeter interface
    void set_greeter(std::function<std::optional<SharedView>(int endpoint)>);
    void close(uint64_t conn_id);
    void rebind(uint64_t conn_id, int new_endpoint);

    void loop();
    void to_break();

    ~TcpBus();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
