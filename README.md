## DeviceWatcher

跨平台命令行工具：实时监听 iOS / Android 设备的接入与拔出，维护设备列表，提供菜单式操作，并支持导出 / 推送到外部系统。
目标平台：Windows（优先，VS2022 + CMake + vcpkg），兼容 macOS / Linux。

<p align="left"> <a href="#"><img alt="license" src="https://img.shields.io/badge/license-MIT-blue"></a> <a href="#"><img alt="lang" src="https://img.shields.io/badge/C%2B%2B-17-4c8"></a> <a href="#"><img alt="platform" src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-999"></a> <a href="#"><img alt="build" src="https://img.shields.io/badge/build-CMake%20%7C%20vcpkg-success"></a> </p>

### ✨ 功能（进行中✅）
- ⏳ ADB 直连（host:track-devices-l）监听 Android 上下线与基础信息
- ⏳ 统一设备模型与事件总线：Attach / InfoUpdated / Detach
- ⏳ CLI 菜单：实时监视、列表、详情、JSON/CSV 导出
- ⏳（开发中）iOS 监听（libimobiledevice/usbmuxd）
- ⏳ 去抖动与多源信息合流（ADB / iOS / USB 底层）
- ⏳ Webhook / 本地 TCP 推送（NDJSON）
- ⏳ Windows USB 底层信息（VID/PID/口径路径）
- ⏳ TUI（FTXUI）仪表盘、规则引擎、Prometheus Exporter

### 🧱 架构概览

```
DeviceWatcher
 ├─ core/
 │   ├─ DeviceManager        # 统一设备表、事件去抖与合流
 │   ├─ DeviceModel          # DeviceInfo / DeviceEvent
 │   └─ EventBus / Utils
 ├─ providers/
 │   ├─ AndroidAdbProvider   # ADB 直连，跟踪与 getprop 聚合
 │   ├─ IosUsbmuxProvider    # libimobiledevice/usbmuxd 设备事件
 │   └─ UsbProvider          # (Win) SetupAPI / CM_NOTIFY 取 VID/PID/口径
 └─ ui/
     ├─ CliMenu              # 菜单式 CLI
     └─ TuiApp (optional)    # FTXUI 仪表盘（可选编译）

```

统一设备模型(节选)

```
struct DeviceInfo {
  enum class Type { Android, iOS, Unknown };
  Type type;
  std::string uid;            // Android: serial, iOS: UDID
  std::string displayName;    // Android: model; iOS: DeviceName
  std::string manufacturer;   // e.g. Apple / OnePlus
  std::string model;          // e.g. iPhone15,2 / Pixel 8
  std::string osVersion;      // e.g. iOS 18.1 / Android 15
  std::string abi;            // arm64-v8a (Android)
  std::string transport;      // USB / WiFi / Unknown
  uint16_t vid = 0, pid = 0;  // from USB Provider (optional)
  bool online = false;
};
```

### 📦 依赖与构建

建设中...

先安装/配置 vcpkg，并置好VCPKG_ROOT或用 -DCMAKE_TOOLCHAIN_FILE 指向 vcpkg toolchain
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DWITH_LIBIMOBILEDEVICE=ON
cmake --build build --config Debug
```

```
Windows 构建与运行

  - 配置并生成：
      - cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/
        vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DWITH_LIBIMOBILEDEVICE=ON
      - cmake --build build --config Debug
  - 运行前设置 PATH（Debug 示例）：
      - 把 build\\vcpkg_installed\\x64-windows\\debug\\bin 加到 PATH
      - 或 Release：build\\vcpkg_installed\\x64-windows\\bin
  - 先决条件：
      - 安装 Apple Mobile Device Support（随 iTunes 或 Apple 官方安装包），否则无法与设备通信。
  - 使用：
      - 运行程序，菜单按 [6] 启动 iOS 监听；插拔设备应看到 ATTACH/DETACH，随后 INFO 里含 DeviceName/ProductType/
        ProductVersion。

  说明与可选项

  - 仅在清单加 libimobiledevice 就够用；libplist、usbmuxd 会作为传递依赖一并拉取，且由 libimobiledevice::libimobiledevice 透
    传链接。
  - 若希望非交互/安装包运行，更稳妥做法是拷贝依赖 DLL 到可执行目录；需要的话我可以加一个 CMake POST_BUILD 步骤自动复制。
  - Linux/macOS 可用 Homebrew/apt 安装同名包；若 config 模式找不到，可再加 pkg-config 回退（我也可以帮你补上）。
```

### 🚀 使用

建设中...

chcp 65001 && .\build\Debug\DeviceWatcher.exe --help

### 🗂️ 导出格式

建设中...