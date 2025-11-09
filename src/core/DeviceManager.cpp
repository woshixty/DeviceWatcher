#include "core/DeviceManager.h"

#include <algorithm>

DeviceManager::Snapshot DeviceManager::snapshot() const {
    Snapshot list;
    list.reserve(devices_.size());
    for (const auto& kv : devices_) {
        list.push_back(kv.second);
    }
    return list;
}

int DeviceManager::subscribe(Subscriber cb) {
    subscribers_.push_back(std::move(cb));
    // Return token as index+1 (0 reserved as invalid)
    return static_cast<int>(subscribers_.size());
}

void DeviceManager::unsubscribe(int token) {
    if (token <= 0) return;
    const size_t idx = static_cast<size_t>(token - 1);
    if (idx < subscribers_.size()) {
        subscribers_[idx] = nullptr; // mark as removed
    }
}

void DeviceManager::addOrUpdateDevice(const DeviceInfo& info) {
    devices_[info.uid] = info;
    // Notify subscribers (minimal behavior)
    DeviceEvent evt{ "updated", info.uid, info.displayName, info.online };
    for (auto& sub : subscribers_) {
        if (sub) sub(evt);
    }
}

void DeviceManager::removeDevice(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it != devices_.end()) {
        DeviceEvent evt{ "removed", it->second.uid, it->second.displayName, false };
        devices_.erase(it);
        for (auto& sub : subscribers_) {
            if (sub) sub(evt);
        }
    }
}

