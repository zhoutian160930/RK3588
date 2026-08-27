# Qt UI 模块(yolov8_sam_demo_qt)。与 LVGL 版(ui/)并行,验证后删除旧版。
find_package(Qt5 REQUIRED COMPONENTS Widgets)
find_package(OpenCV REQUIRED)
set(CMAKE_AUTOMOC ON)
# 不用 AUTOUIC:工程无 .ui 文件,且 core/ui_log.h 会与 Qt uic 生成文件命名冲突

include_directories(${Qt5Widgets_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})

add_executable(yolov8_sam_demo_qt
    qt_main.cpp
    mainwindow.cpp
    mainwindow.h
    video_widget.cpp
    video_widget.h
    stats_panel.cpp
    stats_panel.h
    log_panel.cpp
    log_panel.h
    ../core/pipeline.cpp
    ../core/pipeline.h
    ../core/frame_bus.cpp
    ../core/logger.cpp
    ../core/ui_log.cpp
    ../core/config.cpp
    ../core/judgment.cpp
    ../core/can_bus.cpp
    ../core/gpio_out.cpp
    ../core/gpio_in.cpp
    ../core/camera_capture.cpp
    ../core/image_saver.cpp
)
set_source_files_properties(../core/logger.cpp PROPERTIES SKIP_AUTOUIC ON)  # ui_log.h 与 Qt uic 命名撞车
set_source_files_properties(../core/ui_log.cpp PROPERTIES SKIP_AUTOUIC ON)

target_include_directories(yolov8_sam_demo_qt PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(yolov8_sam_demo_qt PRIVATE
    Qt5::Widgets
    ${OpenCV_LIBS}
    yolov8-utils
    MvCameraControl
    ${SPDLOG_LIB}
    fmt
    pthread
)
target_compile_definitions(yolov8_sam_demo_qt PRIVATE SPDLOG_COMPILED_LIB)

# 可执行文件直接输出到 build/ 下(不要 build/qtui/ 多套一层)
set_target_properties(yolov8_sam_demo_qt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
