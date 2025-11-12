#include "core/Serialize.h"

#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using nlohmann::json;

namespace {
const char* typeToStr(Type t) {
    switch (t) {
        case Type::Android: return "ANDROID";
        case Type::iOS: return "IOS";
        default: return "UNKNOWN";
    }
}

static std::string csvEscape(const std::string& s) {
    bool needQuotes = s.find_first_of(",""\n\r") != std::string::npos;
    if (!needQuotes) return s;
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}
}

namespace Serialize {

bool writeDevicesJson(const std::string& path, const DeviceManager::Snapshot& list) {
    try {
        std::filesystem::path p(path);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }
        json arr = json::array();
        for (const auto& d : list) {
            json o;
            o["type"] = typeToStr(d.type);
            o["uid"] = d.uid;
            o["manufacturer"] = d.manufacturer;
            o["model"] = d.model;
            o["osVersion"] = d.osVersion;
            o["abi"] = d.abi;
            o["online"] = d.online;
            arr.push_back(std::move(o));
        }
        std::ofstream ofs(path, std::ios::binary);
        ofs << arr.dump(2);
        ofs.close();
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("writeDevicesJson failed: {}", ex.what());
        return false;
    }
}

bool writeDevicesCsv(const std::string& path, const DeviceManager::Snapshot& list) {
    try {
        std::filesystem::path p(path);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream ofs(path, std::ios::binary);
        ofs << "type,uid,manufacturer,model,osVersion,abi,online\n";
        for (const auto& d : list) {
            ofs << csvEscape(typeToStr(d.type)) << ','
                << csvEscape(d.uid) << ','
                << csvEscape(d.manufacturer) << ','
                << csvEscape(d.model) << ','
                << csvEscape(d.osVersion) << ','
                << csvEscape(d.abi) << ','
                << (d.online ? "true" : "false")
                << "\n";
        }
        ofs.close();
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("writeDevicesCsv failed: {}", ex.what());
        return false;
    }
}

} // namespace Serialize

