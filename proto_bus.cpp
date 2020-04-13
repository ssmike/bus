#include "proto_bus.h"
#include "executor.h"

#include "service.pb.h"

namespace bus {
    class ProtoBus::Impl {
    public:
        Impl(Options opts, EndpointManager& manager)
            : greeter_(opts.greeter)
            , endpoint_manager_(manager)
            , pool_{ 2 * opts.tcp_opts.max_message_size }
            , bus_(opts.tcp_opts, pool_, manager)
            , batch_opts_(opts.batch_opts)
            , loop_([&] { bus_.loop(); }, std::chrono::seconds::zero())
        {
            bus_.set_greeter([=] (int endpoint) {
                    detail::Greeter greeter;
                    greeter.set_port(opts.tcp_opts.port);
                    greeter.set_force_endpoint(greeter_.has_value());
                    if (greeter_) {
                        greeter.set_endpoint_id(greeter_.value());
                    }
                    auto result = SharedView(pool_, greeter.ByteSizeLong());
                    greeter.SerializeToArray(result.data(), result.size());
                    return result;
                });
            bus_.start([=](auto d, auto v) { this->handle(d, v); });
            loop_.start();
            exc_.schedule([=] { timed_flush_batch(); }, batch_opts_.max_delay);
        }

        void handle(TcpBus::ConnHandle handle, SharedView view) {
            if (endpoint_manager_.transient(handle.endpoint)) {
                bus::detail::Greeter greeter;
                greeter.ParseFromArray(view.data(), view.size());
                if (greeter.force_endpoint()) {
                    bus_.rebind(handle.conn_id, greeter.endpoint_id());
                } else {
                    int endpoint = endpoint_manager_.resolve(handle.socket, greeter.port());
                    if (!endpoint_manager_.transient(endpoint)) {
                        bus_.rebind(handle.conn_id, endpoint);
                    } else {
                        bus_.close(handle.conn_id);
                    }
                }
            } else {
                bus::detail::MessageBatch batch;
                batch.ParseFromArray(view.data(), view.size());
                for (auto& header : *batch.mutable_item()) {
                    if (header.type() == detail::Message::REQUEST) {
                        if (handlers_.size() <= header.method() || !handlers_[header.method()]) {
                            throw BusError("invalid handler number");
                        } else {
                            handlers_[header.method()](handle.endpoint, header.seq_id(), std::move(*header.mutable_data()));
                        }
                    }
                    if (header.type() == detail::Message::RESPONSE) {
                        auto reqs = sent_requests_.get();
                        auto it = reqs->find(header.seq_id());
                        if (it != reqs->end()) {
                            it->second.set_value(ErrorT<std::string>::value(std::move(*header.mutable_data())));
                            reqs->erase(it);
                        }
                    }
                }
            }
        }

        void flush_batch(int endpoint, detail::MessageBatch batch) {
            if (!batch.item_size()) {
                return;
            }
            auto buffer = SharedView(pool_, batch.ByteSizeLong());
            batch.SerializeToArray(buffer.data(), buffer.size());
            bus_.send(endpoint, std::move(buffer));
        }

        void timed_flush_batch() {
            exc_.schedule([=] { timed_flush_batch(); }, batch_opts_.max_delay);
            std::unordered_map<int, detail::MessageBatch> accumulated;
            accumulated_.get()->swap(accumulated);
            for (auto& [endpoint, batch] : accumulated) {
                flush_batch(endpoint, std::move(batch));
            }
        }

        void send_item(int endpoint, detail::Message item) {
            std::optional<detail::MessageBatch> to_flush;
            {
                auto accumulated = accumulated_.get();
                auto& batch = (*accumulated)[endpoint];
                *batch.add_item() = std::move(item);

                if (batch.item_size() >= batch_opts_.max_batch) {
                    to_flush.emplace();
                    to_flush.value() = std::move(batch);
                    batch.clear_item();
                }
            }
            if (to_flush.has_value()) {
                flush_batch(endpoint, std::move(to_flush.value()));
            }
        }

    public:
        std::optional<uint64_t> greeter_;

        EndpointManager& endpoint_manager_;
        BufferPool pool_;
        TcpBus bus_;
        std::vector<std::function<void(int, uint32_t, std::string)>> handlers_;
        internal::DelayedExecutor exc_;

        internal::ExclusiveWrapper<std::unordered_map<int, detail::MessageBatch>> accumulated_;

        internal::ExclusiveWrapper<std::unordered_map<uint64_t, Promise<ErrorT<std::string>>>> sent_requests_;
        std::atomic<uint64_t> seq_id_ = 0;

        BatchOptions batch_opts_;
        internal::PeriodicExecutor loop_;
    };

    Future<ErrorT<std::string>> ProtoBus::send_raw(std::string serialized, int endpoint, uint64_t method, std::chrono::duration<double> timeout) {
        detail::Message header;
        uint64_t seq_id = impl_->seq_id_.fetch_add(1);
        header.set_seq_id(seq_id);
        header.set_type(detail::Message::REQUEST);
        header.set_data(std::move(serialized));
        header.set_method(method);
        impl_->send_item(endpoint, std::move(header));

        Promise<ErrorT<std::string>> promise;
        impl_->sent_requests_.get()->insert({ seq_id, promise });
        impl_->exc_.schedule([=] () mutable {
                auto requests = impl_->sent_requests_.get();
                if (requests->find(seq_id) != requests->end()) {
                    promise.set_value(ErrorT<std::string>::error("timeout exceeded"));
                }
            },
            timeout);
        return promise.future();
    }

    void ProtoBus::register_raw_handler(uint32_t method, std::function<Future<std::string>(int, std::string)> handler) {
        impl_->handlers_.resize(std::max<uint32_t>(impl_->handlers_.size(), method + 1));
        impl_->handlers_[method] =
            [handler=std::move(handler), this, method] (int endpoint, uint32_t seq_id, std::string str) {
                handler(endpoint, std::move(str)).subscribe([=](std::string& str) {
                    bus::detail::Message header;
                    header.set_type(detail::Message::RESPONSE);
                    header.set_data(str);
                    header.set_seq_id(seq_id);
                    header.set_method(method);
                    impl_->send_item(endpoint, std::move(header));
                });
            };
    }

    ProtoBus::ProtoBus(Options opts, EndpointManager& manager)
        : impl_(new Impl(opts, manager))
    {
    }

    ProtoBus::~ProtoBus() {
        impl_->bus_.to_break();
    }

}
