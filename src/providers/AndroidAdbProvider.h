#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <vector>

#include <asio.hpp>

#include "core/DeviceManager.h"

class AndroidAdbProvider {
public:
    explicit AndroidAdbProvider(DeviceManager& manager);
    ~AndroidAdbProvider();

    void start();
    void stop();

    // Alias methods as requested
    void Start() { start(); }
    void Stop() { stop(); }

    std::string name() const { return "AndroidAdbProvider"; }

private:
    void runLoop();
    void trackDevices(asio::ip::tcp::socket& socket);

    static void sendAdbRequest(asio::ip::tcp::socket& socket, const std::string& payload);
    static std::string readExact(asio::ip::tcp::socket& socket, std::size_t n);
    static std::string readLenBlock(asio::ip::tcp::socket& socket);
    static std::size_t parseHexLen4(const std::string& s);
    static std::string readUntilEof(asio::ip::tcp::socket& socket, std::size_t maxBytes = 262144);
    static void parseGetprop(const std::string& text, DeviceInfo& infoOut);

    void scheduleEnrichIfNeeded(const DeviceInfo& newInfo, const DeviceInfo* oldInfo);
    void enrichWorker(std::string serial);

    DeviceManager& manager_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex sockMtx_;
    std::shared_ptr<asio::ip::tcp::socket> currentSocket_;

    // ADB server endpoint (configurable via env)
    std::string host_ = "127.0.0.1";
    std::string port_ = "5037";

    // Enrichment bookkeeping
    std::mutex enrichMtx_;
    std::unordered_set<std::string> enriching_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastEnrich_;
    std::vector<std::thread> enrichThreads_;
};
