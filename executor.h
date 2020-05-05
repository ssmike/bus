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
        schedule_point(std::move(what), deadline);
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

            std::vector<std::function<void()>> to_execute;
            auto now = std::chrono::system_clock::now();
            {
                ready_.reset();
                auto actions = actions_.get();
                while (!actions->empty() && actions->begin()->first <= now) {
                    to_execute.push_back(std::move(actions->begin()->second));
                    actions->erase(actions->begin());
                }
                if (!actions->empty()) {
                    wait_until = actions->begin()->first;
                }
            }
            for (auto& action : to_execute) {
                action();
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
