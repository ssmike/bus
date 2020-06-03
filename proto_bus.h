#pragma once

#include "bus.h"
#include "error.h"
#include "future.h"

#include <functional>

namespace bus {

class ProtoBus {
public:
    struct BatchOptions {
        size_t max_batch = 1;
        std::chrono::system_clock::duration max_delay = std::chrono::hours(1);
    };

    struct Options {
        TcpBus::Options tcp_opts;
        BatchOptions batch_opts;
        std::optional<uint64_t> greeter;
    };

public:
    ProtoBus(Options opts, EndpointManager& manager);
    ~ProtoBus();

    void start();

    template<typename RequestProto, typename ResponseProto>
    Future<ErrorT<ResponseProto>> send(RequestProto proto, int endpoint, uint64_t method, std::chrono::duration<double> timeout) {
        return send_raw(proto.SerializeAsString(), endpoint, method, timeout).map(
            [=](ErrorT<std::string>& resp) -> ErrorT<ResponseProto> {
                if (!resp) {
                    return ErrorT<ResponseProto>::error(resp.what());
                } else {
                    ResponseProto proto;
                    proto.ParseFromString(resp.unwrap());
                    return ErrorT<ResponseProto>::value(std::move(proto));
                }
            });
    }

    Executor& executor();

protected:
    template<typename RequestProto, typename ResponseProto>
    void register_handler(uint32_t method, std::function<Future<ResponseProto>(int, RequestProto)> handler) {
        register_raw_handler(method, [handler=std::move(handler)] (int endp, std::string str, std::function<void(std::string)> cb) {
                RequestProto proto;
                proto.ParseFromString(str);
                handler(endp, std::move(proto)).subscribe([cb=std::move(cb)] (ResponseProto& proto) { cb(proto.SerializeAsString()); });
            });
    }

    template<typename RequestProto, typename ResponseProto>
    void register_handler(uint32_t method, std::function<void(int, RequestProto, Promise<ResponseProto>)> handler) {
        register_raw_handler(method, [handler=std::move(handler)] (int endp, std::string str, std::function<void(std::string)> cb) {
                RequestProto proto;
                proto.ParseFromString(str);
                Promise<ResponseProto> promise;
                promise.future().subscribe([cb=std::move(cb)] (ResponseProto& proto) { cb(proto.SerializeAsString()); });
                handler(endp, std::move(proto), promise);
            });
    }

private:
    Future<ErrorT<std::string>> send_raw(std::string serialized, int endpoint, uint64_t method, std::chrono::duration<double> timeout);

    void register_raw_handler(uint32_t method, std::function<void(int, std::string, std::function<void(std::string)>)> handler);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
