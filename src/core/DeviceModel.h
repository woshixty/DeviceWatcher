#pragma once

#include <string>

struct DeviceInfo {
    std::string type;         // e.g. "android", "ios", "usb"
    std::string uid;          // unique identifier per device
    std::string displayName;  // user-friendly name
    bool online{false};       // online/offline state
};

struct DeviceEvent {
    std::string type;         // semantic event type, e.g. "added", "removed", "updated"
    std::string uid;          // device UID concerned by the event
    std::string displayName;  // optional, may mirror DeviceInfo
    bool online{false};
};

