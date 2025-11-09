#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/DeviceModel.h"

class DeviceManager {
public:
    using Snapshot = std::vector<DeviceInfo>;
    using Subscriber = std::function<void(const DeviceEvent&)>;

    DeviceManager() = default;
    ~DeviceManager() = default;

    // Return a copy of the current device list.
    Snapshot snapshot() const;

    // Subscribe to device events (minimal stub for now). Returns token index.
    int subscribe(Subscriber cb);
    void unsubscribe(int token);

    // The below helpers are stubs for future expansion.
    void addOrUpdateDevice(const DeviceInfo& info);
    void removeDevice(const std::string& uid);

private:
    std::unordered_map<std::string, DeviceInfo> devices_; // uid -> info
    std::vector<Subscriber> subscribers_;                 // simple subscriber list
};

