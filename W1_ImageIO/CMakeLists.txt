cmake_minimum_required(VERSION 3.21)
project(w1_imageio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== ⚠️ 硬阻止 Debug 配置 =====
# 原因:用户 OpenCV 只编了 Release,Debug 会触发 cv::debug_build_guard 命名空间 ABI 不匹配
# VS 顶部工具栏切到 x64-Release 后重新 configure
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "❌ W1 必须用 Release 配置!\n"
        "  当前 CMAKE_BUILD_TYPE=Debug 会触发 cv::debug_build_guard ABI 不匹配。\n"
        "  修复:VS 顶部配置下拉框 → 选 x64-Release → Ctrl+Shift+R 重新 configure → F7 重新编译。\n"
        "  详细原因见 memory: opencv-debug-abi-mismatch")
endif()

# ===== OpenCV 路径（你已装在 D 盘，不走 C 盘）=====
# 注意: 指向 install/lib/ 而不是 install/ !
# 原因: install/ 下的 OpenCVConfig.cmake 是 vcpkg 风格的包装器,会做平台/运行时检测
#       但 install/lib/OpenCVConfig.cmake 才是真正的 config,直接含 include/lib 路径
#       包装器的正则 ^19[34][0-9]$ 不识别 MSVC 19.51 (VS 2026 / VS 18)
set(OpenCV_DIR "D:/opencv-src/build/install/lib")
find_package(OpenCV REQUIRED)

# ===== 源文件（跟 hmi 工程一样显式列出）=====
set(SOURCES main.cpp)

add_executable(w1_imageio ${SOURCES})

# ===== OpenCV 链接 =====
target_include_directories(w1_imageio PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w1_imageio PRIVATE ${OpenCV_LIBS})

# ===== MSVC UTF-8（跟 hmi 工程一致）=====
if(MSVC)
    target_compile_options(w1_imageio PRIVATE /utf-8)
endif()

# ===== VS 工作目录设为源码目录，这样 images/test.jpg 相对路径能找到 =====
set_target_properties(w1_imageio PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ===== 复制 images/ 到 build 目录（F5 调试时用）=====
add_custom_command(TARGET w1_imageio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w1_imageio>/images
)

# ===== 复制 OpenCV DLLs 到 exe 目录（Windows 必做，否则启动报"找不到 DLL"）=====
# OpenCV 是动态库 (DLL),exe 运行时 Windows 会按: exe 同目录 → 系统目录 → PATH 顺序查找
# 不复制就要把 D:/opencv-src/build/install/bin 加进 PATH,容易污染环境
add_custom_command(TARGET w1_imageio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w1_imageio>
    COMMENT "Copying OpenCV DLLs to output directory"
)