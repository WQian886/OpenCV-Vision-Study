cmake_minimum_required(VERSION 3.21)
project(w2_color_filter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== ⚠️ 硬阻止 Debug 配置 =====
# 原因:用户 OpenCV 只编了 Release,Debug 会触发 cv::debug_build_guard 命名空间 ABI 不匹配
# VS 顶部工具栏切到 x64-Release 后重新 configure
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "❌ W2 必须用 Release 配置!\n"
        "  当前 CMAKE_BUILD_TYPE=Debug 会触发 cv::debug_build_guard ABI 不匹配。\n"
        "  修复:VS 顶部配置下拉框 → 选 x64-Release → Ctrl+Shift+R 重新 configure → F7 重新编译。\n"
        "  详细原因见 memory: opencv-debug-abi-mismatch")
endif()

# ===== OpenCV 路径 =====
# 路径是**这台机器**的事实,不是项目的事实 ——
# 所以默认值写在 common/cmake/ThirdParty.cmake 里,CI 用 -D 覆盖。
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/ThirdParty.cmake)
find_package(OpenCV REQUIRED)

# ===== 源文件 =====
set(SOURCES main.cpp)

add_executable(w2_color_filter ${SOURCES})

# ===== OpenCV 链接 =====
target_include_directories(w2_color_filter PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w2_color_filter PRIVATE ${OpenCV_LIBS})

# ===== MSVC UTF-8 =====
if(MSVC)
    target_compile_options(w2_color_filter PRIVATE /utf-8)
endif()

# ===== VS 工作目录 =====
set_target_properties(w2_color_filter PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ===== 复制 images/ 到 build 目录 =====
add_custom_command(TARGET w2_color_filter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w2_color_filter>/images
)

# ===== 复制 OpenCV DLLs 到 exe 目录（Windows 必做）=====
add_custom_command(TARGET w2_color_filter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w2_color_filter>
    COMMENT "Copying OpenCV DLLs to output directory"
)