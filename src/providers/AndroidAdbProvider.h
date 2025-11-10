#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

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

    DeviceManager& manager_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex sockMtx_;
    std::shared_ptr<asio::ip::tcp::socket> currentSocket_;
};
