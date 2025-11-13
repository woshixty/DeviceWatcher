#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "core/DeviceManager.h"

class IosUsbmuxProvider {
public:
    explicit IosUsbmuxProvider(DeviceManager& manager);
    ~IosUsbmuxProvider();

    void start();
    void stop();

    // Capability and state
    bool isSupported() const;
    bool isRunning() const { return running_.load(); }

    std::string name() const { return "IosUsbmuxProvider"; }

private:
    void runLoop();
    void emitAttachBasic(const std::string& udid);
    void emitDetach(const std::string& udid);
    void enrichInfo(const std::string& udid);

    DeviceManager& manager_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};
