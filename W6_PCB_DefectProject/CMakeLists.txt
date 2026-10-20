cmake_minimum_required(VERSION 3.21)
project(w6_pcb_defect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "W6 must use Release. Debug triggers cv::debug_build_guard ABI mismatch.")
endif()

set(OpenCV_DIR "D:/opencv-src/build/install/lib")
find_package(OpenCV REQUIRED)

# ============================================================
# === 共享判定核心 (common/pcb_core)
#
# PCB 判定的真值(板型配方、颜色判据、逐条规则)在 common/pcb_core/ 下,
# W6(命令行)和 W7(算子)**共用同一份**。
#
# 为什么要共用:W6 原本是 `if (label == "R2")` 七处硬编码,换块板规则全空转
# (实测三块好板全被判废、两块废板输出与好板逐字相同);
# W7 那边早就改成了配方驱动 + 逐条标定的版本。
# 硬把 W7 的算法抄一份到 W6,就是本项目栽过四遍的"抄副本" ——
# 抄出来的那份会随 W7 的标定一起漂,漂了没人知道。
#
# 这一层刻意不依赖 Qt:W6 不引 Qt,引了就是给一个学习项目加依赖。
# ============================================================
set(PCB_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/pcb_core")

add_library(w6_pcb_core STATIC
    ${PCB_CORE_DIR}/pcb_core.cpp
    ${PCB_CORE_DIR}/mini_json.cpp
)

target_include_directories(w6_pcb_core PUBLIC
    ${PCB_CORE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w6_pcb_core PUBLIC ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w6_pcb_core PRIVATE /utf-8)
endif()

# ============================================================
# === 共享核心自己的测试
#
# 同一个测试**源文件**也链进 W7 的 w7_core_test —— 一份源码,两个目标。
# 为什么两边都跑:
#   · W7 那边是 VS 生成器 + Qt,跑的是"算子外壳 + 核心"端到端;
#   · W6 那边是 Ninja、无 Qt,跑的是"核心直接调"(W6 的用法),
#     而且顺带证明**核心在 Ninja/MSVC 下也能编**(W7 用的是 VS 生成器)。
#
# 测试自己从当前目录往上找仓库根,所以两个项目的 ctest 工作目录不同也没关系。
# ============================================================
enable_testing()

add_executable(w6_core_test
    ${PCB_CORE_DIR}/tests/test_pcb_core.cpp
)

target_include_directories(w6_core_test PRIVATE ${PCB_CORE_DIR})

target_link_libraries(w6_core_test PRIVATE ${OpenCV_LIBS} w6_pcb_core)

if(MSVC)
    target_compile_options(w6_core_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w6_core_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w6_core_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

add_test(NAME pcb_core COMMAND w6_core_test)

set(SOURCES main.cpp)
add_executable(w6_pcb_defect ${SOURCES})

target_include_directories(w6_pcb_defect PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w6_pcb_defect PRIVATE ${OpenCV_LIBS} w6_pcb_core)

if(MSVC)
    target_compile_options(w6_pcb_defect PRIVATE /utf-8)
endif()

set_target_properties(w6_pcb_defect PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# 把测试图（含 baseline / 7 张缺陷 / manifest.csv）拷到产物目录
add_custom_command(TARGET w6_pcb_defect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w6_pcb_defect>/images
)

# 板型配方也拷一份到 exe 旁边。
#
# 配方 JSON 只有一份(在 W7_VisionMasterLite/recipes/),不复制 —— 复制就是抄副本。
# 但 exe 得能独立跑,所以构建时把它拷到产物目录,程序按 "recipes/<板型>.json"
# 相对工作目录找(和 W7 算子的约定一样)。
#
# 配方本身由 W7_VisionMasterLite/tools/export_recipes.py 从
# **W6 自己的生成器**导出 —— 坐标和规则都来自生成器,不是手抄的。
add_custom_command(TARGET w6_pcb_defect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../W7_VisionMasterLite/recipes
        $<TARGET_FILE_DIR:w6_pcb_defect>/recipes
    COMMENT "Copying board recipes to output directory"
)

# 拷 OpenCV DLL 到产物目录
add_custom_command(TARGET w6_pcb_defect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w6_pcb_defect>
    COMMENT "Copying OpenCV DLLs to output directory"
)
