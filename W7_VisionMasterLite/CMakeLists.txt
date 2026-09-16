cmake_minimum_required(VERSION 3.21)
project(w7_vm_lite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# === Release-only (与 W1–W6 一致) ===
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "W7 must use Release. Debug triggers cv::debug_build_guard ABI mismatch.")
endif()

# === Qt6 ===
set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# === OpenCV (与 W1–W6 完全一致) ===
set(OpenCV_DIR "D:/opencv-src/build/install/lib")
find_package(OpenCV REQUIRED)

# === 源码 ===
set(SOURCES
    main.cpp
    mainwindow.cpp
    pipeline_view.cpp
    operator_node.cpp
    connection_line.cpp
    commands/command.cpp
    operators/param_form.cpp
    operators/operator_factory.cpp
    operators/color_segment.cpp
    operators/blob_count.cpp
    operators/count_check.cpp
    operators/template_match.cpp
    operators/qr_decode.cpp
    operators/image_source_dir.cpp
    operators/image_source_camera.cpp
)

set(HEADERS
    mainwindow.h
    pipeline_view.h
    operator_node.h
    connection_line.h
    operators/operator_base.h
    operators/param_form.h
    operators/operator_factory.h
    operators/color_segment.h
    operators/blob_count.h
    operators/count_check.h
    operators/template_match.h
    operators/qr_decode.h
)

qt_add_executable(w7_vm_lite WIN32 ${SOURCES} ${HEADERS})

target_include_directories(w7_vm_lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_vm_lite PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_vm_lite PRIVATE /utf-8)
endif()

set_target_properties(w7_vm_lite PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# 拷 OpenCV DLL + 测试图(与 W1–W6 一致)
add_custom_command(TARGET w7_vm_lite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_vm_lite>
    COMMENT "Copying OpenCV DLLs to output directory"
)

add_custom_command(TARGET w7_vm_lite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w7_vm_lite>/images
    COMMENT "Copying test images to output directory"
)
