#pragma once

#include "lock.h"

#include <chrono>
#include <functional>

namespace bus {

class Executor {
public:
    template<typename Duration>
    void schedule(std::function<void()> what, Duration when) {
        auto deadline = std::chrono::time_point_cast<std::chrono::system_clock::time_point::duration>(std::chrono::system_clock::now() + when);
        schedule_point(std::move(what), deadline);
    }

    virtual void schedule_point(std::function<void()> what, std::chrono::time_point<std::chrono::system_clock> when) = 0;

    virtual ~Executor() = default;
};

}
