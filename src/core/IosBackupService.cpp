#include "core/IosBackupService.h"

#include <spdlog/spdlog.h>

#if WITH_LIBIMOBILEDEVICE
// Real implementation using libimobiledevice / lockdownd.
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <plist/plist.h>

bool IosBackupService::TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg) {
    errMsg.clear();
    if (udid.empty()) {
        errMsg = "UDID 不能为空";
        return false;
    }

    idevice_t dev = nullptr;
    idevice_error_t derr = idevice_new(&dev, udid.c_str());
    if (derr != IDEVICE_E_SUCCESS || !dev) {
        spdlog::warn("[IosBackup] idevice_new failed for {} err={}", udid, static_cast<int>(derr));
        if (derr == IDEVICE_E_NO_DEVICE) {
            errMsg = "No device with UDID " + udid;
        } else {
            errMsg = "idevice_new error code " + std::to_string(static_cast<int>(derr));
        }
        return false;
    }

    lockdownd_client_t client = nullptr;
    lockdownd_error_t lerr = lockdownd_client_new_with_handshake(dev, &client, "DeviceWatcherBackup");
    if (lerr != LOCKDOWN_E_SUCCESS) {
        spdlog::warn("[IosBackup] lockdown handshake failed for {} err={}", udid, static_cast<int>(lerr));
        errMsg = "lockdownd handshake failed, error code " + std::to_string(static_cast<int>(lerr));
        idevice_free(dev);
        return false;
    }

    auto getStr = [&](const char* domain, const char* key) -> std::string {
        plist_t node = nullptr;
        std::string out;
        if (lockdownd_get_value(client, domain, key, &node) == LOCKDOWN_E_SUCCESS && node) {
            char* s = nullptr;
            if (plist_get_node_type(node) == PLIST_STRING) {
                plist_get_string_val(node, &s);
                if (s) {
                    out = s;
                    free(s);
                }
            }
            plist_free(node);
        }
        return out;
    };

    DeviceInfo info;
    info.type = Type::iOS;
    info.uid = udid;
    info.online = true;
    info.transport = "USB";
    info.manufacturer = "Apple";
    info.deviceName = getStr(nullptr, "DeviceName");
    info.productType = getStr(nullptr, "ProductType");
    info.osVersion = getStr(nullptr, "ProductVersion");
    // Backwards compatible fields
    info.displayName = info.deviceName;
    info.model = info.productType;

    if (outInfo) {
        *outInfo = info;
    }

    lockdownd_client_free(client);
    idevice_free(dev);

    spdlog::info("[IosBackup] TestConnection success udid={} name={} type={} os={}",
                 udid, info.deviceName, info.productType, info.osVersion);
    return true;
}

#else

// Stub implementation when libimobiledevice support is not compiled in.
bool IosBackupService::TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg) {
    (void)udid;
    (void)outInfo;
    errMsg = "IosBackupService: 当前构建未启用 libimobiledevice（请在 CMake 中使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] TestConnection called but libimobiledevice support is not enabled");
    return false;
}

#endif

