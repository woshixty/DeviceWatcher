#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <optional>

#include "core/DeviceModel.h"

class DeviceManager {
public:
    using Snapshot = std::vector<DeviceInfo>;
    using Subscriber = std::function<void(const DeviceEvent&)>;

    DeviceManager();
    ~DeviceManager();

    // Return a copy of the current device list.
    Snapshot snapshot() const;
    // Return onlineSince timestamp if device is currently known online.
    std::optional<std::chrono::system_clock::time_point> onlineSince(const std::string& uid) const;

    // Subscribe to device events (thread-safe). Returns token index.
    int subscribe(Subscriber cb);
    void unsubscribe(int token);

    // The below helpers are stubs for future expansion.
    void addOrUpdateDevice(const DeviceInfo& info);
    void removeDevice(const std::string& uid);

    // Provider pushes an event into manager; manager updates its store and notifies subscribers.
    void onEvent(const DeviceEvent& evt);

private:
    void workerLoop();
    static void mergeInfo(DeviceInfo& dst, const DeviceInfo& src);

    mutable std::mutex mtx_;
    std::unordered_map<std::string, DeviceInfo> devices_; // uid -> info
    std::vector<Subscriber> subscribers_;                 // simple subscriber list
    std::unordered_map<std::string, std::chrono::system_clock::time_point> onlineSince_; // uid -> since

    // Event queue + worker
    std::queue<DeviceEvent> queue_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_{true};

    struct Debounced {
        DeviceEvent::Kind kind;
        DeviceInfo info; // latest info snapshot used for final event
        std::chrono::steady_clock::time_point deadline;
    };
    std::unordered_map<std::string, Debounced> pendings_; // uid -> pending attach/detach
};
