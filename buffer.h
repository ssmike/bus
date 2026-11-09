#pragma once

#include "fwd.h"

#include "lock.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <memory>
#include <stack>
#include <string_view>
#include <utility>
#include <vector>
#include <stdexcept>

namespace bus {


// the model is the following:
// [head] is available for lock-free allocations. we always hold extra ref for head to avoid putting head to free_
// free_ is recyclable pages list
// buffers_ is a global pages list (free or not free)
class BufferPool {
public:
    static constexpr size_t kInvalidBuffer = std::numeric_limits<size_t>::max();

public:
    struct Buffer {
        std::vector<char> data;
        std::atomic<uint64_t> offset = 0;
        std::atomic<int64_t> usage_counter = 0;
        std::atomic<uint64_t> term = 0;

        size_t number;
    };

    struct DataPtr {
        Buffer* buffer = nullptr;
        uint64_t offset = 0;

        operator bool () {
            return buffer;
        }
    };

public:
    BufferPool(uint32_t size)
        : size_(size)
    {
    }

    DataPtr take(size_t size) {
        if (size > size_) {
            throw std::runtime_error("bad buffer alloc");
        }
        if (auto result = try_take(size)) {
            return result;
        } else {
            auto state = state_.get();
            // try reusing concurrent allocation if any
            // we wouldn't spin here because head/term are consistently updated under the same lock
            // or retire buffer because the head ref is also moved under this lock
            if (auto result = try_take(size)) {
                return result;
            }
            Buffer* new_buffer;
            if (state->free_.empty()) {
                auto buffer = std::make_unique<Buffer>();
                buffer->data.reserve(size_);
                buffer->data.resize(size_);
                buffer->number = state->buffers_.size();
                new_buffer = buffer.get();

                state->buffers_.push_back(std::move(buffer));
            } else {
                new_buffer = state->buffers_[state->free_.top()].get();
                state->free_.pop();
            }
            new_buffer->term.store(term_.fetch_add(1) + 1);
            new_buffer->offset.store(0);
            new_buffer->usage_counter.fetch_add(2); // new head ref + take ref
            result = try_take(size, new_buffer);

            if (auto head = head_.load()) {
                if (head->usage_counter.fetch_sub(1) == 1 && head->term.exchange(0) != 0) {
                    state->free_.push(head->number);
                }
            }
            head_.store(new_buffer);

            assert(result);
            return result;
        }
    }

    void put(DataPtr ptr) {
        if (ptr) {
            unref(ptr.buffer);
        }
    }

private:
    void unref(Buffer* buffer) {
        if (buffer->usage_counter.fetch_sub(1) == 1) {
            // is needed to avoid race between failed try-take-spins and reusing in take()
            auto state = state_.get();
            if (buffer->usage_counter.load() == 0 && buffer->term.exchange(0)) {
                state->free_.push(buffer->number);
            }
        }
    }

    DataPtr try_take(size_t size) {
        while (true) {
            auto term = term_.load();
            auto head = head_.load();
            if (!head) {
                return {};
            }
            head->usage_counter.fetch_add(1);
            if (head->term.load() == term) {
                return try_take(size, head);
            } else {
                unref(head);
            }
        }
    }

    DataPtr try_take(size_t size, Buffer* buffer) {
        if (!buffer) {
            return {};
        }
        DataPtr result {
            .buffer = buffer,
            .offset = buffer->offset.fetch_add(size)
        };
        if (result.offset + size > buffer->data.size()) {
            unref(buffer);
            return {};
        } else {
            return result;
        }
    }

private:
    struct State {
        std::vector<std::unique_ptr<Buffer>> buffers_;
        std::stack<size_t> free_;
    };

    internal::ExclusiveWrapper<State> state_;
    const uint64_t size_;
    std::atomic<Buffer*> head_ = nullptr; // head implies extra ref
    std::atomic<uint64_t> term_ = 0;
};

class SharedView {
public:
    SharedView() = default;

    SharedView(BufferPool& pool, size_t size)
        : pool_(&pool)
        , ptr_(pool.take(size))
        , size_(size)
    {
        data_ = ptr_.buffer->data.data() + ptr_.offset;
    }

    SharedView(const SharedView& oth)
        : SharedView()
    {
        mem_copy(oth);
        ref();
    }

    SharedView& operator = (const SharedView& oth) {
        bool same_buffer = this->ptr_.buffer == oth.ptr_.buffer;
        if (!same_buffer) {
            unref();
        }
        mem_copy(oth);
        if (!same_buffer) {
            ref();
        }
        return *this;
    }

    SharedView& operator = (SharedView&& oth) {
        unref();
        mem_reset();
        mem_swap(oth);
        return *this;
    }

    SharedView(SharedView&& oth) {
        mem_swap(oth);
    }

    ~SharedView() {
        unref();
    }

    bool initialized() {
        return pool_;
    }

    SharedView slice(size_t start, size_t size) const {
        SharedView result = *this;
        result.data_ = data_ + start;
        result.size_ = size;
        return result;
    }

    SharedView skip(size_t start) const {
        return slice(start, size() - start);
    }

    SharedView resize(size_t size) {
        return slice(0, size);
    }

    char* data() const {
        return data_;
    }

    size_t size() const {
        return size_;
    }

    std::string_view view() const {
        return { data_, size_ };
    }

private:
    void mem_reset() {
        pool_ = nullptr;
        ptr_ = {};
        size_ = 0;
    }

    void unref() {
        if (pool_) {
            pool_->put(ptr_);
        }
    }

    void ref() {
        if (pool_) {
            ptr_.buffer->usage_counter.fetch_add(1);
        }
    }

    void mem_copy(const SharedView& oth) {
        std::tie(pool_, ptr_, data_, size_) = std::tie(oth.pool_, oth.ptr_, oth.data_, oth.size_);
    }

    void mem_swap(SharedView& view) {
        std::swap(pool_, view.pool_);
        std::swap(ptr_, view.ptr_);
        std::swap(data_, view.data_);
        std::swap(size_, view.size_);
    }

private:
    BufferPool* pool_ = nullptr;
    BufferPool::DataPtr ptr_;
    char* data_ = nullptr;
    size_t size_ = 0;
};

}
