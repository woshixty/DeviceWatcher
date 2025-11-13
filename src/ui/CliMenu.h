#pragma once

#include "core/DeviceManager.h"
#include "providers/IosUsbmuxProvider.h"

class CliMenu {
public:
    CliMenu(DeviceManager& manager, bool& realtimePrintFlag, IosUsbmuxProvider& ios)
        : manager_(manager), realtimePrintFlag_(realtimePrintFlag), ios_(ios) {}

    int run(); // returns exit code

private:
    void printMenu(bool realtimeOn);
    void listDevices();
    void showDeviceDetails();
    void exportJson();
    void exportCsv();
    void toggleIos();

    DeviceManager& manager_;
    bool& realtimePrintFlag_;
    IosUsbmuxProvider& ios_;
};
