#pragma once

#include "core/DeviceManager.h"

class CliMenu {
public:
    CliMenu(DeviceManager& manager, bool& realtimePrintFlag)
        : manager_(manager), realtimePrintFlag_(realtimePrintFlag) {}

    int run(); // returns exit code

private:
    void printMenu(bool realtimeOn);
    void listDevices();
    void showDeviceDetails();
    void exportJson();
    void exportCsv();

    DeviceManager& manager_;
    bool& realtimePrintFlag_;
};
