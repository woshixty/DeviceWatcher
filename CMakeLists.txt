cmake_minimum_required(VERSION 3.15)

project(DeviceWatcher VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable folders for IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Sources
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(${PROJECT_NAME}
    ${SRC_DIR}/main.cpp

    ${SRC_DIR}/core/DeviceManager.cpp
    ${SRC_DIR}/core/EventBus.cpp
    ${SRC_DIR}/core/Utils.cpp
    ${SRC_DIR}/core/Serialize.cpp

    ${SRC_DIR}/providers/AndroidAdbProvider.cpp
    ${SRC_DIR}/providers/IosUsbmuxProvider.cpp
    ${SRC_DIR}/providers/UsbProvider.cpp

    ${SRC_DIR}/ui/CliMenu.cpp
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Expose version to code
target_compile_definitions(${PROJECT_NAME} PRIVATE DEVICEWATCHER_VERSION="${PROJECT_VERSION}")

# Dependencies (manifest-mode via vcpkg). Keep CMake clean; rely on toolchain setup.
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        fmt::fmt
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        asio::asio
        Threads::Threads
)

# Organize sources in IDEs
source_group(TREE ${SRC_DIR} FILES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/core/DeviceManager.cpp
    ${SRC_DIR}/core/DeviceManager.h
    ${SRC_DIR}/core/DeviceModel.h
    ${SRC_DIR}/core/EventBus.cpp
    ${SRC_DIR}/core/EventBus.h
    ${SRC_DIR}/core/Utils.cpp
    ${SRC_DIR}/core/Utils.h
    ${SRC_DIR}/core/Serialize.h
    ${SRC_DIR}/providers/AndroidAdbProvider.cpp
    ${SRC_DIR}/providers/AndroidAdbProvider.h
    ${SRC_DIR}/providers/IosUsbmuxProvider.cpp
    ${SRC_DIR}/providers/IosUsbmuxProvider.h
    ${SRC_DIR}/providers/UsbProvider.cpp
    ${SRC_DIR}/providers/UsbProvider.h
    ${SRC_DIR}/ui/CliMenu.cpp
    ${SRC_DIR}/ui/CliMenu.h
)
