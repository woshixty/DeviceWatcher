#include "providers/AndroidAdbProvider.h"

#include <chrono>
#include <array>
#include <vector>
#include <sstream>
#include <stdexcept>

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <cstdlib>

using asio::ip::tcp;

AndroidAdbProvider::AndroidAdbProvider(DeviceManager& manager)
    : manager_(manager) {
    // Allow overriding ADB server via env
    if (const char* s = std::getenv("ADB_SERVER_SOCKET")) {
        std::string v = s;
        // Expect tcp:HOST:PORT
        const std::string prefix = "tcp:";
        if (v.rfind(prefix, 0) == 0) {
            std::string rest = v.substr(prefix.size());
            auto pos = rest.rfind(':');
            if (pos != std::string::npos) {
                host_ = rest.substr(0, pos);
                port_ = rest.substr(pos + 1);
            }
        }
    }
    if (const char* h = std::getenv("ADB_SERVER_HOST")) host_ = h;
    if (const char* h2 = std::getenv("ADB_HOST")) host_ = h2; // compatibility
    if (const char* p = std::getenv("ADB_SERVER_PORT")) port_ = p;
    spdlog::info("[ADB] using server {}:{}", host_, port_);
}

AndroidAdbProvider::~AndroidAdbProvider() {
    stop();
}

void AndroidAdbProvider::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // already running
    }
    spdlog::info("[ADB] provider starting");
    worker_ = std::thread([this]() { runLoop(); });
}

void AndroidAdbProvider::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // not running
    }
    spdlog::info("[ADB] provider stopping");
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
            auto sockPtr = std::make_shared<tcp::socket>(io);
            {
                std::lock_guard<std::mutex> lk(sockMtx_);
                currentSocket_ = sockPtr;
            }
            spdlog::debug("[ADB] resolving {}:{}", host_, port_);
            // Connect
            tcp::resolver resolver(io);
            auto endpoints = resolver.resolve(host_, port_);
            std::error_code ec;
            asio::connect(*currentSocket_, endpoints, ec);
            if (ec) {
                throw std::runtime_error(std::string("ADB connect failed: ") + ec.message());
            }
            spdlog::info("[ADB] connected to {}:{}", host_, port_);

            // Send request and process streaming updates
            sendAdbRequest(*currentSocket_, "host:track-devices-l");
            spdlog::info("[ADB] sent track-devices-l request and received OKAY");

            // On successful connect, reset known to ensure correct ATTACH notifications
            known.clear();

            for (;;) {
                if (!running_) break;
                std::string block = readLenBlock(*currentSocket_);
                spdlog::debug("[ADB] received block size={} bytes", block.size());
                spdlog::debug("[ADB] block preview: {}", block.size() <= 200 ? block : (block.substr(0, 200) + "...") );
                if (block.empty()) {
                    // Some ADB builds may send empty heartbeat blocks; ignore.
                    continue;
                }
                // block contains multiple lines separated by '\n'
                std::unordered_map<std::string, DeviceInfo> fresh;

                std::string line;
                std::istringstream iss(block);
                int parsedLines = 0;
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;

                    // Robust parse: serial, state, extras (separator can be tab or spaces)
                    std::string serial;
                    std::string state;
                    std::string product;
                    std::string model;
                    std::string device;
                    std::string transportId;

                    // Tokenize by whitespace, but preserve the first two tokens (serial, state)
                    std::istringstream ws(line);
                    if (!(ws >> serial)) {
                        spdlog::debug("[ADB] skip line (no serial): {}", line);
                        continue;
                    }
                    if (!(ws >> state)) {
                        spdlog::debug("[ADB] skip line (no state): {}", line);
                        continue;
                    }
                    // remaining tokens are key:value pairs
                    std::string tok;
                    while (ws >> tok) {
                        if (tok.rfind("product:", 0) == 0) product = tok.substr(8);
                        else if (tok.rfind("model:", 0) == 0) model = tok.substr(6);
                        else if (tok.rfind("device:", 0) == 0) device = tok.substr(7);
                        else if (tok.rfind("transport_id:", 0) == 0) transportId = tok.substr(13);
                    }

                    DeviceInfo info;
                    info.type = Type::Android;
                    info.uid = serial;
                    info.displayName = model.empty() ? serial : model + " (" + serial + ")";
                    info.online = (state == "device");
                    info.model = model;
                    info.adbState = state;
                    fresh[serial] = info;
                    ++parsedLines;
                    spdlog::debug("[ADB] line parsed: serial={} state={} model={} product={} device={} transport_id={}",
                                  serial, state, model, product, device, transportId);
                }
                spdlog::info("[ADB] parsed {} device line(s)", parsedLines);

                // Diff known vs fresh
                // Attach: in fresh, not in known
                int attachCount = 0, updateCount = 0, detachCount = 0;
                for (const auto& kv : fresh) {
                    const auto& serial = kv.first;
                    const auto& info = kv.second;
                    auto it = known.find(serial);
                    if (it == known.end()) {
                        DeviceEvent evt{ DeviceEvent::Kind::Attach, info };
                        manager_.onEvent(evt);
                        ++attachCount;
                        spdlog::info("[ADB] ATTACH serial={} model={} state={}", info.uid, info.model, info.adbState);
                    } else {
                        const DeviceInfo& old = it->second;
                        if (old.adbState != info.adbState || old.model != info.model || old.online != info.online) {
                            DeviceEvent evt{ DeviceEvent::Kind::InfoUpdated, info };
                            manager_.onEvent(evt);
                            ++updateCount;
                            spdlog::info("[ADB] INFOUPDATED serial={} model={} state={} (prev={})",
                                         info.uid, info.model, info.adbState, old.adbState);
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
                        ++detachCount;
                        spdlog::info("[ADB] DETACH serial={} model={} state={}", info.uid, info.model, info.adbState);
                    }
                }
                spdlog::info("[ADB] diff result: attach={} update={} detach={}", attachCount, updateCount, detachCount);

                known.swap(fresh);
            }
        } catch (const std::exception& ex) {
            // Connection issue; fall-through to retry
            spdlog::warn("[ADB] error: {}", ex.what());
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
