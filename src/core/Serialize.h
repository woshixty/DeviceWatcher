#pragma once

#include <string>
#include <vector>

#include "core/DeviceManager.h"

namespace Serialize {

// Write devices as JSON array with fields:
// type, uid, manufacturer, model, osVersion, abi, online
bool writeDevicesJson(const std::string& path, const DeviceManager::Snapshot& list);

// Write devices as CSV with header.
bool writeDevicesCsv(const std::string& path, const DeviceManager::Snapshot& list);

} // namespace Serialize

