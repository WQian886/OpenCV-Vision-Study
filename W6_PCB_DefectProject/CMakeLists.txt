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
# === 测试工作目录:每个套件一份,全在 build 里
#
# W6 的三个套件以前都跑在 build/ 根目录(没给 WORKING_DIRECTORY),
# 产物落点其实已经在 build 里 —— 但**三个套件共用一个目录**。
# 共用的坏处:谁在子目录里写了个固定名字的文件,另一个套件就可能踩到。
# w6_cli_test 会在 CWD 下建 w6_cli_test_tmp/(自己起自己删),
# 别的套件要是也用这个名字,或者将来 ctest -j 并行跑,就会互相删。
#
# 现在每个套件一份:${CMAKE_CURRENT_BINARY_DIR}/test_work/<套件名>/
#
# ## 数据从哪来
#
# 测试**读**的数据(板图、manifest)不走 CWD —— 那是 findRepoRoot()
# 往上找到的源码树,工作目录挪了也不影响。
#
# 但有一份数据是**靠 CWD 找**的:w6::findRecipeDir("") 的自动发现,
# 它按顺序试 "recipes"、 "../W7_VisionMasterLite/recipes"、
# "W7_VisionMasterLite/recipes" 这几个**相对路径**。
#
# 工作目录一挪到 build/test_work/w6_cli/,这三个相对路径就全落空了
# —— 断言当场变红(实测:162 条里红 1 条"自动找得到配方目录")。
# 这正是这条断言该有的样子:它没有假装通过。
#
# 修法不是"改断言",也不是"把 CWD 拉回去"(那就等于白改),
# 而是**把配方暂存进工作目录** —— 这也正是 W6 真正跑起来时的样子:
# CMake 会把 recipes/ 拷到 exe 旁边,自动发现找的就是它。
# ============================================================

# 套件名累积(给末尾的暂存目标用)
set(W6_TEST_SUITES "")

function(w6_add_test suite target)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/test_work/${suite}")
    add_test(NAME ${suite} COMMAND ${target} WORKING_DIRECTORY "${_dir}")
    set(W6_TEST_SUITES ${W6_TEST_SUITES} ${suite} PARENT_SCOPE)
endfunction()

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

w6_add_test(pcb_core w6_core_test)
# ============================================================
# === 共享 QR/OCR 核心 (common/qr_ocr)
#
# QR 解码 + tesseract 文字识别,W5 / W6 / W7 **共用同一份**。
#
# 这是本项目**第五次**抄同一个副本。前三次分别长在:
#   · W5 的 main.cpp(QR + tesseract 各一份)
#   · W6 的 w6_cli.cpp(抄 W5 的,修了宽字符转换和 --psm)
#   · W7 的 qr_decode 算子(又抄了一份 QR 解码)
#
# 抄出来的代价不是"多几行代码":
#   · W6 的 --psm 11 和 W5 的 --psm 6 是**两个不同的答案**,
#     谁也说不清哪个对 —— 它们是两份拷贝,不是一份的两个参数。
#   · W6 那份修了宽字符转换的坑(路径带中文会毁),
#     W5 那份还在逐元素窄化。修一处,另一处照旧坏着。
#
# 同样刻意不依赖 Qt。
# ============================================================
set(QR_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/qr_ocr")

add_library(w6_qr_ocr STATIC
    ${QR_OCR_DIR}/qr_ocr.cpp
)

target_include_directories(w6_qr_ocr PUBLIC
    ${QR_OCR_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w6_qr_ocr PUBLIC ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w6_qr_ocr PRIVATE /utf-8)
endif()

# --- QR/OCR 核心自己的测试 ---
#
# 合并之前这三份代码**一行测试都没有**。
# 现在有了 —— 尤其重要的是"tesseract 没装时必须明说"那条,
# 那正是以前没人知道的行为。
add_executable(w6_qr_ocr_test
    ${QR_OCR_DIR}/tests/test_qr_ocr.cpp
)

target_include_directories(w6_qr_ocr_test PRIVATE ${QR_OCR_DIR})

target_link_libraries(w6_qr_ocr_test PRIVATE ${OpenCV_LIBS} w6_qr_ocr)

if(MSVC)
    target_compile_options(w6_qr_ocr_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w6_qr_ocr_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w6_qr_ocr_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w6_add_test(qr_ocr w6_qr_ocr_test)

# ============================================================
# === W6 自己的可测逻辑层 + 它的回归测试
#
# w6_cli.h/.cpp 是从 main.cpp 里抽出来的**纯逻辑**层
# (查板型 / 找配方 / 跑一块板 / 摊 CSV 行 / 写 CSV)。
#
# 为什么要抽:
#   main.cpp 是程序入口,没法被别的目标链接 ——
#   逻辑留在 main.cpp 里,W6 就一条自动化测试都没有,
#   而 W6 恰恰是最需要测试的一层(它决定"哪些图被检了")。
#
# 仓库里已有同样的先例:W7 的 step_scope.h 就是这么从 mainwindow.cpp
# 抽出来的,见那个头文件的说明。
# ============================================================
set(SOURCES
    main.cpp
    w6_cli.cpp
)

add_executable(w6_pcb_defect ${SOURCES})

target_include_directories(w6_pcb_defect PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w6_pcb_defect PRIVATE ${OpenCV_LIBS} w6_pcb_core w6_qr_ocr)

if(MSVC)
    target_compile_options(w6_pcb_defect PRIVATE /utf-8)
endif()

# --- W6 CLI 层的回归测试 ---
add_executable(w6_cli_test
    tests/test_w6_cli.cpp
    w6_cli.cpp
)

target_include_directories(w6_cli_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w6_cli_test PRIVATE ${OpenCV_LIBS} w6_pcb_core w6_qr_ocr)

if(MSVC)
    target_compile_options(w6_cli_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w6_cli_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w6_cli_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w6_add_test(w6_cli w6_cli_test)

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

# ============================================================
# === 暂存测试数据(每次构建都重拷)
#
# 配方只有一份(在 W7_VisionMasterLite/recipes/),这里是**构建期暂存**,
# 不是第二份定义 —— 真正读的仍然是那一份,导出脚本也只认那一份。
#
# 为什么每次构建都重拷:
#   POST_BUILD 只在重编时跑。改了 recipes/demo.json 而代码没动
#   -> 不重编 -> 不重拷 -> 测试拿旧配方判板,还判得"对" ——
#   旧配方下的正确答案。改配方其实没生效,没人知道。
#   add_custom_target 没有输出文件,永远算过期,所以每次构建都跑。
#
# 配方目录**只有** findRecipeDir("") 的自动发现需要它
# (它按相对路径试 "recipes" 等几个位置)。
# 显式指定 --recipes 的路径、以及测试本身读真值表,都不依赖这里。
# ============================================================
set(_stage_cmds "")
foreach(_suite IN LISTS W6_TEST_SUITES)
    set(_d "${CMAKE_CURRENT_BINARY_DIR}/test_work/${_suite}")
    list(APPEND _stage_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_d}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../W7_VisionMasterLite/recipes"
                "${_d}/recipes")
endforeach()

add_custom_target(w6_test_data ALL ${_stage_cmds}
    COMMENT "Staging W6 test data into build/test_work/")
