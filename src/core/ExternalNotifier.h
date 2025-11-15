#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

#include "core/DeviceManager.h"

// ExternalNotifier: subscribes to DeviceManager and pushes events to
// optional webhook (HTTP POST) and/or local TCP endpoint (NDJSON lines).
class ExternalNotifier {
public:
    struct Settings {
        std::string webhookUrl;        // e.g. http://127.0.0.1:8080/hook
        std::string localTcpEndpoint;  // e.g. 127.0.0.1:9009
    };

    explicit ExternalNotifier(DeviceManager& manager);
    ~ExternalNotifier();

    void setWebhookUrl(const std::string& url);
    void setLocalTcpEndpoint(const std::string& endpoint);

    Settings currentSettings() const;

private:
    struct QueuedEvent {
        DeviceEvent evt;
        std::chrono::system_clock::time_point ts;
    };

    void workerLoop();
    void handle(const QueuedEvent& q);

    static std::string eventToJsonLine(const DeviceEvent& evt,
                                       const std::chrono::system_clock::time_point& ts);

    // Outputs (simple blocking I/O with basic backoff)
    bool sendHttpPost(const std::string& url, const std::string& body);
    bool sendTcpNdjson(const std::string& endpoint, const std::string& line);

    static std::string kindToString(DeviceEvent::Kind k);
    static const char* typeToString(Type t);

    DeviceManager& manager_;
    int subToken_{0};

    std::thread worker_;
    std::atomic<bool> running_{false};

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<QueuedEvent> queue_;

    mutable std::mutex settingsMtx_;
    Settings settings_{};

    // Backoff control (steady_clock)
    std::chrono::steady_clock::time_point httpNextAllowed_{};
    std::chrono::steady_clock::time_point tcpNextAllowed_{};
};

