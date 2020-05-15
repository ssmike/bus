#include "bus.h"
#include "util.h"
#include "lock.h"

#include "error.h"

#include <sys/uio.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>

namespace bus {

class TcpBus::Impl {
public:
    Impl(bus::TcpBus::Options opts, BufferPool& buffer_pool, EndpointManager& endpoint_manager)
        : fixed_pool_size_(opts.fixed_pool_size)
        , port_(opts.port)
        , listener_backlog_(opts.listener_backlog)
        , buffer_pool_(buffer_pool)
        , endpoint_manager_(endpoint_manager)
        , max_message_size_(opts.max_message_size)
        , max_pending_messages_(opts.max_pending_messages)
    {
        epollfd_ = epoll_create1(EPOLL_CLOEXEC);
        CHECK_ERRNO(epollfd_ >= 0);
        breakfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        CHECK_ERRNO(breakfd_ >= 0);

        {
            epoll_event evt;
            evt.events = EPOLLIN;
            evt.data.u64 = break_id_ = pool_.make_id();
            CHECK_ERRNO(epoll_ctl(epollfd_, EPOLL_CTL_ADD, breakfd_, &evt) == 0);
        }

        listen_id_ = pool_.make_id();
    }

    void start(std::function<void(ConnHandle, SharedView)> handler) {
        handler_ = std::move(handler);

        listensock_ = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             IPPROTO_TCP);
        CHECK_ERRNO(listensock_ >= 0);

        {
            int optval = 1;
            setsockopt(listensock_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
        }

        sockaddr_in6 addr;
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port_);
        CHECK_ERRNO(bind(listensock_, reinterpret_cast<struct sockaddr *>(&addr),
                         sizeof(addr)) == 0);

        CHECK_ERRNO(listen(listensock_, listener_backlog_) == 0);

        {
            epoll_event evt;
            evt.events = EPOLLIN;
            evt.data.u64 = listen_id_;
            CHECK_ERRNO(epoll_ctl(epollfd_, EPOLL_CTL_ADD, listensock_, &evt) == 0);
        }
    }

    ~Impl() {
        ::close(listensock_);
        ::close(epollfd_);
    }

    void accept_conns() {
        for (size_t i = 0; i < 2; ++i) {
            EndpointManager::IncomingConnection conn = endpoint_manager_.accept(listensock_);
            if (conn.sock_.get() >= 0) {
                uint64_t id = pool_.make_id();
                auto data = pool_.add(conn.sock_.release(), id, conn.endpoint_);

                epoll_add(data->socket.get(), id);
                pool_.set_available(id);
            } else if (conn.errno_ == EAGAIN) {
                return;
            } else  if (conn.errno_ == EMFILE || conn.errno_ == ENFILE || conn.errno_ == ENOBUFS || conn.errno_ == ENOMEM) {
                pool_.close_old_conns(2);
            } else if (conn.errno_ != EINTR) {
                throw_errno();
            }
        }
    }

    void fix_pool_size(int endpoint) {
        if (endpoint_manager_.transient(endpoint)) {
            return;
        }
        size_t pool_size = pool_.count_connections(endpoint);
        if (pool_size < fixed_pool_size_) {
            for (; pool_size < fixed_pool_size_; ++pool_size) {
                SocketHolder sock = endpoint_manager_.socket(endpoint);
                uint64_t id = pool_.make_id();
                endpoint_manager_.async_connect(sock, endpoint);
                auto data = pool_.add(sock.release(), id, endpoint);
                if (greeter_) {
                    auto egress_data = data->egress_data.get();
                    egress_data->message = greeter_(endpoint);
                    egress_data->offset = 0;
                }
                epoll_add(data->socket.get(), id);
            }
        }
    }

    void handle_read(ConnData* data) {
        ssize_t message_size = std::numeric_limits<ssize_t>::lowest();
        while (true) {
            size_t expected = 0;
            bool has_header = false;
            char* data_ptr;
            if (data->ingress_offset < internal::header_len) {
                expected = internal::header_len - data->ingress_offset;
                data_ptr = data->ingress_header + data->ingress_offset;
            } else {
                has_header = true;
                size_t message_size = internal::read_header(data->ingress_header);
                if (!data->ingress_buf.initialized()) {
                    data->ingress_buf = SharedView(buffer_pool_, message_size);
                }
                expected = message_size + internal::header_len - data->ingress_offset;
                data_ptr = data->ingress_buf.data() + (data->ingress_offset - internal::header_len);
            }
            if (expected > max_message_size_) {
                throw BusError("too big message");
            }
            ssize_t res = read(data->socket.get(), data_ptr, expected);
            if (res >= 0) {
                data->ingress_offset += res;
                if (has_header && res == expected) {
                    handler_({.endpoint=data->endpoint, .socket=data->socket.get(), .conn_id=data->id}, SharedView(std::move(data->ingress_buf)));
                    data->ingress_buf = SharedView();
                    data->ingress_offset = 0;
                    continue;
                }
                // connection closed by remote peer
                if (res == 0) {
                    pool_.close(data->id);
                    return;
                }
                // os buffer exhausted
                if (res < expected) {
                    return;
                }
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            } else if (errno == EINTR) {
                continue;
            } else {
                pool_.close(data->id);
                return;
            }
        }
    }

    void handle_write(ConnData* data) {
        while (1) {
            auto egress_data = data->egress_data.get();
            if (!egress_data->message) {
                auto messages = pending_messages_.get();
                auto& queue = (*messages)[data->endpoint];
                if (queue.empty()) {
                    pool_.set_available(data->id);
                    return;
                }
                egress_data->message = queue.front();
                egress_data->offset = 0; (*messages)[data->endpoint].pop();
            }
            if (!try_write_message(data, egress_data)) {
                return;
            }
        }
    }

    void loop() {
        std::vector<epoll_event> event_buf;
        bool to_break = false;
        while (!to_break) {
            event_buf.resize(pool_.count_connections() + 1);
            int ready = epoll_wait(epollfd_, event_buf.data(), event_buf.size(), -1);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            CHECK_ERRNO(ready >= 0);
            for (size_t i = 0; i < ready; ++i) {
                uint64_t id = event_buf[i].data.u64;
                if (id == break_id_) {
                    uint64_t val;
                    CHECK_ERRNO(read(breakfd_, &val, sizeof(val)) == sizeof(val));
                    to_break = true;
                } else if (id == listen_id_) {
                    accept_conns();
                } else if (auto data = pool_.select(id)) {
                    int endpoint = data->endpoint;
                    if (event_buf[i].events & EPOLLERR) {
                        pool_.close(id);
                        fix_pool_size(endpoint);
                        continue;
                    }
                    if (event_buf[i].events & EPOLLIN) {
                        handle_read(data.get());
                    }
                    if (data && (event_buf[i].events & EPOLLOUT) != 0) {
                        pool_.set_unavailable(id);
                        handle_write(data.get());
                    }
                }
            }
        }
    }

    bool try_write_message(ConnData* data, internal::ExclusiveGuard<ConnData::EgressData>& egress_data) {
        if (!egress_data->message) {
            return true;
        }

        int fd = data->socket.get();

        char header[internal::header_len];
        internal::write_header(egress_data->message->size(), header);

        while (true) {
            iovec iov_holder[2];
            iov_holder[0] = {.iov_base = header, .iov_len = internal::header_len};
            iov_holder[1] = {.iov_base = (void*)egress_data->message->data(), .iov_len = egress_data->message->size()};

            iovec* iov = iov_holder;
            int iovcnt = 2;
            size_t offset = egress_data->offset;

            while (iovcnt > 0 && offset > iov[0].iov_len) {
                offset -= iov[0].iov_len;
                ++iov;
            }
            if (iovcnt == 0) {
                return 0;
            }
            if (offset > 0) {
                iov[0].iov_base = ((char*)iov[0].iov_base) + offset;
                iov[0].iov_len -= offset;
            }
            ssize_t res = writev(fd, iov, iovcnt);
            if (res >= 0) {
                egress_data->offset += res;
                if (egress_data->offset == internal::header_len + egress_data->message->size()) {
                    egress_data->message.reset();
                    pool_.set_available(data->id);
                }
                return true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            } else if (errno == EINTR) {
                continue;
            } else {
                pool_.close(data->id);
                return false;
            }
        }
    }

    void answer(uint64_t conn_id, SharedView message) {
        if (auto data = pool_.select(conn_id)) {
            auto egress_data = data->egress_data.get();
            if (egress_data->message) {
                throw BusError("answer in bound connection");
            }
            egress_data->message = std::move(message);
            egress_data->offset = 0;
            try_write_message(data.get(), egress_data);
        }
    }

    bool send(int endpoint, SharedView message) {
        fix_pool_size(endpoint);
        auto available_connection = pool_.take_available(endpoint);
        if (available_connection) {
            if (auto egress_data = available_connection->egress_data.try_get()) {
                egress_data->message = std::move(message);
                egress_data->offset = 0;
                try_write_message(available_connection.get(), egress_data);
                return true;
            }
        }
        {
            auto messages = pending_messages_.get();
            auto& queue = (*messages)[endpoint];
            if (!max_pending_messages_ || *max_pending_messages_ > queue.size()) {
                queue.push(std::move(message));
                return true;
            } else {
                return false;
            }
        }
    }

    uint64_t epoll_add(int fd, uint64_t id) {
        epoll_event evt;
        evt.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET;
        evt.data.u64 = id;
        CHECK_ERRNO(epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &evt) == 0);
        return evt.data.u64;
    }

public:
    std::function<void(ConnHandle, SharedView)> handler_;
    std::function<std::optional<SharedView>(int endpoint)> greeter_;

    int epollfd_;
    int listensock_;
    int breakfd_;

    size_t break_id_;
    size_t listen_id_;

    const int port_;
    const size_t listener_backlog_;

    ConnectPool pool_;
    const size_t fixed_pool_size_;

    internal::ExclusiveWrapper<std::unordered_map<int, std::queue<SharedView>>> pending_messages_;

    BufferPool& buffer_pool_;
    EndpointManager& endpoint_manager_;

    const size_t max_message_size_;
    const std::optional<size_t> max_pending_messages_;
};

TcpBus::TcpBus(Options opts, BufferPool& buffer_pool, EndpointManager& endpoint_manager)
    : impl_(new Impl(opts, buffer_pool, endpoint_manager))
{
}

void TcpBus::answer(uint64_t conn_id, SharedView buffer) {
    impl_->answer(conn_id, std::move(buffer));
}

bool TcpBus::send(int endpoint, SharedView buffer) {
    if (impl_->endpoint_manager_.transient(endpoint)) {
        throw BusError("bad endpoint");
    }
    return impl_->send(endpoint, std::move(buffer));
}

void TcpBus::clear_queue(int endpoint) {
    impl_->pending_messages_.get()->erase(endpoint);
}

void TcpBus::start(std::function<void(ConnHandle, SharedView)> handler) {
    impl_->start(std::move(handler));
}

void TcpBus::set_greeter(std::function<std::optional<SharedView>(int endpoint)> greeter) {
    impl_->greeter_ = std::move(greeter);
}

void TcpBus::rebind(uint64_t conn_id, int new_endpoint) {
    impl_->pool_.rebind(conn_id, new_endpoint);
}

void TcpBus::close(uint64_t conn_id) {
    impl_->pool_.close(conn_id);
}

void TcpBus::loop() {
    impl_->loop();
}

void TcpBus::to_break() {
    uint64_t val = 1;
    CHECK_ERRNO(write(impl_->breakfd_, &val, sizeof(val)) == sizeof(val));
}

TcpBus::~TcpBus() = default;

};
