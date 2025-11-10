#pragma once

#include <string>

// Device platform/type
enum class Type {
    Android,
    iOS,
    Unknown
};

struct DeviceInfo {
    Type type{Type::Unknown};    // platform
    std::string uid;             // unique identifier per device (e.g., serial)
    std::string displayName;     // user-friendly name
    bool online{false};          // online/offline state

    // Android extras (optional for other platforms)
    std::string model;           // e.g., "Pixel 7"
    std::string adbState;        // e.g., "device", "offline", "unauthorized"
};

struct DeviceEvent {
    enum class Kind { Attach, Detach, InfoUpdated };
    Kind kind{Kind::InfoUpdated};
    DeviceInfo info; // current info snapshot for the device related to the event
};
