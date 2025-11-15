#include "core/ExternalNotifier.h"

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <sstream>
#include <system_error>

#include "core/Utils.h"

using asio::ip::tcp;
using nlohmann::json;

ExternalNotifier::ExternalNotifier(DeviceManager& manager)
    : manager_(manager) {
    running_ = true;
    httpNextAllowed_ = std::chrono::steady_clock::now();
    tcpNextAllowed_ = httpNextAllowed_;

    subToken_ = manager_.subscribe([this](const DeviceEvent& evt) {
        QueuedEvent q{evt, std::chrono::system_clock::now()};
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(q));
        }
        cv_.notify_one();
    });

    worker_ = std::thread([this]() { workerLoop(); });
}

ExternalNotifier::~ExternalNotifier() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        running_ = false;
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    if (subToken_ > 0) {
        manager_.unsubscribe(subToken_);
    }
}

void ExternalNotifier::setWebhookUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(settingsMtx_);
    settings_.webhookUrl = url;
}

void ExternalNotifier::setLocalTcpEndpoint(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(settingsMtx_);
    settings_.localTcpEndpoint = endpoint;
}

ExternalNotifier::Settings ExternalNotifier::currentSettings() const {
    std::lock_guard<std::mutex> lk(settingsMtx_);
    return settings_;
}

void ExternalNotifier::workerLoop() {
    while (true) {
        QueuedEvent q;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]() { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) {
                break;
            }
            q = std::move(queue_.front());
            queue_.pop();
        }
        handle(q);
    }
}

void ExternalNotifier::handle(const QueuedEvent& q) {
    const auto nowSteady = std::chrono::steady_clock::now();
    const std::string line = eventToJsonLine(q.evt, q.ts);

    Settings cfg = currentSettings();

    if (!cfg.webhookUrl.empty() && nowSteady >= httpNextAllowed_) {
        if (!sendHttpPost(cfg.webhookUrl, line)) {
            spdlog::warn("[notify] webhook POST failed, backoff");
            httpNextAllowed_ = nowSteady + std::chrono::seconds(3);
        } else {
            httpNextAllowed_ = nowSteady;
        }
    }

    if (!cfg.localTcpEndpoint.empty() && nowSteady >= tcpNextAllowed_) {
        if (!sendTcpNdjson(cfg.localTcpEndpoint, line)) {
            spdlog::warn("[notify] local TCP push failed, backoff");
            tcpNextAllowed_ = nowSteady + std::chrono::seconds(3);
        } else {
            tcpNextAllowed_ = nowSteady;
        }
    }
}

std::string ExternalNotifier::eventToJsonLine(const DeviceEvent& evt,
                                              const std::chrono::system_clock::time_point& ts) {
    json o;
    o["ts"] = Utils::formatTimeISO8601(ts);
    o["event"] = kindToString(evt.kind);

    const DeviceInfo& d = evt.info;
    json dev;
    dev["type"] = typeToString(d.type);
    dev["uid"] = d.uid;
    dev["manufacturer"] = d.manufacturer;
    dev["model"] = d.model;
    dev["osVersion"] = d.osVersion;
    dev["transport"] = d.transport;
    dev["vid"] = d.vid;
    dev["pid"] = d.pid;

    o["device"] = std::move(dev);
    return o.dump();
}

std::string ExternalNotifier::kindToString(DeviceEvent::Kind k) {
    switch (k) {
        case DeviceEvent::Kind::Attach: return "attach";
        case DeviceEvent::Kind::Detach: return "detach";
        case DeviceEvent::Kind::InfoUpdated: return "info";
    }
    return "info";
}

const char* ExternalNotifier::typeToString(Type t) {
    switch (t) {
        case Type::Android: return "Android";
        case Type::iOS: return "iOS";
        default: return "Unknown";
    }
}

namespace {
struct ParsedUrl {
    std::string host;
    std::string port;
    std::string target;
};

// Very small HTTP URL parser: supports http://host[:port]/path or host[:port]/path.
bool parseHttpUrl(const std::string& url, ParsedUrl& out) {
    std::string work = url;
    const std::string httpPrefix = "http://";
    if (work.rfind(httpPrefix, 0) == 0) {
        work = work.substr(httpPrefix.size());
    }

    auto slash = work.find('/');
    std::string hostport = (slash == std::string::npos) ? work : work.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : work.substr(slash);

    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
        out.port = "80";
    } else {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    }
    out.target = path.empty() ? "/" : path;
    return !out.host.empty();
}
} // namespace

bool ExternalNotifier::sendHttpPost(const std::string& url, const std::string& body) {
    ParsedUrl u;
    if (!parseHttpUrl(url, u)) {
        spdlog::warn("[notify] invalid webhook URL: {}", url);
        return false;
    }

    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(u.host, u.port);
        tcp::socket socket(io);
        asio::connect(socket, endpoints);

        std::ostringstream req;
        req << "POST " << u.target << " HTTP/1.1\r\n";
        req << "Host: " << u.host << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
        req << "Connection: close\r\n\r\n";
        req << body;
        std::string reqStr = req.str();

        asio::write(socket, asio::buffer(reqStr));

        // Best-effort: read some response bytes and ignore
        std::array<char, 256> buf{};
        std::error_code ec;
        socket.read_some(asio::buffer(buf), ec);
        (void)ec;
        return true;
    } catch (const std::exception& ex) {
        spdlog::warn("[notify] HTTP POST to {} failed: {}", url, ex.what());
        return false;
    }
}

bool ExternalNotifier::sendTcpNdjson(const std::string& endpoint, const std::string& line) {
    // endpoint format: host:port (e.g. 127.0.0.1:9009)
    auto pos = endpoint.find(':');
    if (pos == std::string::npos) {
        spdlog::warn("[notify] invalid TCP endpoint: {}", endpoint);
        return false;
    }
    std::string host = endpoint.substr(0, pos);
    std::string port = endpoint.substr(pos + 1);
    if (host.empty() || port.empty()) {
        spdlog::warn("[notify] invalid TCP endpoint: {}", endpoint);
        return false;
    }

    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(host, port);
        tcp::socket socket(io);
        asio::connect(socket, endpoints);

        std::string out = line;
        out.push_back('\n');
        asio::write(socket, asio::buffer(out));
        return true;
    } catch (const std::exception& ex) {
        spdlog::warn("[notify] TCP send to {} failed: {}", endpoint, ex.what());
        return false;
    }
}

