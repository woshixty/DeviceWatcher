#include "core/DeviceManager.h"

#include <algorithm>

namespace {
constexpr std::chrono::milliseconds kDebounceMs(800);
}

DeviceManager::DeviceManager() {
    worker_ = std::thread([this]{ workerLoop(); });
}

DeviceManager::~DeviceManager() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

DeviceManager::Snapshot DeviceManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    Snapshot list;
    list.reserve(devices_.size());
    for (const auto& kv : devices_) {
        list.push_back(kv.second);
    }
    return list;
}

std::optional<std::chrono::system_clock::time_point> DeviceManager::onlineSince(const std::string& uid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = onlineSince_.find(uid);
    if (it == onlineSince_.end()) return std::nullopt;
    return it->second;
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
    // Not used in new flow; kept for compatibility
    std::lock_guard<std::mutex> lock(mtx_);
    devices_[info.uid] = info;
}

void DeviceManager::removeDevice(const std::string& uid) {
    std::lock_guard<std::mutex> lock(mtx_);
    devices_.erase(uid);
}

void DeviceManager::mergeInfo(DeviceInfo& dst, const DeviceInfo& src) {
    if (dst.type == Type::Unknown && src.type != Type::Unknown) dst.type = src.type;
    if (!src.uid.empty()) dst.uid = src.uid;
    if (!src.displayName.empty()) dst.displayName = src.displayName;
    dst.online = src.online;
    if (!src.model.empty()) dst.model = src.model;
    if (!src.adbState.empty()) dst.adbState = src.adbState;
    if (!src.manufacturer.empty()) dst.manufacturer = src.manufacturer;
    if (!src.osVersion.empty()) dst.osVersion = src.osVersion;
    if (!src.abi.empty()) dst.abi = src.abi;
}

void DeviceManager::onEvent(const DeviceEvent& evt) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(evt);
    }
    cv_.notify_one();
}

void DeviceManager::workerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lk(mtx_);
        // Compute next deadline to wait for
        auto nextDeadline = std::chrono::steady_clock::time_point::max();
        for (const auto& kv : pendings_) {
            if (kv.second.deadline < nextDeadline) nextDeadline = kv.second.deadline;
        }

        if (!running_ && queue_.empty() && pendings_.empty()) {
            break;
        }

        if (queue_.empty() && running_) {
            if (nextDeadline == std::chrono::steady_clock::time_point::max()) {
                cv_.wait(lk);
            } else {
                cv_.wait_until(lk, nextDeadline);
            }
        }

        // Pop all queued events
        while (!queue_.empty()) {
            DeviceEvent evt = queue_.front();
            queue_.pop();
            const std::string uid = evt.info.uid;
            auto now = std::chrono::steady_clock::now();

            switch (evt.kind) {
                case DeviceEvent::Kind::Attach: {
                    auto& entry = devices_[uid];
                    mergeInfo(entry, evt.info);
                    entry.online = true;
                    // schedule debounced attach
                    Debounced d{DeviceEvent::Kind::Attach, entry, now + kDebounceMs};
                    pendings_[uid] = d;
                    break;
                }
                case DeviceEvent::Kind::InfoUpdated: {
                    auto& entry = devices_[uid];
                    mergeInfo(entry, evt.info);
                    // immediate notify
                    std::vector<Subscriber> subs = subscribers_;
                    DeviceEvent out{DeviceEvent::Kind::InfoUpdated, entry};
                    lk.unlock();
                    for (auto& s : subs) if (s) s(out);
                    lk.lock();
                    break;
                }
                case DeviceEvent::Kind::Detach: {
                    auto it = devices_.find(uid);
                    if (it != devices_.end()) {
                        it->second.online = false;
                        Debounced d{DeviceEvent::Kind::Detach, it->second, now + kDebounceMs};
                        pendings_[uid] = d;
                    } else {
                        // still create a pending detach with minimal info
                        Debounced d{DeviceEvent::Kind::Detach, evt.info, now + kDebounceMs};
                        d.info.online = false;
                        pendings_[uid] = d;
                    }
                    break;
                }
            }
        }

        // Fire any expired debounced events
        std::vector<DeviceEvent> toSend;
        auto nowtp = std::chrono::steady_clock::now();
        for (auto it = pendings_.begin(); it != pendings_.end(); ) {
            if (it->second.deadline <= nowtp) {
                const std::string uid = it->first;
                Debounced d = it->second;
                if (d.kind == DeviceEvent::Kind::Detach) {
                    // remove device entry
                    devices_.erase(uid);
                    onlineSince_.erase(uid);
                } else {
                    // ensure device is online
                    devices_[uid].online = true;
                    // set onlineSince if not set
                    if (onlineSince_.find(uid) == onlineSince_.end()) {
                        onlineSince_[uid] = std::chrono::system_clock::now();
                    }
                }
                toSend.push_back(DeviceEvent{d.kind, d.info});
                it = pendings_.erase(it);
            } else {
                ++it;
            }
        }

        if (!toSend.empty()) {
            std::vector<Subscriber> subs = subscribers_;
            lk.unlock();
            for (auto& e : toSend) {
                for (auto& s : subs) if (s) s(e);
            }
            lk.lock();
        }
    }
}
