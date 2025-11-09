#pragma once

#include <string>

class UsbProvider {
public:
    UsbProvider() = default;
    ~UsbProvider() = default;

    void start();
    void stop();

    std::string name() const { return "UsbProvider"; }
};

