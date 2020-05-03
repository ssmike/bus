#pragma once

#include "lock.h"
#include "future.h"

#include <condition_variable>
#include <thread>
#include <chrono>
#include <map>

namespace bus::internal {

class DelayedExecutor {
public:
    DelayedExecutor()
        : thread_(std::bind(&DelayedExecutor::execute, this))
    {
    }

    DelayedExecutor(const DelayedExecutor&) = delete;
    DelayedExecutor(DelayedExecutor&&) = delete;

    template<typename Duration>
    void schedule(std::function<void()> what, Duration when) {
        auto deadline = std::chrono::time_point_cast<std::chrono::system_clock::time_point::duration>(std::chrono::system_clock::now() + when);
        auto actions = actions_.get();
        actions->insert({ deadline, std::move(what) });
        ready_.notify();
    }

    void schedule_point(std::function<void()> what, std::chrono::time_point<std::chrono::system_clock> when) {
        auto actions = actions_.get();
        actions->insert({ when, std::move(what) });
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

            if (auto actions = actions_.get(); !actions->empty()) {
                ready_.reset();
                wait_until = actions->begin()->first;
            }
            if (!shot_down_.load()) {
                if (wait_until) {
                    if (!ready_.wait_until(*wait_until)) {
                        std::function<void()> to_execute;
                        if (auto actions = actions_.get(); !actions->empty()) {
                            to_execute = std::move(actions->begin()->second);
                            actions->erase(actions->begin());
                        }
                        if (to_execute) {
                            to_execute();
                        }
                    }
                } else {
                    ready_.wait();
                }
            }
        }
        shot_down_event_.notify();
    }

private:
    ExclusiveWrapper<std::multimap<std::chrono::system_clock::time_point, std::function<void()>>> actions_;
    Event ready_;
    std::atomic_bool shot_down_ = false;
    Event shot_down_event_;
    std::thread thread_;
};

class PeriodicExecutor : DelayedExecutor {
public:
    template<typename Duration>
    PeriodicExecutor(std::function<void()> f, Duration period)
        : f_(std::move(f))
        , period_(std::chrono::duration_cast<decltype(period_)>(period))
    {
    }

    void delayed_start() {
        schedule(std::bind(&PeriodicExecutor::execute, this), period_);
    }

    void start() {
        schedule(std::bind(&PeriodicExecutor::execute, this), std::chrono::seconds::zero());
    }

    void trigger() {
        schedule(std::bind(&PeriodicExecutor::execute_once, this), std::chrono::seconds::zero());
    }

private:
    void execute() {
        schedule(std::bind(&PeriodicExecutor::execute, this), period_);
        f_();
    }

    void execute_once() {
        f_();
    }

private:
    std::function<void()> f_;
    std::chrono::system_clock::duration period_;
};

}
