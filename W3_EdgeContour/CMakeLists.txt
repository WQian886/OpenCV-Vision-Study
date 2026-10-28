cmake_minimum_required(VERSION 3.21)
project(w3_edge_contour LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== ⚠️ 硬阻止 Debug 配置 =====
# 原因:用户 OpenCV 只编了 Release,Debug 会触发 cv::debug_build_guard 命名空间 ABI 不匹配
# VS 顶部工具栏切到 x64-Release 后重新 configure
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "❌ W3 必须用 Release 配置!\n"
        "  当前 CMAKE_BUILD_TYPE=Debug 会触发 cv::debug_build_guard ABI 不匹配。\n"
        "  修复:VS 顶部配置下拉框 → 选 x64-Release → Ctrl+Shift+R 重新 configure → F7。\n"
        "  详细原因见 memory: opencv-debug-abi-mismatch")
endif()

# ===== OpenCV 路径 =====
# 路径是**这台机器**的事实,不是项目的事实 ——
# 所以默认值写在 common/cmake/ThirdParty.cmake 里,CI 用 -D 覆盖。
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/ThirdParty.cmake)
find_package(OpenCV REQUIRED)

# ===== 共享的「合成 PCB 图的事实」 =====
# 颜色、金色的 HSV 窗口、噪声强度 —— 只有这一份,W2 和 W3 共用。
set(PCB_SIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/pcb_sim")

# ===== 源文件 =====
#
# edge_pipeline.cpp **链进两个目标**(demo 和测试),一份源码两个用法。
#
# 为什么不是"测试里再抄一遍逻辑":
#   抄出来的副本会随核心漂移 —— 改了核心的面积门槛,
#   测试还在按旧门槛断言,于是测试「绿着」而 demo 已经变了。
#   绿灯什么也没证明。
#
# 仓库里已有同样的做法:W2 的 color_pipeline.cpp、W6 的 w6_cli.cpp。
set(SOURCES
    main.cpp
    edge_pipeline.cpp
)

add_executable(w3_edge_contour ${SOURCES})

# ===== OpenCV 链接 =====
target_include_directories(w3_edge_contour PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PCB_SIM_DIR}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(w3_edge_contour PRIVATE ${OpenCV_LIBS})

# ===== MSVC UTF-8 =====
if(MSVC)
    target_compile_options(w3_edge_contour PRIVATE /utf-8)
endif()

# ===== VS 工作目录 =====
set_target_properties(w3_edge_contour PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ===== 复制 images/ 到 build 目录 =====
add_custom_command(TARGET w3_edge_contour POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w3_edge_contour>/images
)

# ===== 复制 OpenCV DLLs 到 exe 目录（Windows 必做）=====
add_custom_command(TARGET w3_edge_contour POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w3_edge_contour>
    COMMENT "Copying OpenCV DLLs to output directory"
)

# ============================================================
# === 无头回归测试（2026-10-28 新增）
#
# W1–W5 在 2026-10-26 之前**一条自动化测试都没有**。
# 补法不是"给 main.cpp 加个开关"，而是把纯逻辑抽出来再测它 ——
# 见 edge_pipeline.h 的开头为什么有这一层。
#
# 这个套件**不写任何文件、不开任何窗口**，
# 所以"产物不许落源码树"这条对它天然成立。
# 但工作目录照样给一份：以后有人往里加 imwrite，
# 落点已经在 build/ 里了（和 W1/W2/W6/W7 同一个规矩）。
# ============================================================
enable_testing()

add_executable(w3_edge_contour_test
    tests/test_w3_edge_contour.cpp
    edge_pipeline.cpp
)

target_include_directories(w3_edge_contour_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PCB_SIM_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w3_edge_contour_test PRIVATE ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w3_edge_contour_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w3_edge_contour_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w3_edge_contour_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 工作目录必须在 configure 期就建好：
# ctest 遇到不存在的工作目录会直接报 "Unable to find executable"，
# 那是一条和测试本身无关的红 —— 看起来像代码坏了，其实一个测试都没跑。
set(W3_TEST_WORK "${CMAKE_CURRENT_BINARY_DIR}/test_work/w3_edge_contour")
file(MAKE_DIRECTORY "${W3_TEST_WORK}")

add_test(NAME w3_edge_contour
         COMMAND w3_edge_contour_test
         WORKING_DIRECTORY "${W3_TEST_WORK}")
