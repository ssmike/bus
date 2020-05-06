#include "bus.h"
#include "util.h"
#include "messages.pb.h"

#include <thread>

using namespace bus;

int main() {
    BufferPool bufferPool{4098};
    EndpointManager manager;

    TcpBus second(TcpBus::Options{.port = 4002, .fixed_pool_size = 2}, bufferPool, manager);

    constexpr size_t messages_count = 4000;

    std::string key = "key";
    std::string value = "value";

    for (size_t i = 0; i < 110; ++i) {
        key += "1";
        value += "1";
    }

    std::thread t([&] {
        BufferPool bufferPool{4098};
        EndpointManager manager;
        TcpBus first(TcpBus::Options{.port = 4001, .fixed_pool_size = 2}, bufferPool, manager);

        size_t messages_received = 0;

        first.start([&](auto endp, SharedView view) {
                Operation op2;
                op2.ParseFromArray(view.data(), view.size());
                assert(op2.key() == key);
                assert(op2.value() == value);
                if ((++messages_received) == messages_count) {
                    exit(0);
                }
            });

        first.loop();
    });

    int endpoint = manager.register_endpoint("::1", 4001);
    for (size_t i = 0; i < messages_count; ++i) {
        Operation op;
        op.set_value(value);
        op.set_key(key);

        SharedView buffer{bufferPool, op.ByteSizeLong()};
        op.SerializeToArray(buffer.data(), buffer.size());

        second.send(endpoint, std::move(buffer));
    }

    second.loop();

    t.join();
}
