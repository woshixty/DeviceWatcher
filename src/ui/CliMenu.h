#pragma once

#include "core/DeviceManager.h"
#include "providers/AndroidAdbProvider.h"

class CliMenu {
public:
    CliMenu(DeviceManager& manager, AndroidAdbProvider& adb)
        : manager_(manager), adb_(adb) {}

    int run(); // returns exit code

private:
    void printMenu(bool adbRunning);

    DeviceManager& manager_;
    AndroidAdbProvider& adb_;
    bool adbRunning_{false};
};
