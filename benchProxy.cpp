#include "bus.h"
#include "messages.pb.h"

#include <chrono>
#include <thread>

using namespace bus;

int main() {
    EndpointManager manager;
    int backend_endpoint = manager.register_endpoint("::1", 4001);
    int proxy_endpoint = manager.register_endpoint("::1", 4002);
    int client_endpoint = manager.register_endpoint("::1", 4003);

    std::thread backend([&] {
            BufferPool bufferPool{4098};
            TcpBus bus(TcpBus::Options{.port = 4001, .fixed_pool_size = 7}, bufferPool, manager);
            bus.start([&](auto handle, auto blob) {
                    Operation op;
                    op.set_key("answer");

                    SharedView buffer{bufferPool, op.ByteSizeLong()};
                    op.SerializeToArray(buffer.data(), buffer.size());

                    bus.send(proxy_endpoint, buffer);
                });
            bus.loop();
        });

    std::thread proxy([&] {
            BufferPool bufferPool{4098};
            TcpBus bus(TcpBus::Options{.port = 4002, .fixed_pool_size = 7}, bufferPool, manager);
            bus.start([&](auto handle, auto blob) {
                    Operation op;
                    op.ParseFromArray(blob.data(), blob.size());
                    if (op.key() == "to_proxy") {
                        bus.send(backend_endpoint, blob);
                    } else {
                        bus.send(client_endpoint, blob);
                    }
                });
            bus.loop();
        });

    size_t messages_count = 1000000;
    std::atomic<size_t> messages_received = 0;

    BufferPool bufferPool{4098};
    TcpBus bus(TcpBus::Options{.port = 4003, .fixed_pool_size = 7}, bufferPool, manager);
    std::thread client([&] {
            bus.start([&](auto handle, auto blob) {
                    messages_received.fetch_add(1);
                });
            bus.loop();
        });


    auto pt = std::chrono::steady_clock::now();
    for (size_t i = 0; i < messages_count; ++i) {
        Operation op;
        op.set_value("value");
        op.set_key("to_proxy");

        SharedView buffer{bufferPool, op.ByteSizeLong()};
        op.SerializeToArray(buffer.data(), buffer.size());

        bus.send(proxy_endpoint, buffer);
    }
    while (messages_received.load() != messages_count) {}

    std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pt).count() << "/" << messages_count << std::endl;

    proxy.join();
    client.join();
    backend.join();
}
