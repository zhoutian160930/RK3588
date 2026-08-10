cmake_minimum_required(VERSION 3.16)
project(yolov8)

link_directories(${CMAKE_SOURCE_DIR}/lib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)
set(CMAKE_BUILD_TYPE Release) # None, Debug, Release, RelWithDebInfo, MinSizeRel
#SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -Wno-unused-parameter -O0 -g -Wall")
#SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS}  -Wno-unused-parameter -O0 -g -Wall")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -Wno-unused-parameter -O3 -g -Wall")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS}  -Wno-unused-parameter -O3 -g -Wall")

include_directories(include)
include_directories(utils)

add_subdirectory(utils)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})
# find_package(kaylordut REQUIRED)
# find_package(bytetrack REQUIRED)

# ===================== LVGL UI 集成 =====================
option(ENABLE_UI "Build with LVGL UI integration" ON)
set(LVGL_BACKEND "SDL" CACHE STRING "LVGL display backend: SDL or FBDEV")
set_property(CACHE LVGL_BACKEND PROPERTY STRINGS SDL FBDEV)

# 外部 LVGL 源码路径(已下载到本机)
set(LVGL_SRC_DIR "/home/ubuntu/lvgl-release-v8.3" CACHE PATH "LVGL source root")
set(LV_DRIVERS_PARENT "/home/ubuntu/lv_port_linux-release-v8.3" CACHE PATH "parent dir of lv_drivers")
set(LV_DRIVERS_DIR "${LV_DRIVERS_PARENT}/lv_drivers" CACHE PATH "lv_drivers dir")

if(ENABLE_UI)
    # 指向本仓库的 lv_conf.h / lv_drv_conf.h
    set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/lv_conf.h" CACHE PATH "lv_conf.h path" FORCE)
    set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "include \"lvgl.h\"" FORCE)
    set(LV_CONF_INCLUDE_SIMPLE  ON CACHE BOOL "simple conf include" FORCE)

    # 构建 LVGL 静态库
    add_subdirectory(${LVGL_SRC_DIR} ${CMAKE_BINARY_DIR}/lvgl_build EXCLUDE_FROM_ALL)

    # FreeType (中文显示)
    find_package(Freetype REQUIRED)
    target_link_libraries(lvgl PUBLIC Freetype::Freetype)

    # 按后端选择 lv_drivers 源文件
    find_package(SDL2 REQUIRED)
    # 强制用系统 spdlog，避免被环境里的 micro_ros 静态库污染
    find_library(SPDLOG_LIB NAMES spdlog PATHS /usr/lib/aarch64-linux-gnu /usr/lib NO_DEFAULT_PATH REQUIRED)
    if(LVGL_BACKEND STREQUAL "SDL")
        set(LV_DRIVERS_SRC
            ${LV_DRIVERS_DIR}/sdl/sdl.c
            ${LV_DRIVERS_DIR}/sdl/sdl_common.c)
    elseif(LVGL_BACKEND STREQUAL "FBDEV")
        set(LV_DRIVERS_SRC
            ${LV_DRIVERS_DIR}/display/fbdev.c
            ${LV_DRIVERS_DIR}/indev/evdev.c)
    endif()

    add_library(lv_drivers_lib STATIC ${LV_DRIVERS_SRC} ui/mouse_cursor_icon.c)
    target_include_directories(lv_drivers_lib PUBLIC
        ${LV_DRIVERS_DIR} ${CMAKE_SOURCE_DIR} ${LVGL_SRC_DIR})
    target_link_libraries(lv_drivers_lib PUBLIC lvgl)
    if(LVGL_BACKEND STREQUAL "SDL")
        target_link_libraries(lv_drivers_lib PUBLIC ${SDL2_LIBRARIES})
        target_include_directories(lv_drivers_lib PUBLIC ${SDL2_INCLUDE_DIRS})
        target_compile_definitions(lv_drivers_lib PUBLIC LVGL_BACKEND_SDL=1)
    else()
        target_compile_definitions(lv_drivers_lib PUBLIC LVGL_BACKEND_FBDEV=1)
    endif()

    add_subdirectory(ui)
endif()

# add_executable(videofile_demo videofile_demo.cpp)
# target_link_libraries(videofile_demo ${OpenCV_LIBS} yolov8-utils)

# add_executable(camera_demo camera_demo.cpp)
# target_link_libraries(camera_demo ${OpenCV_LIBS} yolov8-utils)

# add_executable(imagefile_demo imagefile_demo.cpp)
# target_link_libraries(imagefile_demo ${OpenCV_LIBS} yolov8-utils)

add_executable(yolov8_sam_demo yolov8_sam_demo.cpp)
target_link_libraries(yolov8_sam_demo ${OpenCV_LIBS} yolov8-utils)
if(ENABLE_UI)
    target_link_libraries(yolov8_sam_demo ui_lib lv_drivers_lib lvgl ${SPDLOG_LIB} fmt)
    target_compile_definitions(yolov8_sam_demo PRIVATE WITH_UI=1)
endif()
