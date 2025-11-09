#pragma once

#include <string>

class IosUsbmuxProvider {
public:
    IosUsbmuxProvider() = default;
    ~IosUsbmuxProvider() = default;

    void start();
    void stop();

    std::string name() const { return "IosUsbmuxProvider"; }
};

