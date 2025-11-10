#include "providers/AndroidAdbProvider.h"

#include <chrono>
#include <array>
#include <sstream>
#include <stdexcept>

#include <fmt/format.h>

using asio::ip::tcp;

AndroidAdbProvider::AndroidAdbProvider(DeviceManager& manager)
    : manager_(manager) {}

AndroidAdbProvider::~AndroidAdbProvider() {
    stop();
}

void AndroidAdbProvider::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // already running
    }
    worker_ = std::thread([this]() { runLoop(); });
}

void AndroidAdbProvider::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // not running
    }
    // Close current socket to break any blocking reads
    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        if (currentSocket_) {
            std::error_code ec;
            currentSocket_->shutdown(tcp::socket::shutdown_both, ec);
            currentSocket_->close(ec);
        }
    }
    if (worker_.joinable()) worker_.join();
}

void AndroidAdbProvider::runLoop() {
    std::unordered_map<std::string, DeviceInfo> known; // serial -> info
    while (running_) {
        try {
            asio::io_context io;
            tcp::socket socket(io);
            {
                std::lock_guard<std::mutex> lk(sockMtx_);
                currentSocket_ = std::make_shared<tcp::socket>(std::move(socket));
            }
            // Connect
            tcp::resolver resolver(io);
            auto endpoints = resolver.resolve("127.0.0.1", "5037");
            asio::connect(*currentSocket_, endpoints);

            // Send request and process streaming updates
            sendAdbRequest(*currentSocket_, "host:track-devices-l");

            // On successful connect, reset known to ensure correct ATTACH notifications
            known.clear();

            for (;;) {
                if (!running_) break;
                std::string block = readLenBlock(*currentSocket_);
                // block contains multiple lines separated by '\n'
                std::unordered_map<std::string, DeviceInfo> fresh;

                std::string line;
                std::istringstream iss(block);
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;

                    // Expect: <serial>\t<state>\tproduct:<...>\tmodel:<...>\tdevice:<...>
                    std::string serial;
                    std::string state;
                    std::string product;
                    std::string model;
                    std::string device;

                    // Split by tabs
                    std::vector<std::string> parts;
                    std::string tmp;
                    std::istringstream lss(line);
                    while (std::getline(lss, tmp, '\t')) parts.push_back(tmp);
                    if (parts.size() >= 2) {
                        serial = parts[0];
                        state = parts[1];
                        for (size_t i = 2; i < parts.size(); ++i) {
                            const auto& p = parts[i];
                            if (p.rfind("product:", 0) == 0) product = p.substr(8);
                            else if (p.rfind("model:", 0) == 0) model = p.substr(6);
                            else if (p.rfind("device:", 0) == 0) device = p.substr(7);
                        }
                        DeviceInfo info;
                        info.type = Type::Android;
                        info.uid = serial;
                        info.displayName = model.empty() ? serial : model + " (" + serial + ")";
                        info.online = (state == "device");
                        info.model = model;
                        info.adbState = state;
                        fresh[serial] = info;
                    }
                }

                // Diff known vs fresh
                // Attach: in fresh, not in known
                for (const auto& kv : fresh) {
                    const auto& serial = kv.first;
                    const auto& info = kv.second;
                    auto it = known.find(serial);
                    if (it == known.end()) {
                        DeviceEvent evt{ DeviceEvent::Kind::Attach, info };
                        manager_.onEvent(evt);
                    } else {
                        const DeviceInfo& old = it->second;
                        if (old.adbState != info.adbState || old.model != info.model || old.online != info.online) {
                            DeviceEvent evt{ DeviceEvent::Kind::InfoUpdated, info };
                            manager_.onEvent(evt);
                        }
                    }
                }
                // Detach: in known, not in fresh
                for (const auto& kv : known) {
                    const auto& serial = kv.first;
                    if (fresh.find(serial) == fresh.end()) {
                        DeviceInfo info = kv.second;
                        info.online = false;
                        DeviceEvent evt{ DeviceEvent::Kind::Detach, info };
                        manager_.onEvent(evt);
                    }
                }

                known.swap(fresh);
            }
        } catch (const std::exception& ex) {
            // Connection issue; fall-through to retry
        }

        // Small sleep before reconnect
        for (int i = 0; i < 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void AndroidAdbProvider::sendAdbRequest(asio::ip::tcp::socket& socket, const std::string& payload) {
    // Send 4-hex length + payload
    const std::string len = fmt::format("{:04x}", (unsigned)payload.size());
    std::array<asio::const_buffer, 2> bufs = {asio::buffer(len), asio::buffer(payload)};
    asio::write(socket, bufs);

    // Response: 4 bytes OKAY/FAIL
    std::string resp = readExact(socket, 4);
    if (resp == "OKAY") {
        return;
    } else if (resp == "FAIL") {
        std::string l4 = readExact(socket, 4);
        std::size_t n = parseHexLen4(l4);
        std::string msg = readExact(socket, n);
        throw std::runtime_error("ADB FAIL: " + msg);
    } else {
        throw std::runtime_error("ADB invalid response: " + resp);
    }
}

std::string AndroidAdbProvider::readExact(asio::ip::tcp::socket& socket, std::size_t n) {
    std::string out;
    out.resize(n);
    asio::read(socket, asio::buffer(out.data(), n));
    return out;
}

std::size_t AndroidAdbProvider::parseHexLen4(const std::string& s) {
    if (s.size() != 4) throw std::runtime_error("invalid length header size");
    unsigned int v = 0;
    std::stringstream ss;
    ss << std::hex << s;
    ss >> v;
    return static_cast<std::size_t>(v);
}

std::string AndroidAdbProvider::readLenBlock(asio::ip::tcp::socket& socket) {
    std::string l4 = readExact(socket, 4);
    std::size_t n = parseHexLen4(l4);
    if (n == 0) return std::string();
    return readExact(socket, n);
}
