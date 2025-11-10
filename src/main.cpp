// DeviceWatcher main entry

#include <iostream>
#include <string>

#include <fmt/format.h>

#include "ui/CliMenu.h"
#include "core/DeviceManager.h"
#include "providers/AndroidAdbProvider.h"
#include "core/Utils.h"

#ifndef DEVICEWATCHER_VERSION
#define DEVICEWATCHER_VERSION "0.0.0"
#endif

static void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [--help] [--version]\n";
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "DeviceWatcher " << DEVICEWATCHER_VERSION << "\n";
            return 0;
        }
    }

    fmt::print("DeviceWatcher started\n");

    DeviceManager manager;
    // Subscribe printer
    manager.subscribe([](const DeviceEvent& evt) {
        std::string hhmmss;
        auto ts = Utils::currentTimestamp();
        if (ts.size() >= 19) hhmmss = ts.substr(11, 8); else hhmmss = ts;

        auto kindToStr = [](DeviceEvent::Kind k) {
            switch (k) {
                case DeviceEvent::Kind::Attach: return "ATTACH";
                case DeviceEvent::Kind::Detach: return "DETACH";
                case DeviceEvent::Kind::InfoUpdated: return "INFOUPDATED";
            }
            return "INFO";
        };
        auto typeToStr = [](Type t) {
            switch (t) {
                case Type::Android: return "ANDROID";
                case Type::iOS: return "IOS";
                default: return "UNKNOWN";
            }
        };

        const auto& di = evt.info;
        fmt::print("[{}] {} {} SN={} model={} state={}\n",
                   hhmmss, kindToStr(evt.kind), typeToStr(di.type), di.uid, di.model, di.adbState);
    });

    AndroidAdbProvider adb(manager);
    CliMenu menu(manager, adb);
    return menu.run();
}
