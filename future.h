#pragma once

#include <condition_variable>
#include <mutex>
#include <atomic>
#include <optional>
#include <vector>
#include <functional>
#include <chrono>

namespace bus {
    namespace internal {
    class Event {
    public:
        Event() {
        }

        void reset() {
            std::unique_lock<std::mutex> lock(mutex_);
            event_set_.store(false);
        }

        void notify() {
            if (!event_set_.exchange(true)) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.notify_all();
            }
        }

        bool set() const {
            return event_set_.load();
        }

        void wait() {
            if (!set()) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return set(); });
            }
        }

        bool wait_until(std::chrono::system_clock::time_point pt) {
            if (!set()) {
                std::unique_lock<std::mutex> lock(mutex_);
                return cv_.wait_until(lock, pt, [&] { return set(); });
            }
            return true;
        }

    private:
        std::atomic<bool> event_set_ = false;

        std::mutex mutex_;
        std::condition_variable cv_;
    };

    template<typename T>
    class FutureState {
    public:
        template<typename... Args>
        void set_value(bool check_double_set, Args&&... args) {
            std::vector<std::function<void(T&)>> to_call;
            if (!evt_.set()) {
                std::unique_lock lock(mutex_);
                if (value_) {
                    if (check_double_set) {
                        throw std::logic_error("double FutureState::set_value");
                    } else {
                        return;
                    }
                }
                value_.emplace(std::forward<Args>(args)...);
                to_call = std::move(callbacks_);
            } else {
                if (check_double_set) {
                    throw std::logic_error("double FutureState::set_value");
                } else {
                    return;
                }
            }
            evt_.notify();
            for (auto& cb : to_call) {
                cb(get());
            }
        }

        template<typename Func>
        void apply(Func f) {
            if (evt_.set()) {
                f(get());
            } else {
                std::unique_lock<std::mutex> lock(mutex_);
                if (value_) {
                    lock.unlock();
                    f(get());
                } else {
                    callbacks_.push_back(std::move(f));
                }
            }
        }

        bool has_value() {
            return evt_.set() && value_.has_value();
        }

        T& get() {
            if (!evt_.set()) {
                throw std::logic_error("value not set");
            }
            return *value_;
        }

        T& wait() {
            evt_.wait();
            return get();
        }

    private:
        Event evt_;
        std::mutex mutex_;
        std::optional<T> value_;
        std::vector<std::function<void(T&)>> callbacks_;
    };

    } // namespace internal

template<typename T>
class ErrorT {
private:
    class FailureTag {};
    ErrorT(FailureTag, std::string what)
        : message_(std::move(what))
    {
    }

public:
    ErrorT() = default;
    ErrorT(const ErrorT<T>&) = default;
    ErrorT(ErrorT<T>&&) = default;

    template<typename... Args>
    void emplace(Args&&... args) {
        value_.emplace(std::forward<Args>(args)...);
    }

    template<typename... Args>
    static ErrorT<T> value(Args&&... args) {
        ErrorT<T> result;
        result.emplace(std::forward<Args>(args)...);
        return result;
    }

    static ErrorT<T> error(std::string msg) {
        return ErrorT(ErrorT<T>::FailureTag(), std::move(msg));
    }

    operator bool () const {
        return value_.has_value();
    }

    const char* what() {
        return message_.data();
    }

    T& unwrap() {
        if (!*this) {
            throw std::runtime_error(message_);
        } else {
            return *value_;
        }
    }

private:
    std::optional<T> value_;
    std::string message_;
};

template<typename T>
class Promise;

template<typename T>
class Future {
public:
    using Type = T;

public:
    Future(std::shared_ptr<internal::FutureState<T>> state) : state_(std::move(state)) {}
    Future(const Future<T>&) = default;
    Future(Future<T>&&) = default;

    Future<T>& operator = (const Future<T>&) = default;
    Future() = default;

    T& get() {
        return state_->get();
    }

    T& wait() {
        return state_->wait();
    }

    template<typename Func>
    void subscribe(Func f) {
        state_->apply(std::move(f));
    }

    template<typename Func>
    Future<std::invoke_result_t<Func, T&>> map(Func f) {
        Promise<std::invoke_result_t<Func, T&>> result;
        state_->apply([f=std::move(f), result] (T& t) mutable {
                result.set_value_once(f(t));
            });
        return result.future();
    }

    template<typename Func>
    Future<ErrorT<std::invoke_result_t<Func, T&>>> apply(Func f) {
        using return_t = std::invoke_result_t<Func, T&>;
        Promise<ErrorT<std::invoke_result_t<Func, T&>>> result;
        state_->apply([f=std::move(f), result] (T& t) mutable {
                try {
                    result.set_value_once(f(t));
                } catch(const std::exception& e) {
                    result.set_value_once(ErrorT<return_t>::error(e.what()));
                }
            });
        return result.future();
    }

    template<typename Func>
    Future<typename std::invoke_result_t<Func, T&>::Type> chain(Func f) {
        using return_t = std::invoke_result_t<Func, T&>;
        Promise<typename std::invoke_result_t<Func, T&>::Type> result;
        state_->apply([f=std::move(f), result] (T& t) mutable {
                f(t).subscribe([=] (auto& v) mutable {
                        result.set_value_once(v);
                    });
            });
        return result.future();
    }

private:
    std::shared_ptr<internal::FutureState<T>> state_;
};

template<typename T>
Future<T> make_future(T&& t) {
    Promise<T> res;
    res.set_value_once(std::forward<T>(t));
    return res.future();
}

template<typename T>
class Promise {
public:
    Promise() : state_(new internal::FutureState<T>()) {}

    Promise(const Promise<T>&) = default;
    Promise(Promise<T>&&) = default;

    Promise<T>& operator = (Promise<T>&) = default;
    Promise<T>& operator = (Promise<T>&&) = default;

    void swap(Promise<T>& other) {
        state_.swap(other.state_);
    }

    template<typename... Args>
    void set_value(Args&&... args) {
        state_->set_value(/* check double-set */ false, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void set_value_once(Args&&... args) {
        state_->set_value(/* check double-set */ true, std::forward<Args>(args)...);
    }

    bool has_value() {
        return state_->has_value();
    }

    Future<T> future() {
        return state_;
    }

private:
    std::shared_ptr<internal::FutureState<T>> state_;
};

} // namespace bus
