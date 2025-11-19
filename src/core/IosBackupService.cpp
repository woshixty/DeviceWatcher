#include "core/IosBackupService.h"

#include <spdlog/spdlog.h>

#if WITH_LIBIMOBILEDEVICE

// 真正实现：使用 libimobiledevice / lockdownd / mobilebackup2。
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/mobilebackup2.h>
#include <plist/plist.h>

#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <vector>
#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <chrono>

namespace {

// MobileBackup2 原始协议中的数据块标记常量（参考 idevicebackup2，但不直接复用实现）
constexpr uint8_t kMb2CodeSuccess     = 0x00;
constexpr uint8_t kMb2CodeErrorLocal  = 0x06;
constexpr uint8_t kMb2CodeErrorRemote = 0x0b;
constexpr uint8_t kMb2CodeFileData    = 0x0c;

// 将网络字节序的大端 32 位整数转为主机字节序
uint32_t from_be32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x000000FFu) << 24);
}

// 从 mobilebackup2 流中读取指定长度的数据，直到读满或出错。
bool mb2_read_exact(mobilebackup2_client_t client, char* buf, uint32_t length) {
    uint32_t total = 0;
    while (total < length) {
        uint32_t chunk = 0;
        mobilebackup2_error_t err = mobilebackup2_receive_raw(
            client,
            buf + total,
            length - total,
            &chunk);
        if (err != MOBILEBACKUP2_E_SUCCESS) {
            spdlog::warn("[IosBackup] mobilebackup2_receive_raw error={}, total={} len={}",
                         (int)err, total, length);
            return false;
        }
        if (chunk == 0) {
            // 设备暂时没有数据，继续等待。
            continue;
        }
        total += chunk;
    }
    return true;
}

// 读取一个大端的 32 位无符号整数。
bool mb2_read_u32(mobilebackup2_client_t client, uint32_t& out) {
    uint32_t raw = 0;
    uint32_t bytes = 0;
    while (true) {
        mobilebackup2_error_t err = mobilebackup2_receive_raw(
            client,
            reinterpret_cast<char*>(&raw),
            sizeof(raw),
            &bytes);
        if (err != MOBILEBACKUP2_E_SUCCESS) {
            spdlog::warn("[IosBackup] mobilebackup2_receive_raw (u32) err={}", (int)err);
            return false;
        }
        if (bytes == 0) {
            // 暂无数据，继续等待
            continue;
        }
        if (bytes != sizeof(raw)) {
            spdlog::warn("[IosBackup] mobilebackup2_receive_raw (u32) partial={} expected=4", bytes);
            return false;
        }
        out = from_be32(raw);
        return true;
    }
}

// 读取一个 UTF-8 文件名（长度前缀 + 字节数组）。
// endOfList=true 表示遇到长度为 0 的结束标记。
bool mb2_read_filename(mobilebackup2_client_t client,
                       std::string& name,
                       bool& endOfList)
{
    endOfList = false;
    name.clear();

    uint32_t len = 0;
    if (!mb2_read_u32(client, len)) {
        return false;
    }

    if (len == 0) {
        // 0 长度代表没有更多文件
        endOfList = true;
        return true;
    }
    if (len > 4096) {
        spdlog::warn("[IosBackup] filename length too large: {}", len);
        return false;
    }

    std::string buf;
    buf.resize(len);
    if (!mb2_read_exact(client, buf.data(), len)) {
        return false;
    }

    name.assign(buf.data(), buf.size());
    return true;
}

// 简单的 errno -> 设备错误码映射（对协议来说非必须，这里只区分常见几类）
int errno_to_device_error_simple(int e) {
    switch (e) {
        case ENOENT: return -6;
        case EEXIST: return -7;
        case ENOTDIR: return -8;
        case EISDIR: return -9;
        case ENOSPC: return -15;
        default: return -1;
    }
}

// 处理 DLMessageUploadFiles：设备往主机发送文件（备份核心路径）。
// 返回成功接收的文件数量；如发生严重 IO 或协议错误返回 -1。
int mb2_handle_upload_files(mobilebackup2_client_t client,
                            plist_t message,
                            const std::string& backupDir,
                            const std::function<void(double,const std::string&)>& onProgress)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 1) {
        spdlog::warn("[IosBackup] malformed DLMessageUploadFiles plist");
        return -1;
    }

    // 设备在 message[3] 上报整体进度（0.0~100.0），有些版本可能用 uint 存总大小，
    // 这里只尝试读取 real 类型做一个粗略进度。
    plist_t progressNode = nullptr;
    if (plist_array_get_size(message) >= 4) {
        progressNode = plist_array_get_item(message, 3);
        if (progressNode && plist_get_node_type(progressNode) == PLIST_REAL) {
            double percent = 0.0;
            plist_get_real_val(progressNode, &percent);
            if (percent >= 0.0 && percent <= 100.0) {
                onProgress(percent / 100.0, "Receiving files");
            }
        }
    }

    int errCode = 0;
    const char* errDesc = nullptr;
    unsigned int fileCount = 0;

    std::filesystem::path rootPath(backupDir);

    while (true) {
        // 顺序读取：domain(未用) + 相对文件路径
        std::string domain;
        std::string relPath;
        bool endOfList = false;

        if (!mb2_read_filename(client, domain, endOfList)) {
            spdlog::warn("[IosBackup] failed to read domain while receiving files");
            errCode = errno_to_device_error_simple(EIO);
            errDesc = "Failed to read filename (domain)";
            break;
        }
        if (endOfList) {
            // 没有更多文件，结束循环
            break;
        }

        if (!mb2_read_filename(client, relPath, endOfList) || endOfList) {
            spdlog::warn("[IosBackup] failed to read filename while receiving files");
            errCode = errno_to_device_error_simple(EIO);
            errDesc = "Failed to read filename (path)";
            break;
        }

        std::filesystem::path destPath = rootPath / std::filesystem::path(relPath).lexically_normal();
        std::error_code ec;
        auto parent = destPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                spdlog::error("[IosBackup] create_directories failed: {} ({})", ec.message(), ec.value());
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to create destination directory";
                break;
            }
        }

        // 读取第一个块的长度与标记
        uint32_t nlen = 0;
        if (!mb2_read_u32(client, nlen)) {
            spdlog::warn("[IosBackup] failed to read block length");
            errCode = errno_to_device_error_simple(EIO);
            errDesc = "Failed to read data block length";
            break;
        }

        uint8_t code = 0;
        if (!mb2_read_exact(client, reinterpret_cast<char*>(&code), 1)) {
            spdlog::warn("[IosBackup] failed to read block code");
            errCode = errno_to_device_error_simple(EIO);
            errDesc = "Failed to read data block code";
            break;
        }

        if (code != kMb2CodeFileData && code != kMb2CodeSuccess && code != kMb2CodeErrorRemote) {
            spdlog::info("[IosBackup] unknown file data flag: 0x{:02x}", (unsigned)code);
        }

        // 打开输出文件
        std::FILE* fp = std::fopen(destPath.string().c_str(), "wb");
        if (!fp) {
            int e = errno;
            spdlog::error("[IosBackup] fopen failed for {}: {} ({})",
                          destPath.string(), std::strerror(e), e);
            errCode = errno_to_device_error_simple(e);
            errDesc = std::strerror(e);
            break;
        }

        std::vector<char> buffer(32768);

        // 持续读取直到该文件结束（nlen==0）或出现错误
        while (code == kMb2CodeFileData) {
            if (nlen == 0) {
                break;
            }
            uint32_t blockSize = nlen > 0 ? (nlen - 1) : 0;
            uint32_t remaining = blockSize;
            while (remaining > 0) {
                uint32_t chunk = std::min<uint32_t>(remaining, static_cast<uint32_t>(buffer.size()));
                if (!mb2_read_exact(client, buffer.data(), chunk)) {
                    spdlog::warn("[IosBackup] failed while reading file data chunk");
                    std::fclose(fp);
                    errCode = errno_to_device_error_simple(EIO);
                    errDesc = "Failed while reading file data";
                    goto upload_files_end;
                }
                if (std::fwrite(buffer.data(), 1, chunk, fp) != chunk) {
                    int e = errno;
                    spdlog::error("[IosBackup] fwrite failed for {}: {} ({})",
                                  destPath.string(), std::strerror(e), e);
                    std::fclose(fp);
                    errCode = errno_to_device_error_simple(e);
                    errDesc = std::strerror(e);
                    goto upload_files_end;
                }
                remaining -= chunk;
            }

            // 读下一块的头
            if (!mb2_read_u32(client, nlen)) {
                spdlog::warn("[IosBackup] failed to read next block length");
                std::fclose(fp);
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to read next block length";
                goto upload_files_end;
            }
            if (nlen == 0) {
                break;
            }
            if (!mb2_read_exact(client, reinterpret_cast<char*>(&code), 1)) {
                spdlog::warn("[IosBackup] failed to read next block code");
                std::fclose(fp);
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to read next block code";
                goto upload_files_end;
            }

            // 简单更新一下进度
            onProgress(0.1, "Receiving file " + relPath);
        }

        std::fclose(fp);
        ++fileCount;
    }

upload_files_end:
    {
        // 向设备返回本次操作的状态
        plist_t status = plist_new_dict();
        mobilebackup2_send_status_response(client, errCode, errDesc, status);
        plist_free(status);
    }

    if (errCode != 0) {
        return -1;
    }
    return static_cast<int>(fileCount);
}

// 处理 DLMessageGetFreeDiskSpace：设备询问主机可用磁盘空间。
void mb2_handle_get_free_space(mobilebackup2_client_t client,
                               const std::string& backupDir)
{
    uint64_t freeBytes = 0;
    int status = 0;
    std::error_code ec;
    auto info = std::filesystem::space(backupDir, ec);
    if (!ec) {
        freeBytes = info.available;
    } else {
        status = errno_to_device_error_simple(EIO);
        spdlog::warn("[IosBackup] std::filesystem::space failed: {}", ec.message());
    }

    plist_t payload = plist_new_uint(freeBytes);
    mobilebackup2_send_status_response(client, status, nullptr, payload);
    plist_free(payload);
}

// 处理 DLContentsOfDirectory：列出目录内容。
void mb2_handle_list_directory(mobilebackup2_client_t client,
                               plist_t message,
                               const std::string& backupDir)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 2) {
        spdlog::warn("[IosBackup] malformed DLContentsOfDirectory message");
        return;
    }

    plist_t node = plist_array_get_item(message, 1);
    if (!node || plist_get_node_type(node) != PLIST_STRING) {
        spdlog::warn("[IosBackup] DLContentsOfDirectory missing path string");
        return;
    }

    char* rel = nullptr;
    plist_get_string_val(node, &rel);
    if (!rel) {
        spdlog::warn("[IosBackup] DLContentsOfDirectory empty path");
        return;
    }

    std::filesystem::path dirPath = std::filesystem::path(backupDir) /
                                    std::filesystem::path(rel).lexically_normal();
    free(rel);

    plist_t dirList = plist_new_dict();

    std::error_code ec;
    if (std::filesystem::exists(dirPath, ec) && std::filesystem::is_directory(dirPath, ec)) {
        for (auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (ec) break;
            std::string name = entry.path().filename().string();
            plist_t info = plist_new_dict();
            const char* typeStr = "DLFileTypeUnknown";
            if (entry.is_directory()) {
                typeStr = "DLFileTypeDirectory";
            } else if (entry.is_regular_file()) {
                typeStr = "DLFileTypeRegular";
                auto sz = entry.file_size(ec);
                if (!ec) {
                    plist_dict_set_item(info, "DLFileSize", plist_new_uint(sz));
                }
            }
            plist_dict_set_item(info, "DLFileType", plist_new_string(typeStr));
            // modification date 可选，这里简单用 UNIX 时间戳
            auto ftime = entry.last_write_time(ec);
            if (!ec) {
                auto s = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
                auto ts = s.time_since_epoch().count();
                plist_dict_set_item(info, "DLFileModificationDate", plist_new_unix_date(ts));
            }
            plist_dict_set_item(dirList, name.c_str(), info);
        }
    }

    mobilebackup2_send_status_response(client, 0, nullptr, dirList);
    plist_free(dirList);
}

// 处理 DLMessageCreateDirectory：创建目录。
void mb2_handle_create_directory(mobilebackup2_client_t client,
                                 plist_t message,
                                 const std::string& backupDir)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 2) {
        spdlog::warn("[IosBackup] malformed DLMessageCreateDirectory");
        return;
    }

    plist_t dirNode = plist_array_get_item(message, 1);
    if (!dirNode || plist_get_node_type(dirNode) != PLIST_STRING) {
        spdlog::warn("[IosBackup] DLMessageCreateDirectory missing path string");
        return;
    }

    char* rel = nullptr;
    plist_get_string_val(dirNode, &rel);
    if (!rel) {
        spdlog::warn("[IosBackup] DLMessageCreateDirectory empty path");
        return;
    }

    int errCode = 0;
    const char* errDesc = nullptr;

    std::filesystem::path fullPath = std::filesystem::path(backupDir) /
                                     std::filesystem::path(rel).lexically_normal();
    free(rel);

    std::error_code ec;
    std::filesystem::create_directories(fullPath, ec);
    if (ec && ec.value() != EEXIST) {
        spdlog::error("[IosBackup] create_directories failed: {} ({})", ec.message(), ec.value());
        errCode = errno_to_device_error_simple(EIO);
        errDesc = "Failed to create directory";
    }

    mobilebackup2_send_status_response(client, errCode, errDesc, nullptr);
}

// 处理 DLMessageMoveFiles / DLMessageMoveItems：移动文件或目录。
void mb2_handle_move_items(mobilebackup2_client_t client,
                           plist_t message,
                           const std::string& backupDir)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 2) {
        spdlog::warn("[IosBackup] malformed DLMessageMoveFiles");
        return;
    }

    plist_t dict = plist_array_get_item(message, 1);
    if (!dict || plist_get_node_type(dict) != PLIST_DICT) {
        spdlog::warn("[IosBackup] DLMessageMoveFiles missing dict");
        return;
    }

    int errCode = 0;
    const char* errDesc = nullptr;

    plist_dict_iter iter = nullptr;
    plist_dict_new_iter(dict, &iter);
    if (!iter) {
        errCode = errno_to_device_error_simple(EIO);
        errDesc = "Failed to iterate move items";
    } else {
        char* key = nullptr;
        plist_t val = nullptr;
        while (errCode == 0) {
            plist_dict_next_item(dict, iter, &key, &val);
            if (!key || !val) {
                break;
            }
            if (plist_get_node_type(val) != PLIST_STRING) {
                free(key);
                continue;
            }
            char* dst = nullptr;
            plist_get_string_val(val, &dst);
            if (!dst) {
                free(key);
                continue;
            }

            std::filesystem::path srcPath = std::filesystem::path(backupDir) /
                                            std::filesystem::path(key).lexically_normal();
            std::filesystem::path dstPath = std::filesystem::path(backupDir) /
                                            std::filesystem::path(dst).lexically_normal();
            free(key);
            free(dst);

            std::error_code ec;
            if (std::filesystem::exists(dstPath, ec)) {
                std::filesystem::remove_all(dstPath, ec);
            }
            std::filesystem::create_directories(dstPath.parent_path(), ec);
            if (ec) {
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to prepare destination directory";
                break;
            }
            std::filesystem::rename(srcPath, dstPath, ec);
            if (ec) {
                spdlog::error("[IosBackup] rename failed: {}", ec.message());
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to move file";
                break;
            }
        }
        free(iter);
    }

    mobilebackup2_send_status_response(client, errCode, errDesc, nullptr);
}

// 处理 DLMessageRemoveFiles / DLMessageRemoveItems：删除文件或目录。
void mb2_handle_remove_items(mobilebackup2_client_t client,
                             plist_t message,
                             const std::string& backupDir)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 2) {
        spdlog::warn("[IosBackup] malformed DLMessageRemoveFiles");
        return;
    }

    plist_t arr = plist_array_get_item(message, 1);
    if (!arr || plist_get_node_type(arr) != PLIST_ARRAY) {
        spdlog::warn("[IosBackup] DLMessageRemoveFiles missing array");
        return;
    }

    uint32_t count = plist_array_get_size(arr);
    int errCode = 0;
    const char* errDesc = nullptr;

    for (uint32_t i = 0; i < count; ++i) {
        plist_t node = plist_array_get_item(arr, i);
        if (!node || plist_get_node_type(node) != PLIST_STRING) {
            continue;
        }
        char* rel = nullptr;
        plist_get_string_val(node, &rel);
        if (!rel) continue;

        std::filesystem::path fullPath = std::filesystem::path(backupDir) /
                                         std::filesystem::path(rel).lexically_normal();
        free(rel);

        std::error_code ec;
        if (std::filesystem::exists(fullPath, ec)) {
            if (std::filesystem::is_directory(fullPath, ec)) {
                std::filesystem::remove_all(fullPath, ec);
            } else {
                std::filesystem::remove(fullPath, ec);
            }
            if (ec) {
                spdlog::warn("[IosBackup] remove failed: {}", ec.message());
                errCode = errno_to_device_error_simple(EIO);
                errDesc = "Failed to remove item";
            }
        }
    }

    mobilebackup2_send_status_response(client, errCode, errDesc, nullptr);
}

// 处理 DLMessageCopyItem：拷贝文件或目录。
void mb2_handle_copy_item(mobilebackup2_client_t client,
                          plist_t message,
                          const std::string& backupDir)
{
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 3) {
        spdlog::warn("[IosBackup] malformed DLMessageCopyItem");
        return;
    }

    plist_t srcNode = plist_array_get_item(message, 1);
    plist_t dstNode = plist_array_get_item(message, 2);
    if (!srcNode || !dstNode ||
        plist_get_node_type(srcNode) != PLIST_STRING ||
        plist_get_node_type(dstNode) != PLIST_STRING) {
        spdlog::warn("[IosBackup] DLMessageCopyItem missing strings");
        return;
    }

    char* srcRel = nullptr;
    char* dstRel = nullptr;
    plist_get_string_val(srcNode, &srcRel);
    plist_get_string_val(dstNode, &dstRel);
    if (!srcRel || !dstRel) {
        free(srcRel);
        free(dstRel);
        return;
    }

    std::filesystem::path srcPath = std::filesystem::path(backupDir) /
                                    std::filesystem::path(srcRel).lexically_normal();
    std::filesystem::path dstPath = std::filesystem::path(backupDir) /
                                    std::filesystem::path(dstRel).lexically_normal();
    free(srcRel);
    free(dstRel);

    int errCode = 0;
    const char* errDesc = nullptr;
    std::error_code ec;

    if (std::filesystem::exists(srcPath, ec)) {
        std::filesystem::create_directories(dstPath.parent_path(), ec);
        if (!ec) {
            std::filesystem::copy(srcPath, dstPath,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  ec);
        }
        if (ec) {
            spdlog::warn("[IosBackup] copy failed: {}", ec.message());
            errCode = errno_to_device_error_simple(EIO);
            errDesc = "Failed to copy item";
        }
    }

    mobilebackup2_send_status_response(client, errCode, errDesc, nullptr);
}

// 解析 DLMessageProcessMessage 中的最终错误码（0 表示成功）。
int mb2_parse_operation_result(plist_t message, std::string& outDesc) {
    outDesc.clear();
    if (!message || plist_get_node_type(message) != PLIST_ARRAY ||
        plist_array_get_size(message) < 2) {
        return -1;
    }
    plist_t dict = plist_array_get_item(message, 1);
    if (!dict || plist_get_node_type(dict) != PLIST_DICT) {
        return -1;
    }
    int errorCode = -1;
    plist_t codeNode = plist_dict_get_item(dict, "ErrorCode");
    if (codeNode && plist_get_node_type(codeNode) == PLIST_UINT) {
        uint64_t v = 0;
        plist_get_uint_val(codeNode, &v);
        errorCode = static_cast<int>(v);
    }
    plist_t descNode = plist_dict_get_item(dict, "ErrorDescription");
    if (descNode && plist_get_node_type(descNode) == PLIST_STRING) {
        char* s = nullptr;
        plist_get_string_val(descNode, &s);
        if (s) {
            outDesc.assign(s);
            free(s);
        }
    }
    return errorCode;
}

} // namespace

// 测试连接
bool IosBackupService::TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg) {
    errMsg.clear();
    if (udid.empty()) {
        errMsg = "UDID 不能为空";
        return false;
    }

    idevice_t dev = nullptr;
    idevice_error_t derr = idevice_new(&dev, udid.c_str());
    if (derr != IDEVICE_E_SUCCESS || !dev) {
        spdlog::warn("[IosBackup] idevice_new failed for {} err={}", udid, static_cast<int>(derr));
        if (derr == IDEVICE_E_NO_DEVICE) {
            errMsg = "No device with UDID " + udid;
        } else {
            errMsg = "idevice_new error code " + std::to_string(static_cast<int>(derr));
        }
        return false;
    }

    lockdownd_client_t client = nullptr;
    lockdownd_error_t lerr = lockdownd_client_new_with_handshake(dev, &client, "DeviceWatcherBackup");
    if (lerr != LOCKDOWN_E_SUCCESS) {
        spdlog::warn("[IosBackup] lockdown handshake failed for {} err={}", udid, static_cast<int>(lerr));
        errMsg = "lockdownd handshake failed, error code " + std::to_string(static_cast<int>(lerr));
        idevice_free(dev);
        return false;
    }

    auto getStr = [&](const char* domain, const char* key) -> std::string {
        plist_t node = nullptr;
        std::string out;
        if (lockdownd_get_value(client, domain, key, &node) == LOCKDOWN_E_SUCCESS && node) {
            char* s = nullptr;
            if (plist_get_node_type(node) == PLIST_STRING) {
                plist_get_string_val(node, &s);
                if (s) {
                    out = s;
                    free(s);
                }
            }
            plist_free(node);
        }
        return out;
    };

    DeviceInfo info;
    info.type = Type::iOS;
    info.uid = udid;
    info.online = true;
    info.transport = "USB";
    info.manufacturer = "Apple";
    info.deviceName = getStr(nullptr, "DeviceName");
    info.productType = getStr(nullptr, "ProductType");
    info.osVersion  = getStr(nullptr, "ProductVersion");
    // 向后兼容字段
    info.displayName = info.deviceName;
    info.model       = info.productType;

    if (outInfo) {
        *outInfo = info;
    }

    lockdownd_client_free(client);
    idevice_free(dev);

    spdlog::info("[IosBackup] TestConnection success udid={} name={} type={} os={}",
                 udid, info.deviceName, info.productType, info.osVersion);
    return true;
}

IosBackupService::BackupResult IosBackupService::PerformBackup(
    const std::string& udid,
    const BackupOptions& opt,
    std::function<void(double,const std::string&)> onProgress)
{
    BackupResult res;
    if (!onProgress) onProgress = [](double, const std::string&) {};

    try {
        if (opt.backupDir.empty()) {
            res.code = BackupResultCode::IOError;
            res.message = "backupDir 不能为空";
            return res;
        }
        if (opt.encrypt) {
            res.code = BackupResultCode::Unsupported;
            res.message = "Encrypted backup not supported in this version.";
            return res;
        }

        // 确保目录存在
        std::filesystem::path backupPath(opt.backupDir);
        if (!backupPath.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(backupPath, ec);
            if (ec) {
                res.code = BackupResultCode::IOError;
                res.message = "创建备份目录失败: " + ec.message();
                return res;
            }
        }
    } catch (const std::exception& ex) {
        res.code = BackupResultCode::IOError;
        res.message = std::string("创建备份目录失败: ") + ex.what();
        return res;
    }

    onProgress(0.0, "Preparing");

    idevice_t dev = nullptr;
    idevice_error_t derr = idevice_new(&dev, udid.empty() ? nullptr : udid.c_str());
    if (derr != IDEVICE_E_SUCCESS || !dev) {
        spdlog::warn("[IosBackup] idevice_new failed for {} err={}", udid, (int)derr);
        if (derr == IDEVICE_E_NO_DEVICE) {
            res.code = BackupResultCode::NoDevice;
            res.message = "No device with UDID " + udid;
        } else {
            res.code = BackupResultCode::ConnectionError;
            res.message = "无法连接到设备，idevice_new error code " + std::to_string((int)derr);
        }
        return res;
    }

    lockdownd_client_t lockdown = nullptr;
    lockdownd_error_t lerr = lockdownd_client_new_with_handshake(
        dev, &lockdown, "DeviceWatcherBackup");
    if (lerr != LOCKDOWN_E_SUCCESS) {
        spdlog::warn("[IosBackup] lockdownd handshake failed for {} err={}", udid, (int)lerr);
        res.code = BackupResultCode::ConnectionError;
        res.message = "lockdownd 握手失败，错误码 " + std::to_string((int)lerr);
        idevice_free(dev);
        return res;
    }

    // 检查是否启用了加密备份（本阶段不支持）
    bool willEncrypt = false;
    plist_t encNode = nullptr;
    if (lockdownd_get_value(lockdown,
                            "com.apple.mobile.backup",
                            "WillEncrypt",
                            &encNode) == LOCKDOWN_E_SUCCESS &&
        encNode) {
        if (plist_get_node_type(encNode) == PLIST_BOOLEAN) {
            uint8_t b = 0;
            plist_get_bool_val(encNode, &b);
            willEncrypt = (b != 0);
        }
        plist_free(encNode);
    }
    if (willEncrypt) {
        lockdownd_client_free(lockdown);
        idevice_free(dev);
        onProgress(1.0, "Encrypted backup not supported");
        res.code = BackupResultCode::Unsupported;
        res.message = "Encrypted backup not supported in this version.";
        return res;
    }

    mobilebackup2_client_t mb2 = nullptr;
    mobilebackup2_error_t mberr = mobilebackup2_client_start_service(
        dev, &mb2, "DeviceWatcherBackup");
    if (mberr != MOBILEBACKUP2_E_SUCCESS || !mb2) {
        spdlog::warn("[IosBackup] mobilebackup2_client_start_service failed udid={} err={}", udid, (int)mberr);
        res.code = BackupResultCode::Mobilebackup2Error;
        res.message = "无法启动 mobilebackup2 会话，错误码 " + std::to_string((int)mberr);
        lockdownd_client_free(lockdown);
        idevice_free(dev);
        return res;
    }

    lockdownd_client_free(lockdown);
    lockdown = nullptr;

    // 简单执行一次版本交换，确保协议可用
    double local_versions[] = { 2.0, 2.1, 1.0 };
    double remote_version = 0.0;
    mberr = mobilebackup2_version_exchange(
        mb2,
        local_versions,
        (char)(sizeof(local_versions) / sizeof(local_versions[0])),
        &remote_version);
    if (mberr != MOBILEBACKUP2_E_SUCCESS) {
        spdlog::warn("[IosBackup] mobilebackup2_version_exchange failed err={}", (int)mberr);
        res.code = BackupResultCode::Mobilebackup2Error;
        res.message = "mobilebackup2 版本握手失败，错误码 " + std::to_string((int)mberr);
        mobilebackup2_client_free(mb2);
        idevice_free(dev);
        return res;
    }

    spdlog::info("[IosBackup] mobilebackup2 version exchange ok, remote={}", remote_version);

    onProgress(0.05, "Requesting backup");

    // 构造备份请求参数：当前仅支持强制全量备份。
    plist_t opts = plist_new_dict();
    if (opt.fullBackup) {
        plist_dict_set_item(opts, "ForceFullBackup", plist_new_bool(1));
    }

    mberr = mobilebackup2_send_request(
        mb2,
        "Backup",
        udid.c_str(),      // TargetIdentifier
        udid.c_str(),      // SourceIdentifier（仅做全量备份，直接使用相同 UDID）
        opts);
    plist_free(opts);
    if (mberr != MOBILEBACKUP2_E_SUCCESS) {
        spdlog::warn("[IosBackup] mobilebackup2_send_request Backup failed err={}", (int)mberr);
        mobilebackup2_client_free(mb2);
        idevice_free(dev);
        res.code = BackupResultCode::Mobilebackup2Error;
        if (mberr == MOBILEBACKUP2_E_BAD_VERSION) {
            res.message = "无法开始备份：mobilebackup2 协议版本不兼容";
        } else if (mberr == MOBILEBACKUP2_E_REPLY_NOT_OK) {
            res.message = "设备拒绝开始备份（可能未解锁或未信任）";
        } else {
            res.message = "无法开始备份，会话返回错误码 " + std::to_string((int)mberr);
        }
        return res;
    }

    onProgress(0.1, "Receiving backup data");

    bool operationOk = false;
    std::string opErrorDesc;

    // 处理来自设备的一系列 DLMessage* 消息直到结束。
    while (true) {
        plist_t message = nullptr;
        char* dlmsg = nullptr;
        mobilebackup2_error_t r = mobilebackup2_receive_message(mb2, &message, &dlmsg);
        if (r == MOBILEBACKUP2_E_RECEIVE_TIMEOUT) {
            // 暂时无数据，继续等待。
            if (message) plist_free(message);
            if (dlmsg) free(dlmsg);
            continue;
        }
        if (r != MOBILEBACKUP2_E_SUCCESS) {
            spdlog::error("[IosBackup] mobilebackup2_receive_message failed err={}", (int)r);
            if (message) plist_free(message);
            if (dlmsg) free(dlmsg);
            break;
        }

        std::string msgType = dlmsg ? dlmsg : "";
        free(dlmsg);
        dlmsg = nullptr;

        if (msgType == "DLMessageUploadFiles") {
            int count = mb2_handle_upload_files(mb2, message, opt.backupDir, onProgress);
            spdlog::info("[IosBackup] DLMessageUploadFiles count={}", count);
        } else if (msgType == "DLMessageGetFreeDiskSpace") {
            mb2_handle_get_free_space(mb2, opt.backupDir);
        } else if (msgType == "DLContentsOfDirectory") {
            mb2_handle_list_directory(mb2, message, opt.backupDir);
        } else if (msgType == "DLMessageCreateDirectory") {
            mb2_handle_create_directory(mb2, message, opt.backupDir);
        } else if (msgType == "DLMessageMoveFiles" || msgType == "DLMessageMoveItems") {
            mb2_handle_move_items(mb2, message, opt.backupDir);
        } else if (msgType == "DLMessageRemoveFiles" || msgType == "DLMessageRemoveItems") {
            mb2_handle_remove_items(mb2, message, opt.backupDir);
        } else if (msgType == "DLMessageCopyItem") {
            mb2_handle_copy_item(mb2, message, opt.backupDir);
        } else if (msgType == "DLMessageProcessMessage") {
            int code = mb2_parse_operation_result(message, opErrorDesc);
            if (code == 0) {
                operationOk = true;
            } else {
                operationOk = false;
            }
        } else if (msgType == "DLMessageDisconnect") {
            plist_free(message);
            break;
        } else {
            spdlog::warn("[IosBackup] unhandled dlmsg from device: {}", msgType);
        }

        plist_free(message);
    }

    mobilebackup2_client_free(mb2);
    idevice_free(dev);

    if (!operationOk) {
        onProgress(1.0, "Backup failed");
        res.code = BackupResultCode::Mobilebackup2Error;
        if (!opErrorDesc.empty()) {
            res.message = "备份失败: " + opErrorDesc;
        } else {
            res.message = "备份失败：mobilebackup2 会话未报告成功";
        }
        return res;
    }

    onProgress(1.0, "Backup finished");

    res.code = BackupResultCode::Ok;
    res.message = "备份完成: " + opt.backupDir;
    spdlog::info("[IosBackup] backup completed, dir={}", opt.backupDir);
    return res;
}

std::vector<IosBackupService::BackupRecord> IosBackupService::ListBackups(
    const std::string& rootDir,
    std::string& errMsg)
{
    errMsg.clear();
    std::vector<BackupRecord> records;

    namespace fs = std::filesystem;

    try {
        fs::path root(rootDir);
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            errMsg = "备份根目录不存在或不是目录: " + rootDir;
            return records;
        }

        int badCount = 0;

        for (const auto& udidEntry : fs::directory_iterator(root)) {
            if (!udidEntry.is_directory()) {
                continue;
            }
            const fs::path& udidPath = udidEntry.path();
            std::string udid = udidPath.filename().string();

            for (const auto& backupEntry : fs::directory_iterator(udidPath)) {
                if (!backupEntry.is_directory()) {
                    continue;
                }
                fs::path backupPath = backupEntry.path();

                BackupRecord rec;
                rec.path = backupPath.string();
                rec.udid = udid;

                // 查找 Info.plist 或 Manifest.plist
                fs::path infoPath = backupPath / "Info.plist";
                fs::path manifestPath = backupPath / "Manifest.plist";

                plist_t plist = nullptr;
                std::string plistName;

                if (fs::exists(infoPath)) {
                    plist_read_from_file(infoPath.string().c_str(), &plist, nullptr);
                    plistName = "Info.plist";
                } else if (fs::exists(manifestPath)) {
                    plist_read_from_file(manifestPath.string().c_str(), &plist, nullptr);
                    plistName = "Manifest.plist";
                }

                if (!plist || plist_get_node_type(plist) != PLIST_DICT) {
                    if (plist) plist_free(plist);
                    ++badCount;
                    continue;
                }

                // 从 Info.plist 中读取设备信息
                if (plistName == "Info.plist") {
                    plist_t node = nullptr;

                    node = plist_dict_get_item(plist, "Device Name");
                    if (node && plist_get_node_type(node) == PLIST_STRING) {
                        char* s = nullptr;
                        plist_get_string_val(node, &s);
                        if (s) {
                            rec.deviceName = s;
                            free(s);
                        }
                    }
                    if (rec.deviceName.empty()) {
                        node = plist_dict_get_item(plist, "Display Name");
                        if (node && plist_get_node_type(node) == PLIST_STRING) {
                            char* s = nullptr;
                            plist_get_string_val(node, &s);
                            if (s) {
                                rec.deviceName = s;
                                free(s);
                            }
                        }
                    }

                    node = plist_dict_get_item(plist, "Product Type");
                    if (node && plist_get_node_type(node) == PLIST_STRING) {
                        char* s = nullptr;
                        plist_get_string_val(node, &s);
                        if (s) {
                            rec.productType = s;
                            free(s);
                        }
                    }

                    node = plist_dict_get_item(plist, "Product Version");
                    if (node && plist_get_node_type(node) == PLIST_STRING) {
                        char* s = nullptr;
                        plist_get_string_val(node, &s);
                        if (s) {
                            rec.iosVersion = s;
                            free(s);
                        }
                    }

                    node = plist_dict_get_item(plist, "Last Backup Date");
                    if (node && plist_get_node_type(node) == PLIST_DATE) {
                        int64_t sec = 0;
                        plist_get_unix_date_val(node, &sec);
                        rec.backupTime = static_cast<std::time_t>(sec);
                    }
                } else {
                    // Manifest.plist：尝试从 Lockdown 子字典中读取设备信息
                    plist_t lockdown = plist_dict_get_item(plist, "Lockdown");
                    if (lockdown && plist_get_node_type(lockdown) == PLIST_DICT) {
                        plist_t node = nullptr;

                        node = plist_dict_get_item(lockdown, "DeviceName");
                        if (node && plist_get_node_type(node) == PLIST_STRING) {
                            char* s = nullptr;
                            plist_get_string_val(node, &s);
                            if (s) {
                                rec.deviceName = s;
                                free(s);
                            }
                        }

                        node = plist_dict_get_item(lockdown, "ProductType");
                        if (node && plist_get_node_type(node) == PLIST_STRING) {
                            char* s = nullptr;
                            plist_get_string_val(node, &s);
                            if (s) {
                                rec.productType = s;
                                free(s);
                            }
                        }

                        node = plist_dict_get_item(lockdown, "ProductVersion");
                        if (node && plist_get_node_type(node) == PLIST_STRING) {
                            char* s = nullptr;
                            plist_get_string_val(node, &s);
                            if (s) {
                                rec.iosVersion = s;
                                free(s);
                            }
                        }
                    }
                }

                plist_free(plist);

                // 粗略统计备份大小
                std::uint64_t total = 0;
                try {
                    for (const auto& p : fs::recursive_directory_iterator(backupPath)) {
                        if (p.is_regular_file()) {
                            std::error_code sec;
                            auto sz = p.file_size(sec);
                            if (!sec) {
                                total += sz;
                            }
                        }
                    }
                } catch (const std::exception& ex) {
                    spdlog::warn("[IosBackup] recursive_directory_iterator failed at {}: {}",
                                 backupPath.string(), ex.what());
                }
                rec.totalBytes = total;

                records.push_back(std::move(rec));
            }
        }

        if (badCount > 0) {
            errMsg = "扫描完成，跳过 " + std::to_string(badCount) + " 个损坏备份";
        }
    } catch (const std::exception& ex) {
        errMsg = std::string("扫描备份目录时发生异常: ") + ex.what();
    }

    return records;
}

IosBackupService::BackupResult IosBackupService::PerformRestore(
    const BackupRecord& record,
    const std::string& targetUdid,
    std::function<void(double,const std::string&)> onProgress)
{
    (void)record;
    (void)targetUdid;
    if (!onProgress) {
        onProgress = [](double, const std::string&) {};
    }

    onProgress(0.0, "restore not implemented yet");

    BackupResult res;
    res.code = BackupResultCode::Unsupported;
    res.message = "restore not implemented yet";

    spdlog::info("[IosBackup] PerformRestore called but not implemented yet");

    onProgress(1.0, "restore not implemented yet");
    return res;
}

#else  // !WITH_LIBIMOBILEDEVICE

// 未启用 libimobiledevice：TestConnection 的 stub
bool IosBackupService::TestConnection(const std::string& udid, DeviceInfo* outInfo, std::string& errMsg) {
    (void)udid;
    (void)outInfo;
    errMsg = "IosBackupService: 当前构建未启用 libimobiledevice（请在 CMake 中使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] TestConnection called but libimobiledevice support is not enabled");
    return false;
}

IosBackupService::BackupResult IosBackupService::PerformBackup(
    const std::string& udid,
    const BackupOptions& opt,
    std::function<void(double,const std::string&)> onProgress)
{
    (void)udid;
    (void)opt;
    if (onProgress) {
        onProgress(0.0, "libimobiledevice not compiled in");
        onProgress(1.0, "Backup unsupported");
    }

    BackupResult res;
    res.code = BackupResultCode::Unsupported;
    res.message = "IosBackupService: 当前构建未启用 libimobiledevice（请使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] PerformBackup called but libimobiledevice support is not enabled");
    return res;
}

std::vector<IosBackupService::BackupRecord> IosBackupService::ListBackups(
    const std::string& rootDir,
    std::string& errMsg)
{
    (void)rootDir;
    errMsg = "IosBackupService: 当前构建未启用 libimobiledevice，无法扫描 iOS 备份（请使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] ListBackups called but libimobiledevice support is not enabled");
    return {};
}

IosBackupService::BackupResult IosBackupService::PerformRestore(
    const BackupRecord& record,
    const std::string& targetUdid,
    std::function<void(double,const std::string&)> onProgress)
{
    (void)record;
    (void)targetUdid;
    if (onProgress) {
        onProgress(0.0, "libimobiledevice not compiled in");
        onProgress(1.0, "Restore unsupported");
    }

    BackupResult res;
    res.code = BackupResultCode::Unsupported;
    res.message = "IosBackupService: 当前构建未启用 libimobiledevice，无法执行还原（请使用 -DWITH_LIBIMOBILEDEVICE=ON 并正确安装依赖）";
    spdlog::warn("[IosBackup] PerformRestore called but libimobiledevice support is not enabled");
    return res;
}

#endif
