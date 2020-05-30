#pragma once

#include "future.h"
#include "executor.h"
#include "action_map.h"


#include <chrono>
#include <condition_variable>
#include <thread>


namespace bus::internal {

class DelayedExecutor : public Executor {
public:
    DelayedExecutor()
        : thread_(std::bind(&DelayedExecutor::execute, this))
    {
    }

    DelayedExecutor(const DelayedExecutor&) = delete;
    DelayedExecutor(DelayedExecutor&&) = delete;

    void schedule_point(std::function<void()> what, std::chrono::time_point<std::chrono::system_clock> when) override {
        actions_.get()->insert(when, std::move(what));
        ready_.notify();
    }

    ~DelayedExecutor() {
        shot_down_.store(true);
        ready_.notify();
        shot_down_event_.wait();

        thread_.join();
    }

private:
    void execute() {
        while (!shot_down_.load()) {
            std::optional<std::chrono::system_clock::time_point> wait_until;

            auto now = std::chrono::system_clock::now();
            ready_.reset();
            while (true) {
                std::function<void()> to_execute;
                {
                    auto actions = actions_.get();
                    if (auto next = actions->next_time_point()) {
                        if (*next <= now) {
                            to_execute = actions->pick_action();
                        } else {
                            wait_until = next;
                        }
                    }
                }
                if (to_execute) {
                    to_execute();
                } else {
                    break;
                }
            }

            if (shot_down_.load()) {
                break;
            }
            if (wait_until) {
                ready_.wait_until(*wait_until);
            } else {
                ready_.wait();
            }
        }
        shot_down_event_.notify();
    }

private:
    internal::ExclusiveWrapper<ActionMap, std::recursive_mutex> actions_;
    Event ready_;
    std::atomic_bool shot_down_ = false;
    Event shot_down_event_;
    std::thread thread_;
};

class PeriodicExecutor {
public:
    template<typename Duration>
    PeriodicExecutor(std::function<void()> f, Duration period)
        : f_(std::move(f))
        , period_(std::chrono::duration_cast<decltype(period_)>(period))
    {
        executor_holder_ = std::make_unique<DelayedExecutor>();
        backend_ = executor_holder_.get();
    }

    template<typename Duration>
    PeriodicExecutor(std::function<void()> f, Duration period, Executor& executor)
        : f_(std::move(f))
        , period_(std::chrono::duration_cast<decltype(period_)>(period))
    {
        backend_ = &executor;
    }

    void delayed_start() {
        backend_->schedule(std::bind(&PeriodicExecutor::execute, this), period_);
    }

    void start() {
        backend_->schedule(std::bind(&PeriodicExecutor::execute, this), std::chrono::seconds::zero());
    }

    void trigger() {
        backend_->schedule(std::bind(&PeriodicExecutor::execute_once, this), std::chrono::seconds::zero());
    }

private:
    void execute() {
        backend_->schedule(std::bind(&PeriodicExecutor::execute, this), period_);
        f_();
    }

    void execute_once() {
        f_();
    }

private:
    std::function<void()> f_;
    std::chrono::system_clock::duration period_;

    Executor* backend_;
    std::unique_ptr<DelayedExecutor> executor_holder_;
};

}
