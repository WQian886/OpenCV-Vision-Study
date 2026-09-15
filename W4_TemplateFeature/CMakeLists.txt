cmake_minimum_required(VERSION 3.21)
project(w4_template_feature LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== ⚠️ 硬阻止 Debug 配置 =====
# 原因:用户 OpenCV 只编了 Release,Debug 会触发 cv::debug_build_guard 命名空间 ABI 不匹配
# VS 顶部工具栏切到 x64-Release 后重新 configure
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "❌ W4 必须用 Release 配置!\n"
        "  当前 CMAKE_BUILD_TYPE=Debug 会触发 cv::debug_build_guard ABI 不匹配。\n"
        "  修复:VS 顶部配置下拉框 → 选 x64-Release → Ctrl+Shift+R 重新 configure → F7。\n"
        "  详细原因见 memory: opencv-debug-abi-mismatch")
endif()

# ===== OpenCV 路径(必须指 install/lib/,理由见 opencv-cmake-dir-quirk memory)=====
set(OpenCV_DIR "D:/opencv-src/build/install/lib")
find_package(OpenCV REQUIRED)

# ===== 源文件 =====
set(SOURCES main.cpp)
add_executable(w4_template_feature ${SOURCES})

# ===== OpenCV 链接 =====
target_include_directories(w4_template_feature PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w4_template_feature PRIVATE ${OpenCV_LIBS})

# ===== MSVC UTF-8 =====
if(MSVC)
    target_compile_options(w4_template_feature PRIVATE /utf-8)
endif()

# ===== VS 工作目录 =====
set_target_properties(w4_template_feature PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ===== 复制 images/ 到 build 目录 =====
add_custom_command(TARGET w4_template_feature POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w4_template_feature>/images
)

# ===== 复制 OpenCV DLLs 到 exe 目录(Windows 必做)=====
add_custom_command(TARGET w4_template_feature POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w4_template_feature>
    COMMENT "Copying OpenCV DLLs to output directory"
)
