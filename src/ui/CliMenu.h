#pragma once

#include "core/DeviceManager.h"
#include "core/ExternalNotifier.h"
#include "providers/IosUsbmuxProvider.h"

class CliMenu {
public:
    CliMenu(DeviceManager& manager, bool& realtimePrintFlag, IosUsbmuxProvider& ios, ExternalNotifier& notifier)
        : manager_(manager), realtimePrintFlag_(realtimePrintFlag), ios_(ios), notifier_(notifier) {}

    int run(); // returns exit code

private:
    void printMenu(bool realtimeOn);
    void listDevices();
    void showDeviceDetails();
    void exportJson();
    void exportCsv();
    void toggleIos();
    void configureNotifications();
    void testIosConnection();

    DeviceManager& manager_;
    bool& realtimePrintFlag_;
    IosUsbmuxProvider& ios_;
    ExternalNotifier& notifier_;
};
