#include "ui/CliMenu.h"

#include <iostream>

void CliMenu::printMenu(bool adbRunning) {
    std::cout << "\n=== DeviceWatcher Menu ===\n";
    std::cout << "1) " << (adbRunning ? "停止 Android 监听" : "启动 Android 监听") << "\n";
    std::cout << "q) 退出" << std::endl;
}

int CliMenu::run() {
    printMenu(adbRunning_);
    char c = 0;
    while (std::cin >> c) {
        if (c == 'q' || c == 'Q') {
            if (adbRunning_) {
                adb_.stop();
                adbRunning_ = false;
            }
            break;
        }
        if (c == '1') {
            if (adbRunning_) {
                adb_.stop();
                adbRunning_ = false;
                std::cout << "Android 监听已停止" << std::endl;
            } else {
                adb_.start();
                adbRunning_ = true;
                std::cout << "Android 监听已启动" << std::endl;
            }
            printMenu(adbRunning_);
        }
    }
    return 0;
}
