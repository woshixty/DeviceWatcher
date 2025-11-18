#pragma once

#include <string>

#include "core/DeviceModel.h"

// Wrapper around libimobiledevice for testing iOS connectivity by UDID.
// When libimobiledevice support is not compiled in, behaves as a stub
// and always returns false with a descriptive error message.
class IosBackupService {
public:
    // Test connection to an iOS device by UDID.
    // On success, fills outInfo (if non-null) with basic fields:
    // type=iOS, uid, manufacturer=Apple, deviceName, productType, osVersion.
    // Returns true on success; on failure returns false and sets errMsg.
    bool TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg);
};

