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
        } else {
            std::cout << "无效选项: " << cmd << std::endl;
        }
        printMenu(realtimePrintFlag_);
    }
    return 0;
}
