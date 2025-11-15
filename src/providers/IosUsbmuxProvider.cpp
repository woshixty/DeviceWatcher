// iOS usbmux/libimobiledevice integration (optional)
#include "providers/IosUsbmuxProvider.h"

#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

#if WITH_LIBIMOBILEDEVICE
// On supported builds, include libimobiledevice headers
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <plist/plist.h>
#endif

IosUsbmuxProvider::IosUsbmuxProvider(DeviceManager& manager)
    : manager_(manager) {}

IosUsbmuxProvider::~IosUsbmuxProvider() {
    stop();
}

bool IosUsbmuxProvider::isSupported() const {
#if WITH_LIBIMOBILEDEVICE
    return true;
#else
    return false;
#endif
}

void IosUsbmuxProvider::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    spdlog::info("[iOS] provider starting{}", isSupported() ? "" : " (no support, stub)");
    worker_ = std::thread([this]{ runLoop(); });
}

void IosUsbmuxProvider::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    spdlog::info("[iOS] provider stopping");
    if (worker_.joinable()) worker_.join();
}

void IosUsbmuxProvider::emitAttachBasic(const std::string& udid) {
    DeviceInfo info;
    info.type = Type::iOS;
    info.uid = udid;
    info.transport = "USB";
    info.online = true;
    info.manufacturer = "Apple";
    DeviceEvent evt{ DeviceEvent::Kind::Attach, info };
    manager_.onEvent(evt);
}

void IosUsbmuxProvider::emitDetach(const std::string& udid) {
    DeviceInfo info;
    info.type = Type::iOS;
    info.uid = udid;
    info.transport = "USB";
    info.online = false;
    DeviceEvent evt{ DeviceEvent::Kind::Detach, info };
    manager_.onEvent(evt);
}

void IosUsbmuxProvider::enrichInfo(const std::string& udid) {
#if WITH_LIBIMOBILEDEVICE
    idevice_t dev = nullptr;
    idevice_error_t derr = idevice_new(&dev, udid.c_str());
    if (derr != IDEVICE_E_SUCCESS || !dev) {
        spdlog::warn(
            "[iOS] idevice_new failed for {} err={} (提示: 请确保已安装并运行 iTunes/Apple Mobile Device Support, 设备已解锁并在弹窗中选择信任)",
            udid,
            static_cast<int>(derr)
        );
        return;
    }
    lockdownd_client_t client = nullptr;
    lockdownd_error_t lerr = lockdownd_client_new_with_handshake(dev, &client, "DeviceWatcher");
    if (lerr != LOCKDOWN_E_SUCCESS) {
        spdlog::warn(
            "[iOS] lockdown handshake failed for {} err={} (提示: 请在 iPhone 上点击信任此电脑, 或在 iTunes 中打开设备完成配对)",
            udid,
            static_cast<int>(lerr)
        );
        idevice_free(dev);
        return;
    }
    auto getStr = [&](const char* domain, const char* key) -> std::string {
        plist_t node = nullptr;
        std::string out;
        if (lockdownd_get_value(client, domain, key, &node) == LOCKDOWN_E_SUCCESS && node) {
            char* s = nullptr;
            uint64_t len = 0;
            if (plist_get_node_type(node) == PLIST_STRING) {
                plist_get_string_val(node, &s);
                if (s) { out = s; free(s); }
            }
            plist_free(node);
        }
        return out;
    };

    DeviceInfo info;
    info.type = Type::iOS;
    info.uid = udid;
    info.transport = "USB";
    info.online = true;
    info.manufacturer = "Apple";
    info.displayName = getStr(nullptr, "DeviceName");
    info.model = getStr(nullptr, "ProductType");
    info.osVersion = getStr(nullptr, "ProductVersion");
    if (!info.displayName.empty()) {
        info.displayName += " (" + udid + ")";
    }
    DeviceEvent evt{ DeviceEvent::Kind::InfoUpdated, info };
    manager_.onEvent(evt);

    lockdownd_client_free(client);
    idevice_free(dev);
#else
    (void)udid; // unused
#endif
}

void IosUsbmuxProvider::runLoop() {
#if WITH_LIBIMOBILEDEVICE
    // Subscribe to device events
    idevice_event_subscribe([](const idevice_event_t* event, void* userdata){
        auto* self = static_cast<IosUsbmuxProvider*>(userdata);
        if (!self->running_.load()) return;
        if (!event || !event->udid) return;
        std::string udid = event->udid;
        switch (event->event) {
            case IDEVICE_DEVICE_ADD:
                self->emitAttachBasic(udid);
                self->enrichInfo(udid);
                break;
            case IDEVICE_DEVICE_REMOVE:
                self->emitDetach(udid);
                break;
            default:
                break;
        }
    }, this);

    // Enumerate existing devices on start
    char** list = nullptr;
    int count = 0;
    if (idevice_get_device_list(&list, &count) == IDEVICE_E_SUCCESS && list) {
        for (int i = 0; i < count; ++i) {
            if (!list[i]) continue;
            std::string udid = list[i];
            emitAttachBasic(udid);
            enrichInfo(udid);
        }
        idevice_device_list_free(list);
    }

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    idevice_event_unsubscribe();
#else
    // Stub loop when not supported
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
#endif
}
