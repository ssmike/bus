#pragma once

#include "lock.h"

#include <map>
#include <functional>
#include <array>

namespace bus::internal {

class ActionMap {
private:
    static constexpr size_t kSmallThreshold = 20;

public:
    using time_point = std::chrono::system_clock::time_point;

    ActionMap() = default;

    void insert(time_point pt, std::function<void()> action) {
        min_pt_ = std::min(min_pt_.value_or(time_point::max()), pt);
        if (map_.empty()) {
            for (size_t i = 0; i < small_map_.size(); ++i) {
                if (!small_map_[i].second) {
                    small_map_[i] = { pt, std::move(action) };
                    small_map_empty_ = false;
                    return;
                }
            }
        }
        map_.insert( { pt, std::move(action) } );
    }

    std::optional<time_point> next_time_point() {
        return min_pt_;
    }

    std::function<void()> pick_action() {
        if (!small_map_empty_) {
            std::pair<time_point, size_t> small_min = { time_point::max(), small_map_.size() };
            for (size_t i = 0; i < small_map_.size(); ++i) {
                if (small_map_[i].second) {
                    small_min = std::min(small_min, { small_map_[i].first, i });
                }
            }
            time_point big_min = time_point::max();
            if (!map_.empty()) {
                big_min = map_.begin()->first;
            }
            if (small_min.second < small_map_.size() && big_min > small_min.first) {
                std::function<void()> result = std::move(small_map_[small_min.second].second);
                small_map_[small_min.second].second = {};
                recalc_min();
                return result;
            }
        }
        if (map_.empty()) {
            return {};
        } else {
            auto it = map_.begin();
            std::function<void()> result = std::move(it->second);
            map_.erase(it);
            recalc_min();
            return result;
        }
    }

private:
    void recalc_min() {
        min_pt_.reset();
        if (!map_.empty()) {
            min_pt_ = map_.begin()->first;
        }
        if (!small_map_empty_) {
            small_map_empty_ = true;
            for (size_t i = 0; i < small_map_.size(); ++i) {
                if (small_map_[i].second) {
                    small_map_empty_ = false;
                    min_pt_ = std::min(min_pt_.value_or(time_point::max()), small_map_[i].first);
                }
            }
        }
    }

private:
    std::multimap<time_point, std::function<void()>> map_;
    std::array<std::pair<time_point, std::function<void()>>, kSmallThreshold> small_map_;
    bool small_map_empty_ = true;

    std::optional<time_point> min_pt_;
};

}
