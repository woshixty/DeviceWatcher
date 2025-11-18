// CLI menu: now also configures external notifications (webhook / local TCP)
#include "ui/CliMenu.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include "core/Utils.h"
#include "core/DeviceModel.h"
#include "core/Serialize.h"
#include "core/IosBackupService.h"

using std::string;

static const char* typeToStr(Type t) {
    switch (t) {
        case Type::Android: return "ANDROID";
        case Type::iOS: return "IOS";
        default: return "UNKNOWN";
    }
}

void CliMenu::printMenu(bool realtimeOn) {
    std::cout << "\n=== DeviceWatcher 菜单 ===\n";
    std::cout << "[1] 实时监视 " << (realtimeOn ? "开" : "关") << "（默认开）\n";
    std::cout << "[2] 当前设备列表（表格：uid type model osVersion onlineSince）\n";
    std::cout << "[3] 查看设备详情（输入uid）\n";
    std::cout << "[4] 导出设备清单 JSON 到 ./out/devices.json\n";
    std::cout << "[5] 导出设备清单 CSV 到 ./out/devices.csv\n";
    std::cout << "[6] " << (ios_.isRunning() ? "停止 iOS 监听" : "启动 iOS 监听")
              << (ios_.isSupported() ? "" : "（未编译支持）") << "\n";
    std::cout << "[7] 设置外部通知（webhook / 本地TCP）\n";
    std::cout << "[B] 测试 iOS 设备连接\n";
    std::cout << "[P] iOS 备份\n";
    std::cout << "[9] 退出\n";
}

void CliMenu::listDevices() {
    auto list = manager_.snapshot();
    if (list.empty()) {
        std::cout << "当前无设备" << std::endl;
        return;
    }

    fmt::print("\n{:<24} {:<8} {:<24} {:<10} {:<12}\n", "uid", "type", "model", "osVersion", "onlineSince");
    for (const auto& d : list) {
        auto since = manager_.onlineSince(d.uid);
        std::string sinceStr = since.has_value() ? Utils::formatTimeHHMMSS(*since) : "-";
        fmt::print("{:<24} {:<8} {:<24} {:<10} {:<12}\n",
                   d.uid,
                   typeToStr(d.type),
                   d.model,
                   d.osVersion,
                   sinceStr);
    }
}

void CliMenu::showDeviceDetails() {
    std::cout << "请输入设备 UID: ";
    std::string uid;
    if (!(std::cin >> uid)) return;

    auto list = manager_.snapshot();
    auto it = std::find_if(list.begin(), list.end(), [&](const DeviceInfo& i){ return i.uid == uid; });
    if (it == list.end()) {
        std::cout << "未找到 UID 对应的设备: " << uid << std::endl;
        return;
    }
    const auto& d = *it;
    auto since = manager_.onlineSince(d.uid);
    std::string sinceStr = since.has_value() ? Utils::formatTimeHHMMSS(*since) : "-";

    std::cout << "\n=== 设备详情 ===\n";
    fmt::print("type: {}\n", typeToStr(d.type));
    fmt::print("uid: {}\n", d.uid);
    fmt::print("displayName: {}\n", d.displayName);
    fmt::print("manufacturer: {}\n", d.manufacturer);
    fmt::print("model: {}\n", d.model);
    fmt::print("osVersion: {}\n", d.osVersion);
    fmt::print("abi: {}\n", d.abi);
    fmt::print("adbState: {}\n", d.adbState);
    fmt::print("online: {}\n", d.online ? "true" : "false");
    if (d.vid || d.pid) {
        fmt::print("vid: 0x{:04x}\n", (unsigned)d.vid);
        fmt::print("pid: 0x{:04x}\n", (unsigned)d.pid);
    }
    if (!d.usbPath.empty()) {
        fmt::print("usbPath: {}\n", d.usbPath);
    }
    fmt::print("onlineSince: {}\n", sinceStr);
}

void CliMenu::exportJson() {
    auto list = manager_.snapshot();
    const std::string path = "./out/devices.json";
    if (Serialize::writeDevicesJson(path, list)) {
        std::cout << "已导出 JSON: " << path << std::endl;
    } else {
        std::cout << "导出 JSON 失败: " << path << std::endl;
    }
}

void CliMenu::exportCsv() {
    auto list = manager_.snapshot();
    const std::string path = "./out/devices.csv";
    if (Serialize::writeDevicesCsv(path, list)) {
        std::cout << "已导出 CSV: " << path << std::endl;
    } else {
        std::cout << "导出 CSV 失败: " << path << std::endl;
    }
}

void CliMenu::toggleIos() {
    if (!ios_.isSupported()) {
        std::cout << "当前构建未启用 libimobiledevice/usbmuxd 支持（-DWITH_LIBIMOBILEDEVICE=ON）" << std::endl;
        return;
    }
    if (ios_.isRunning()) {
        ios_.stop();
        std::cout << "iOS 监听已停止" << std::endl;
    } else {
        ios_.start();
        std::cout << "iOS 监听已启动" << std::endl;
    }
}

void CliMenu::configureNotifications() {
    auto cfg = notifier_.currentSettings();
    std::cout << "\n=== 外部通知设置 ===\n";
    std::cout << "当前 webhookUrl: "
              << (cfg.webhookUrl.empty() ? "<空>" : cfg.webhookUrl) << "\n";
    std::cout << "当前 localTcpEndpoint: "
              << (cfg.localTcpEndpoint.empty() ? "<空>" : cfg.localTcpEndpoint) << "\n";

    std::cout << "输入新的 webhookUrl (直接回车保持不变, 输入 - 清空): ";
    std::string url;
    std::getline(std::cin >> std::ws, url);
    if (!url.empty()) {
        if (url == "-") url.clear();
        notifier_.setWebhookUrl(url);
    }

    std::cout << "输入新的本地 TCP 端点 (如 127.0.0.1:9009, 直接回车保持不变, 输入 - 清空): ";
    std::string ep;
    std::getline(std::cin, ep);
    if (!ep.empty()) {
        if (ep == "-") ep.clear();
        notifier_.setLocalTcpEndpoint(ep);
    }

    cfg = notifier_.currentSettings();
    std::cout << "已更新外部通知: webhookUrl="
              << (cfg.webhookUrl.empty() ? "<空>" : cfg.webhookUrl)
              << " localTcpEndpoint="
              << (cfg.localTcpEndpoint.empty() ? "<空>" : cfg.localTcpEndpoint)
              << "\n";
}

void CliMenu::testIosConnection() {
    std::cout << "请输入 iOS 设备 UDID: ";
    std::string udid;
    if (!(std::cin >> udid)) return;

    IosBackupService svc;
    DeviceInfo info;
    std::string err;
    if (!svc.TestConnection(udid, &info, err)) {
        std::cout << "测试连接失败: " << err << std::endl;
        return;
    }

    std::cout << "\n=== iOS 连接测试成功 ===\n";
    fmt::print("uid: {}\n", info.uid);
    fmt::print("deviceName: {}\n", info.deviceName);
    fmt::print("productType: {}\n", info.productType);
    fmt::print("osVersion: {}\n", info.osVersion);
    fmt::print("manufacturer: {}\n", info.manufacturer);
}

void CliMenu::iosBackup() {
    // 1. 收集当前在线的 iOS 设备
    auto all = manager_.snapshot();
    std::vector<DeviceInfo> iosList;
    for (const auto& d : all) {
        if (d.type == Type::iOS && d.online) {
            iosList.push_back(d);
        }
    }
    if (iosList.empty()) {
        std::cout << "当前没有在线的 iOS 设备" << std::endl;
        return;
    }

    // 2. 展示设备列表
    std::cout << "\n=== 可用的 iOS 设备 ===\n";
    for (size_t i = 0; i < iosList.size(); ++i) {
        const auto& d = iosList[i];
        fmt::print("[{}] uid={} name={} type={} os={}\n",
                   i + 1,
                   d.uid,
                   d.displayName.empty() ? d.deviceName : d.displayName,
                   d.productType.empty() ? d.model : d.productType,
                   d.osVersion);
    }

    // 3. 选择设备（编号或直接输入 UDID）
    std::cout << "请选择设备编号或直接输入 UDID（回车取消）: ";
    std::string sel;
    std::getline(std::cin >> std::ws, sel);
    if (sel.empty()) return;

    std::string udid;
    bool isIndex = !sel.empty() && std::all_of(sel.begin(), sel.end(), ::isdigit);
    if (isIndex) {
        size_t idx = std::stoul(sel);
        if (idx == 0 || idx > iosList.size()) {
            std::cout << "无效编号: " << sel << std::endl;
            return;
        }
        udid = iosList[idx - 1].uid;
    } else {
        udid = sel;
    }

    // 4. 输入备份目录
    std::cout << "请输入备份目录路径（例如 D:\\Backups\\iPhone_2025_11_15）: ";
    std::string backupDir;
    std::getline(std::cin, backupDir);
    if (backupDir.empty()) {
        std::cout << "备份目录不能为空" << std::endl;
        return;
    }

    IosBackupService::BackupOptions opt;
    opt.backupDir = backupDir;
    opt.fullBackup = true;
    opt.encrypt = false;

    IosBackupService svc;

    auto progressCb = [](double ratio, const std::string& msg) {
        int pct = static_cast<int>(ratio * 100.0 + 0.5);
        fmt::print("[iOS Backup] {:3d}% {}\n", pct, msg);
    };

    std::cout << "开始备份 iOS 设备: " << udid << std::endl;
    auto result = svc.PerformBackup(udid, opt, progressCb);

    fmt::print("备份结果: code={} message={}\n",
               static_cast<int>(result.code),
               result.message);
}

int CliMenu::run() {
    printMenu(realtimePrintFlag_);
    std::string cmd;
    while (true) {
        std::cout << "> ";
        if (!(std::cin >> cmd)) break;
        if (cmd == "9" || cmd == "q" || cmd == "Q") {
            break;
        }
        if (cmd == "1") {
            realtimePrintFlag_ = !realtimePrintFlag_;
            std::cout << "实时监视已" << (realtimePrintFlag_ ? "开启" : "关闭") << std::endl;
        } else if (cmd == "2") {
            listDevices();
        } else if (cmd == "3") {
            showDeviceDetails();
        } else if (cmd == "4") {
            exportJson();
        } else if (cmd == "5") {
            exportCsv();
        } else if (cmd == "6") {
            toggleIos();
        } else if (cmd == "7") {
            configureNotifications();
        } else if (cmd == "B" || cmd == "b") {
            testIosConnection();
        } else if (cmd == "P" || cmd == "p") {
            iosBackup();
        } else {
            std::cout << "无效选项: " << cmd << std::endl;
        }
        printMenu(realtimePrintFlag_);
    }
    return 0;
}
