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

    std::thread t([&] {
        BufferPool bufferPool{4098};
        EndpointManager manager;
        TcpBus first(TcpBus::Options{.port = 4001, .fixed_pool_size = 2}, bufferPool, manager);

        size_t messages_received = 0;

        first.start([&](int endp, SharedView view) {
                Operation op2;
                op2.ParseFromArray(view.data(), view.size());
                assert(op2.key() == "key");
                assert(op2.data() == "data");

                if ((++messages_received) == messages_count) {
                    exit(0);
                }
            });

        first.loop();
    });

    for (size_t i = 0; i < messages_count; ++i) {
        Operation op;
        op.set_data("data");
        op.set_key("key");

        ScopedBuffer buffer{bufferPool, op.ByteSizeLong()};
        op.SerializeToArray(buffer.get().data(), buffer.get().size());

        second.send(manager.register_endpoint("::1", 4001), std::move(buffer));
    }

    second.loop();

    t.join();
}
