#pragma once

#include <string>
#include <functional>

#include "core/DeviceModel.h"

// 封装 libimobiledevice：测试连接 + 备份。
// 当未编译 libimobiledevice 支持时，备份接口会返回 Unsupported。
class IosBackupService {
public:
    // 测试连接：根据 UDID 尝试连接 iOS 设备并拉取基础信息。
    // 成功返回 true，并在 outInfo 中填充 type=iOS、uid、manufacturer、deviceName、productType、osVersion 等。
    // 失败返回 false，并在 errMsg 中写入人类可读的错误原因。
    bool TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg);

    // 备份选项
    struct BackupOptions {
        std::string backupDir;    // 备份根目录，例如 D:\\Backups\\iPhone_2025_11_15
        bool fullBackup = true;   // 当前仅支持全量备份
        bool encrypt = false;     // 本阶段不支持加密备份
    };

    // 备份结果代码
    enum class BackupResultCode {
        Ok,
        NoDevice,
        ConnectionError,
        Mobilebackup2Error,
        IOError,
        Unsupported,
        Unknown
    };

    // 备份结果
    struct BackupResult {
        BackupResultCode code{BackupResultCode::Unknown};
        std::string message;      // 友好错误信息或成功描述
    };

    // 执行整机备份（最小可用版本）。
    // onProgress: 进度回调 [0.0,1.0]，消息如 "Preparing" / "Running idevicebackup2" / "Finished"。
    BackupResult PerformBackup(const std::string& udid,
                               const BackupOptions& opt,
                               std::function<void(double,const std::string&)> onProgress);
};

