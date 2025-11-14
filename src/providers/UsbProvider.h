#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>

#include "core/DeviceManager.h"

class UsbProvider {
public:
    explicit UsbProvider(DeviceManager& manager);
    ~UsbProvider();

    void start();
    void stop();

    std::string name() const { return "UsbProvider"; }
    bool isRunning() const { return running_.load(); }

private:
    struct Event {
        enum class Kind { Arrive, Remove, Refresh };
        Kind kind;
        std::wstring symlinkW; // raw device interface path
        uint16_t vid{0};
        uint16_t pid{0};
    };

    void workerLoop();
    void handleEvent(const Event& e);
    void enumeratePresent();

    static std::string utf8FromWide(const std::wstring& ws);
    static bool parseVidPidFromPath(const std::wstring& path, uint16_t& vid, uint16_t& pid);

    // Heuristic association to existing device uid
    std::string pickBestUidForUsb(uint16_t vid, uint16_t pid);

    DeviceManager& manager_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<Event> q_;
    // Remember mapping from symlink path to uid for removals if needed
    std::unordered_map<std::wstring, std::string> pathToUid_;
};
