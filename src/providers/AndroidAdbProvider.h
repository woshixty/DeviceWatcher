#pragma once

#include <string>

class AndroidAdbProvider {
public:
    AndroidAdbProvider() = default;
    ~AndroidAdbProvider() = default;

    void start();
    void stop();

    std::string name() const { return "AndroidAdbProvider"; }
};

