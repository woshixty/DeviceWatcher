#include "core/EventBus.h"

int EventBus::subscribe(Callback cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int token = nextToken_++;
    subscribers_.emplace(token, std::move(cb));
    return token;
}

void EventBus::unsubscribe(int token) {
    std::lock_guard<std::mutex> lock(mtx_);
    subscribers_.erase(token);
}

void EventBus::publish(const DeviceEvent& evt) {
    // Copy callbacks under lock, then invoke without lock to avoid re-entrancy issues.
    std::unordered_map<int, Callback> subsCopy;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        subsCopy = subscribers_;
    }
    for (auto& kv : subsCopy) {
        if (kv.second) {
            kv.second(evt);
        }
    }
}

