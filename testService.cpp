#include "proto_bus.h"
#include "bus.h"
#include "util.h"

#include "messages.pb.h"


using namespace bus;

internal::Event event;

class SimpleService: ProtoBus {
public:
    SimpleService(EndpointManager& manager, int port, bool receiver)
        : ProtoBus({.tcp_opts=TcpBus::Options{.port=port, .fixed_pool_size=2}, .batch_opts={.max_batch=2, .max_delay=std::chrono::seconds(1)}}, manager)
    {
        if (receiver) {
            register_handler<Operation, Operation>(1, [&](int, Operation op) -> Future<Operation> {
                op.set_key(op.key() + " - mirrored");
                return make_future(std::move(op));
            });
        }

        ProtoBus::start();
    }

    std::chrono::steady_clock::duration bench(int endpoint, size_t repeats) {
        Operation op;
        op.set_key("key");
        op.set_value("value");

        std::chrono::steady_clock::duration sum = std::chrono::steady_clock::duration::zero();
        for (size_t i = 0; i < repeats; ++i) {
            auto pt = std::chrono::steady_clock::now();
            auto f1 = send<Operation, Operation>(op, endpoint, 1, std::chrono::seconds(4));
            auto f2 = send<Operation, Operation>(op, endpoint, 1, std::chrono::seconds(4));
            f1.wait().unwrap();
            f2.wait().unwrap();
            sum += (std::chrono::steady_clock::now() - pt);
        }

        return sum / repeats;
    }

    void execute(int endpoint) {
        Operation op;
        op.set_key("key");
        op.set_value("value");

        send<Operation, Operation>(op, endpoint, 1, std::chrono::seconds(4))
            .subscribe([=](ErrorT<Operation>& op) {
                    assert(op);
                    assert(op.unwrap().key() == "key - mirrored");
                    assert(op.unwrap().value() == "value");
                    std::cerr << "OK" << std::endl;
                    event.notify();
                });
    }

private:
};

int main() {
    EndpointManager manager;

    SimpleService second(manager, 4002, false);
    SimpleService first(manager, 4003, true);
    int receiver = manager.register_endpoint("::1", 4003);
    second.execute(receiver);
    event.wait();

    std::cerr << "round-trip thru loopback " << std::chrono::duration_cast<std::chrono::nanoseconds>(second.bench(receiver, 1000)).count() << std::endl;
}
