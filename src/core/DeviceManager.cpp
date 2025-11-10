#include "core/DeviceManager.h"

#include <algorithm>

DeviceManager::Snapshot DeviceManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    Snapshot list;
    list.reserve(devices_.size());
    for (const auto& kv : devices_) {
        list.push_back(kv.second);
    }
    return list;
}

int DeviceManager::subscribe(Subscriber cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    subscribers_.push_back(std::move(cb));
    // Return token as index+1 (0 reserved as invalid)
    return static_cast<int>(subscribers_.size());
}

void DeviceManager::unsubscribe(int token) {
    if (token <= 0) return;
    std::lock_guard<std::mutex> lock(mtx_);
    const size_t idx = static_cast<size_t>(token - 1);
    if (idx < subscribers_.size()) {
        subscribers_[idx] = nullptr; // mark as removed
    }
}

void DeviceManager::addOrUpdateDevice(const DeviceInfo& info) {
    std::vector<Subscriber> subsCopy;
    DeviceEvent evt;
    evt.kind = DeviceEvent::Kind::InfoUpdated;
    evt.info = info;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        devices_[info.uid] = info;
        subsCopy = subscribers_;
    }
    for (auto& sub : subsCopy) {
        if (sub) sub(evt);
    }
}

void DeviceManager::removeDevice(const std::string& uid) {
    std::vector<Subscriber> subsCopy;
    DeviceEvent evt;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = devices_.find(uid);
        if (it != devices_.end()) {
            evt.kind = DeviceEvent::Kind::Detach;
            evt.info = it->second;
            evt.info.online = false;
            devices_.erase(it);
            subsCopy = subscribers_;
            found = true;
        }
    }
    if (found) {
        for (auto& sub : subsCopy) {
            if (sub) sub(evt);
        }
    }
}

void DeviceManager::onEvent(const DeviceEvent& evt) {
    std::vector<Subscriber> subsCopy;
    DeviceEvent toSend = evt;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        switch (evt.kind) {
            case DeviceEvent::Kind::Attach:
            case DeviceEvent::Kind::InfoUpdated:
                devices_[evt.info.uid] = evt.info;
                break;
            case DeviceEvent::Kind::Detach:
                devices_.erase(evt.info.uid);
                break;
        }
        subsCopy = subscribers_;
    }
    for (auto& sub : subsCopy) {
        if (sub) sub(toSend);
    }
}
