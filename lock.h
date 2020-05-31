#pragma once

#include <atomic>
#include <mutex>
#include <memory>
#include <optional>

namespace bus::internal {

class SpinLock {
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;

    void lock() {
        while (!try_lock()) {
            ;
        }
    }

    bool try_lock() {
        return !acquired.exchange(true, std::memory_order_acquire);
    }

    void unlock() {
        acquired.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool acquired = false;
};

template<typename T, typename Lock>
class ExclusiveGuard {
public:
    ExclusiveGuard(T& value, std::unique_lock<Lock> lock)
        : value_(value)
        , lock_(std::move(lock))
    {
    }

    T& operator * () {
        return value_;
    }

    const T& operator * () const {
        return value_;
    }

    T* operator -> () {
        return &value_;
    }

    const T* operator -> () const {
        return &value_;
    }

    operator bool () const {
        return lock_.owns_lock();
    }

    void unlock() {
        lock_.unlock();
    }

private:
    T& value_;
    std::unique_lock<Lock> lock_;
};

template<typename T, typename Lock>
class ExclusiveGuard<std::unique_ptr<T>, Lock> {
public:
    ExclusiveGuard(std::unique_ptr<T>& value, std::unique_lock<Lock> lock)
        : value_(*value)
        , lock_(std::move(lock))
    {
    }

    T& operator * () {
        return value_;
    }

    const T& operator * () const {
        return value_;
    }

    T* operator -> () {
        return &value_;
    }

    const T* operator -> () const {
        return &value_;
    }

private:
    T& value_;
    std::unique_lock<Lock> lock_;
};

template<typename T, typename Lock=std::mutex>
class ExclusiveWrapper {
public:
    template<typename... Args>
    ExclusiveWrapper(Args&&... args)
        : value_(std::forward<Args>(args)...)
    {
    }

    ExclusiveGuard<T, Lock> get() {
        return ExclusiveGuard(value_, std::unique_lock(mutex_));
    }

    ExclusiveGuard<T, Lock> try_get() {
        return ExclusiveGuard(value_, std::unique_lock(mutex_, std::try_to_lock));
    }

private:
    T value_;
    Lock mutex_;
};

}
