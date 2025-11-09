// DeviceWatcher main entry

#include <iostream>
#include <string>

#include <fmt/core.h>

#include "ui/CliMenu.h"

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

    CliMenu menu;
    return menu.run();
}
