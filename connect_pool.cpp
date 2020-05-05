#include "connect_pool.h"

#include "error.h"

#include <netinet/tcp.h>
#include <unistd.h>

#include <unordered_map>
#include <list>


namespace bus {

SocketHolder::~SocketHolder() {
    if (sock_ < 0) {
        return;
    }
    struct linger sl;
    sl.l_onoff = 1;
    sl.l_linger = 0;
    setsockopt(sock_, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(sock_);
}

class ConnectPool::Impl {
public:
    struct PoolItem : public ConnData {
        std::list<uint64_t>::iterator usage_list_pos_;
        std::list<uint64_t>::iterator by_endpoint_pos_;
        bool available_ = false;
    };

public:
    std::shared_ptr<PoolItem> select(uint64_t id) {
        auto it = by_id_.find(id);
        if (it == by_id_.end()) {
            return nullptr;
        } else {
            return it->second;
        }
    }

public:
    std::unordered_map<uint64_t, std::shared_ptr<PoolItem>> by_id_;
    std::unordered_map<int, std::list<uint64_t>> by_endpoint_;
    std::list<uint64_t> by_usage_;
};

ConnectPool::ConnectPool()
    : impl_(new Impl())
{
}

size_t ConnectPool::make_id() {
    return id_.fetch_add(1, std::memory_order_seq_cst);
}

std::shared_ptr<ConnData> ConnectPool::add(SocketHolder holder, uint64_t id, int endpoint) {
    auto impl = impl_.get();
    if (impl->by_id_.find(id) != impl->by_id_.end()) {
        throw BusError("duplicate id");
    }

    auto usage_iterator = impl->by_usage_.insert(impl->by_usage_.begin(), id);
    auto hint_iterator =
        impl->by_endpoint_[endpoint].insert(impl->by_endpoint_[endpoint].end(), id);

    auto& data = impl->by_id_[id] = std::make_shared<Impl::PoolItem>();
    data->id = id;
    data->socket = std::move(holder);
    data->endpoint = endpoint;

    data->usage_list_pos_ = usage_iterator;
    data->by_endpoint_pos_ = hint_iterator;

    size_.fetch_add(1, std::memory_order_seq_cst);

    return data;
}

std::shared_ptr<ConnData> ConnectPool::select(uint64_t id) {
    auto impl = impl_.get();
    auto data = impl->select(id);
    impl->by_usage_.erase(data->usage_list_pos_);
    data->usage_list_pos_ = impl->by_usage_.insert(impl->by_usage_.begin(), id);
    return data;
}

void ConnectPool::rebind(uint64_t id, int endpoint) {
    auto impl = impl_.get();
    auto data = impl->select(id);
    impl->by_endpoint_[data->endpoint].erase(data->by_endpoint_pos_);
    data->endpoint = endpoint;
    if (data->available_) {
        data->by_endpoint_pos_ = impl->by_endpoint_[endpoint].insert(impl->by_endpoint_[endpoint].begin(), id);
    } else {
        data->by_endpoint_pos_ = impl->by_endpoint_[endpoint].insert(impl->by_endpoint_[endpoint].end(), id);
    }

}

std::shared_ptr<ConnData> ConnectPool::take_available(int endpoint) {
    auto impl = impl_.get();
    auto it = impl->by_endpoint_.find(endpoint);
    if (it == impl->by_endpoint_.end()) {
        return nullptr;
    }

    while (true) {
        if (it->second.empty()) {
            impl->by_endpoint_.erase(it);
            return nullptr;
        }

        auto data = impl->select(*it->second.begin());
        if (!data) {
            it->second.erase(it->second.begin());
        } else if (!data->available_) {
            return nullptr;
        } else {
            return data;
        }
    }
}

void ConnectPool::set_available(uint64_t id) {
    auto impl = impl_.get();
    if (auto data = impl->select(id)) {
        data->available_ = true;
        auto& d_list = impl->by_endpoint_[data->endpoint];
        auto it = d_list.insert(d_list.begin(), id);
        d_list.erase(data->by_endpoint_pos_);
        data->by_endpoint_pos_ = it;
    }
}

void ConnectPool::set_unavailable(uint64_t id) {
    auto impl = impl_.get();
    if (auto data = impl->select(id)) {
        data->available_ = true;
        auto& d_list = impl->by_endpoint_[data->endpoint];
        auto it = d_list.insert(d_list.end(), id);
        d_list.erase(data->by_endpoint_pos_);
        data->by_endpoint_pos_ = it;
    }
}

size_t ConnectPool::count_connections(int endpoint) {
    auto impl = impl_.get();
    return impl->by_endpoint_[endpoint].size();
}

size_t ConnectPool::count_connections() {
    return size_.load(std::memory_order_seq_cst);
}

void ConnectPool::close(uint64_t id) {
    auto impl = impl_.get();
    auto it = impl->by_id_.find(id);
    if (it != impl->by_id_.end()) {
        impl->by_endpoint_[it->second->endpoint].erase(it->second->by_endpoint_pos_);
        impl->by_usage_.erase(it->second->usage_list_pos_);
        if (impl->by_endpoint_[it->second->endpoint].empty()) {
            impl->by_endpoint_.erase(it->second->endpoint);
        }
        impl->by_id_.erase(it);
    }
}

void ConnectPool::close_old_conns(size_t cnt) {
    auto impl = impl_.get();
    for (size_t i = 0; i < 2 && !impl->by_usage_.empty(); ++i) {
        auto it = --(impl->by_usage_.end());
        uint64_t id = *it;
        close(id);

        if (!impl->by_usage_.empty()) {
            it =  --(impl->by_usage_.end());
            if (id == *it) {
                impl->by_usage_.erase(it);
            }
        }
    }
}

ConnectPool::~ConnectPool() = default;

}
