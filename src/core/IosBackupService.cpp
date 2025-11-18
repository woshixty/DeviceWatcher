#include "core/IosBackupService.h"

#include <spdlog/spdlog.h>

#if WITH_LIBIMOBILEDEVICE

// 真正实现：使用 libimobiledevice / lockdownd / mobilebackup2。
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/mobilebackup2.h>
#include <plist/plist.h>

#include <filesystem>
#include <cstdlib>
#include <sstream>

// 测试连接
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
    info.osVersion  = getStr(nullptr, "ProductVersion");
    // 向后兼容字段
    info.displayName = info.deviceName;
    info.model       = info.productType;

    if (outInfo) {
        *outInfo = info;
    }

    lockdownd_client_free(client);
    idevice_free(dev);

    spdlog::info("[IosBackup] TestConnection success udid={} name={} type={} os={}",
                 udid, info.deviceName, info.productType, info.osVersion);
    return true;
}

IosBackupService::BackupResult IosBackupService::PerformBackup(
    const std::string& udid,
    const BackupOptions& opt,
    std::function<void(double,const std::string&)> onProgress)
{
    BackupResult res;
    if (!onProgress) onProgress = [](double, const std::string&) {};

    if (opt.backupDir.empty()) {
        res.code = BackupResultCode::IOError;
        res.message = "backupDir 不能为空";
        return res;
    }
    if (opt.encrypt) {
        res.code = BackupResultCode::Unsupported;
        res.message = "Encrypted backup not supported in this version.";
        return res;
    }

    // 确保目录存在
    try {
        std::filesystem::path p(opt.backupDir);
        if (!p.empty()) {
            std::filesystem::create_directories(p);
        }
    } catch (const std::exception& ex) {
        res.code = BackupResultCode::IOError;
        res.message = std::string("创建备份目录失败: ") + ex.what();
        return res;
    }

    onProgress(0.0, "Preparing");

    // 简单做一次 idevice_new 检查 + 启动 mobilebackup2 会话，确保服务可用
    idevice_t dev = nullptr;
    idevice_error_t derr = idevice_new(&dev, udid.empty() ? nullptr : udid.c_str());
    if (derr != IDEVICE_E_SUCCESS || !dev) {
        if (derr == IDEVICE_E_NO_DEVICE) {
            res.code = BackupResultCode::NoDevice;
            res.message = "No device with UDID " + udid;
        } else {
            res.code = BackupResultCode::ConnectionError;
            res.message = "无法连接到设备，idevice_new error code " + std::to_string(static_cast<int>(derr));
        }
        return res;
    }

    mobilebackup2_client_t mb2 = nullptr;
    mobilebackup2_error_t mberr = mobilebackup2_client_start_service(dev, &mb2, "DeviceWatcherBackup");
    if (mberr != MOBILEBACKUP2_E_SUCCESS || !mb2) {
        spdlog::warn("[IosBackup] mobilebackup2_client_start_service failed udid={} err={}", udid, (int)mberr);
        res.code = BackupResultCode::Mobilebackup2Error;
        res.message = "无法启动 mobilebackup2 会话，错误码 " + std::to_string((int)mberr);
        idevice_free(dev);
        return res;
    }

    // 简单执行一次版本交换，确保协议可用
    double local_versions[] = { 2.0, 1.0 };
    double remote_version = 0.0;
    mberr = mobilebackup2_version_exchange(mb2, local_versions, (char)(sizeof(local_versions)/sizeof(local_versions[0])), &remote_version);
    if (mberr != MOBILEBACKUP2_E_SUCCESS) {
        spdlog::warn("[IosBackup] mobilebackup2_version_exchange failed err={}", (int)mberr);
        res.code = BackupResultCode::Mobilebackup2Error;
        res.message = "mobilebackup2 版本握手失败，错误码 " + std::to_string((int)mberr);
        mobilebackup2_client_free(mb2);
        idevice_free(dev);
        return res;
    }

    spdlog::info("[IosBackup] mobilebackup2 version exchange ok, remote={}", remote_version);

    mobilebackup2_client_free(mb2);
    idevice_free(dev);

    // 最小可用版本：真实备份交给 idevicebackup2 CLI，当前类负责前置检查、错误映射和进度回调。
    onProgress(0.1, "Running idevicebackup2");

    // 允许通过 IDEVICEBACKUP2_EXE 指定 exe 路径，否则使用 PATH 中的 idevicebackup2
    std::string exe = "idevicebackup2";
    if (const char* env = std::getenv("IDEVICEBACKUP2_EXE")) {
        if (*env) exe = env;
    }

    std::ostringstream cmd;
    cmd << "\"" << exe << "\" backup "
        << "\"" << opt.backupDir << "\"";
    if (!udid.empty()) {
        cmd << " -u \"" << udid << "\"";
    }

    const std::string cmdStr = cmd.str();
    spdlog::info("[IosBackup] running command: {}", cmdStr);

    int rc = std::system(cmdStr.c_str());
    if (rc != 0) {
        onProgress(1.0, "Backup failed");
        res.code = BackupResultCode::IOError;
        res.message = "idevicebackup2 备份失败，退出码=" + std::to_string(rc)
                    + "（请检查是否安装 idevicebackup2，设备是否已信任）";
        return res;
    }

    onProgress(1.0, "Backup finished");

    res.code = BackupResultCode::Ok;
    res.message = "备份完成: " + opt.backupDir;
    spdlog::info("[IosBackup] backup completed, dir={}", opt.backupDir);
    return res;
}

#else  // !WITH_LIBIMOBILEDEVICE

// 未启用 libimobiledevice：TestConnection 的 stub
bool IosBackupService::TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg) {
    (void)udid;
    (void)outInfo;
    errMsg = "IosBackupService: 当前构建未启用 libimobiledevice（请在 CMake 中使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] TestConnection called but libimobiledevice support is not enabled");
    return false;
}

IosBackupService::BackupResult IosBackupService::PerformBackup(
    const std::string& udid,
    const BackupOptions& opt,
    std::function<void(double,const std::string&)> onProgress)
{
    (void)udid;
    (void)opt;
    if (onProgress) {
        onProgress(0.0, "libimobiledevice not compiled in");
        onProgress(1.0, "Backup unsupported");
    }

    BackupResult res;
    res.code = BackupResultCode::Unsupported;
    res.message = "IosBackupService: 当前构建未启用 libimobiledevice（请使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] PerformBackup called but libimobiledevice support is not enabled");
    return res;
}

#endif

