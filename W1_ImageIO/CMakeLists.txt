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

# ===== OpenCV 路径 =====
# 路径是**这台机器**的事实,不是项目的事实 ——
# 所以默认值写在 common/cmake/ThirdParty.cmake 里,CI 用 -D 覆盖。
# (OpenCV_DIR 为什么必须指向 install/lib 而不是 install/,
#  以及"为什么必须用 CACHE 变量",见那个文件的注释。)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/ThirdParty.cmake)
find_package(OpenCV REQUIRED)

# ===== 源文件（跟 hmi 工程一样显式列出）=====
#
# pixel_ops.cpp **链进两个目标**（demo 和测试），一份源码两个用法。
#
# 为什么不是"测试里再抄一遍判据"：
#   抄出来的副本会随核心漂移 —— 改了判据，测试还在按旧判据断言，
#   于是测试「绿着」而 demo 已经变了。绿灯什么也没证明。
#
# 仓库里已有同样的做法：W6 的 w6_cli.cpp、W2 的 color_pipeline.cpp。
set(SOURCES
    main.cpp
    pixel_ops.cpp
)

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
# 不复制就要把 OpenCV 的 bin 目录加进 PATH,容易污染环境
add_custom_command(TARGET w1_imageio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w1_imageio>
    COMMENT "Copying OpenCV DLLs to output directory"
)

# ============================================================
# === 无头回归测试（2026-10-26 新增）
#
# W1–W5 在这一天之前**一条自动化测试都没有**。
# 补法不是"给 main.cpp 加个开关"，而是把纯逻辑抽出来再测它 ——
# 见 pixel_ops.h 的开头为什么有这一层。
#
# 这个套件会往**工作目录**写一个 PNG 再读回来（验证存盘/读盘这一链），
# 所以工作目录必须存在，而且必须在 **build/** 里 ——
# 源码树一个字节都不许变（CI 的最后一步就查这个）。
# ============================================================
enable_testing()

add_executable(w1_imageio_test
    tests/test_w1_imageio.cpp
    pixel_ops.cpp
)

target_include_directories(w1_imageio_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w1_imageio_test PRIVATE ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w1_imageio_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w1_imageio_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w1_imageio_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 工作目录必须在 configure 期就建好：
# ctest 遇到不存在的工作目录会直接报 "Unable to find executable"，
# 那是一条和测试本身无关的红 —— 看起来像代码坏了，其实一个测试都没跑。
set(W1_TEST_WORK "${CMAKE_CURRENT_BINARY_DIR}/test_work/w1_imageio")
file(MAKE_DIRECTORY "${W1_TEST_WORK}")

add_test(NAME w1_imageio
         COMMAND w1_imageio_test
         WORKING_DIRECTORY "${W1_TEST_WORK}")
