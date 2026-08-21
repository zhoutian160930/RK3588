cmake_minimum_required(VERSION 3.16)
project(yolov8)

link_directories(${CMAKE_SOURCE_DIR}/lib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)
set(CMAKE_BUILD_TYPE Release) # None, Debug, Release, RelWithDebInfo, MinSizeRel
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -Wno-unused-parameter -O3 -g -Wall")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -Wno-unused-parameter -O3 -g -Wall")

include_directories(include)
include_directories(utils)

# MVS SDK (海康威视工业相机 ARM64 SDK): 自动探测常见安装位置
# (/userdata/MVS 或官方安装的 /opt/MVS),都未找到时报错并提示手动指定:
#   cmake -DMVS_SDK_DIR=/your/path/to/MVS ..
set(MVS_SDK_DIR "" CACHE PATH "HikVision MVS SDK install path")
if(NOT MVS_SDK_DIR)
    if(EXISTS "/userdata/MVS/lib/aarch64/libMvCameraControl.so")
        set(MVS_SDK_DIR "/userdata/MVS")
    elseif(EXISTS "/opt/MVS/lib/aarch64/libMvCameraControl.so")
        set(MVS_SDK_DIR "/opt/MVS")
    else()
        message(FATAL_ERROR "未找到海康 MVS SDK,请先安装(见 docs/deployment.md),或手动指定: cmake -DMVS_SDK_DIR=/path/to/MVS ..")
    endif()
endif()
message(STATUS "MVS SDK: ${MVS_SDK_DIR}")
include_directories(${MVS_SDK_DIR}/include)
link_directories(${MVS_SDK_DIR}/lib/aarch64 /usr/lib)
set(CMAKE_BUILD_RPATH "${MVS_SDK_DIR}/lib/aarch64")

add_subdirectory(utils)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# 系统 spdlog(Qt UI 与 headless 共用)
find_library(SPDLOG_LIB NAMES spdlog PATHS /usr/lib/aarch64-linux-gnu /usr/lib NO_DEFAULT_PATH REQUIRED)

# ===================== Qt UI =====================
option(ENABLE_QT_UI "Build Qt Widgets UI (yolov8_sam_demo_qt)" ON)
if(ENABLE_QT_UI)
    add_subdirectory(qtui)
endif()

# ===================== headless 演示程序(无 UI) =====================
add_executable(yolov8_sam_demo yolov8_sam_demo.cpp
    core/config.cpp
    core/judgment.cpp
    core/can_bus.cpp
    core/gpio_out.cpp
    core/gpio_in.cpp
    core/gpio_utils.cpp
    core/image_saver.cpp
    core/logger.cpp
    core/ui_log.cpp
)
target_include_directories(yolov8_sam_demo PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(yolov8_sam_demo ${OpenCV_LIBS} yolov8-utils MvCameraControl ${SPDLOG_LIB} fmt pthread)
target_compile_definitions(yolov8_sam_demo PRIVATE SPDLOG_COMPILED_LIB)
