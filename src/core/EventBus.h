#pragma once

#include <functional>
#include <mutex>
#include <unordered_map>

#include "core/DeviceModel.h"

// A minimal, threadsafe event bus for DeviceEvent.
// API is present; behavior is intentionally simple.
class EventBus {
public:
    using Callback = std::function<void(const DeviceEvent&)>;

    // Subscribe and receive a numeric token that can be used to unsubscribe.
    int subscribe(Callback cb);

    // Unsubscribe by token; no-op if not found.
    void unsubscribe(int token);

    // Publish an event to all current subscribers.
    void publish(const DeviceEvent& evt);

private:
    std::mutex mtx_;
    int nextToken_{1};
    std::unordered_map<int, Callback> subscribers_;
};

