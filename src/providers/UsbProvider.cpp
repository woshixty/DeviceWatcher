// Windows USB provider: enrich DeviceInfo with VID/PID/usbPath via CM notifications
#include "providers/UsbProvider.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#ifndef GUID_DEVINTERFACE_USB_DEVICE
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
#endif
#endif

#include <chrono>
#include <cstring>
#include <algorithm>
#include <vector>

UsbProvider::UsbProvider(DeviceManager& manager)
    : manager_(manager) {}

UsbProvider::~UsbProvider() { stop(); }

void UsbProvider::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    spdlog::info("[USB] provider starting{}",
#ifdef _WIN32
                 ""
#else
                 " (stub)"
#endif
    );
    worker_ = std::thread([this]{ workerLoop(); });
}

void UsbProvider::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    spdlog::info("[USB] provider stopping");
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

std::string UsbProvider::utf8FromWide(const std::wstring& ws) {
#ifdef _WIN32
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
#else
    return {};
#endif
}

bool UsbProvider::parseVidPidFromPath(const std::wstring& path, uint16_t& vid, uint16_t& pid) {
    vid = pid = 0;
    auto findToken = [&](const wchar_t* key) -> uint16_t {
        auto pos = std::search(path.begin(), path.end(), key, key + wcslen(key));
        if (pos == path.end()) return 0;
        auto it = pos + wcslen(key);
        unsigned int v = 0;
        for (int i = 0; i < 4 && it != path.end(); ++i, ++it) {
            wchar_t c = towlower(*it);
            unsigned d = 0;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = 10 + (c - L'a');
            else break;
            v = (v << 4) | d;
        }
        return static_cast<uint16_t>(v);
    };
    uint16_t v = findToken(L"VID_");
    uint16_t p = findToken(L"PID_");
    if (v || p) { vid = v; pid = p; return true; }
    return false;
}

std::string UsbProvider::pickBestUidForUsb(uint16_t vid, uint16_t pid) {
    (void)pid;
    auto list = manager_.snapshot();
    // prefer devices that are online and missing USB info
    std::vector<DeviceInfo> cands;
    for (const auto& d : list) {
        if (!d.online) continue;
        if (d.vid != 0 || d.pid != 0) continue; // already enriched
        // Simple vendor heuristic: Apple -> iOS, common Android vendors -> Android
        if (vid == 0x05AC && d.type != Type::iOS) continue; // Apple
        if (vid != 0x05AC && d.type == Type::iOS) continue;
        cands.push_back(d);
    }
    if (cands.size() == 1) return cands.front().uid;

    // Use recency as tie-breaker
    using clk = std::chrono::system_clock;
    auto now = clk::now();
    std::string best;
    auto bestAge = std::chrono::seconds::max();
    for (const auto& d : cands) {
        auto since = manager_.onlineSince(d.uid);
        if (!since.has_value()) continue;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - *since);
        if (age < bestAge) { bestAge = age; best = d.uid; }
    }
    // only accept if seen recently
    if (!best.empty() && bestAge <= std::chrono::seconds(8)) return best;
    return {};
}

void UsbProvider::handleEvent(const Event& e) {
#ifdef _WIN32
    if (e.kind == Event::Kind::Arrive || e.kind == Event::Kind::Refresh) {
        uint16_t vid = e.vid, pid = e.pid;
        if (!vid && !pid) {
            parseVidPidFromPath(e.symlinkW, vid, pid);
        }
        const std::string path = utf8FromWide(e.symlinkW);
        std::string uid = pickBestUidForUsb(vid, pid);
        if (!uid.empty()) {
            DeviceInfo info;
            info.uid = uid;
            info.online = true;
            info.transport = "USB";
            info.vid = vid;
            info.pid = pid;
            info.usbPath = path;
            DeviceEvent evt{ DeviceEvent::Kind::InfoUpdated, info };
            manager_.onEvent(evt);
            pathToUid_[e.symlinkW] = uid;
            spdlog::info("[USB] enriched uid={} vid=0x{:04x} pid=0x{:04x}", uid, (unsigned)vid, (unsigned)pid);
        } else {
            spdlog::debug("[USB] no matching uid for path={} vid=0x{:04x} pid=0x{:04x}", path, (unsigned)vid, (unsigned)pid);
        }
    } else if (e.kind == Event::Kind::Remove) {
        auto it = pathToUid_.find(e.symlinkW);
        if (it != pathToUid_.end()) {
            // no need to send detach; higher layers handle via ADB/iOS
            pathToUid_.erase(it);
        }
    }
#else
    (void)e;
#endif
}

void UsbProvider::workerLoop() {
#ifdef _WIN32
    // Enumeration at start
    enumeratePresent();

    // Register CM notification
    HCMNOTIFICATION hNotify = nullptr;
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;

    auto callback = [](HCMNOTIFICATION h, PVOID ctx, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD size) -> DWORD {
        (void)h; (void)size;
        auto* self = reinterpret_cast<UsbProvider*>(ctx);
        if (!self->running_.load()) return ERROR_SUCCESS;
        if (!data || data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
        UsbProvider::Event e{};
        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
            e.kind = Event::Kind::Arrive;
        } else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
            e.kind = Event::Kind::Remove;
        } else {
            return ERROR_SUCCESS;
        }
        e.symlinkW = data->u.DeviceInterface.SymbolicLink;
        {
            std::lock_guard<std::mutex> lk(self->mtx_);
            self->q_.push(std::move(e));
        }
        self->cv_.notify_one();
        return ERROR_SUCCESS;
    };

    CONFIGRET cr = CM_Register_Notification(&filter, this, callback, &hNotify);
    if (cr != CR_SUCCESS) {
        spdlog::warn("[USB] CM_Register_Notification failed cr={}", (int)cr);
    }

    while (running_.load()) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (q_.empty()) {
            cv_.wait_for(lk, std::chrono::milliseconds(500));
        }
        while (!q_.empty()) {
            Event e = std::move(q_.front());
            q_.pop();
            lk.unlock();
            handleEvent(e);
            lk.lock();
        }
    }

    if (hNotify) {
        CM_Unregister_Notification(hNotify);
    }
#else
    // Stub: just idle until stop
    while (running_.load()) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(500));
    }
#endif
}

void UsbProvider::enumeratePresent() {
#ifdef _WIN32
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, i, &ifData); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;
        std::vector<char> buf(needed);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(buf.data());
#ifdef UNICODE
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
#else
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
#endif
        if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, detail, needed, nullptr, nullptr)) {
            continue;
        }
#ifdef UNICODE
        std::wstring sym = detail->DevicePath;
#else
        std::wstring sym; // Ansi build not expected
#endif
        Event e{};
        e.kind = Event::Kind::Refresh;
        e.symlinkW = std::move(sym);
        parseVidPidFromPath(e.symlinkW, e.vid, e.pid);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            q_.push(std::move(e));
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    cv_.notify_one();
#else
    // no-op
#endif
}
