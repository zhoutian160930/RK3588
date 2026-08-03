find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# UI 模块库
add_library(ui_lib STATIC
    ui_main.cpp
    ui_canvas.cpp
    ui_app.cpp
    ui_filebrowser.cpp
    ../core/frame_bus.cpp
)
target_include_directories(ui_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ui_lib PUBLIC
    lvgl
    lv_drivers_lib
    yolov8-utils
    ${OpenCV_LIBS}
)

# Phase 1 验证程序
add_executable(ui_test ui_test.cpp)
target_link_libraries(ui_test PRIVATE ui_lib)
