cmake_minimum_required(VERSION 3.21)
project(w7_vm_lite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ============================================================
# === 测试工作目录:产物落 build,不落源码树
#
# ⚠ 2026-10-20 之前,14 个套件里有 6 个的 WORKING_DIRECTORY 是
#   ${CMAKE_SOURCE_DIR} —— 也就是**源码树**
#   (pcb_operators / image_sources / measurement / run_policy /
#    detection_operators / template_match)。
#   于是测试造的临时图(images/w7_*.png、images/_t_*.png、
#   images/_test_src.avi)和算子的落盘产物,**全写在源码树里**。
#   另外 8 个套件没给 WORKING_DIRECTORY,跑在 build/ 根目录 ——
#   产物是进了 build,但那 8 个套件**共用一个目录**。
#
# 当时的应对是往 .gitignore 里加规则把它们**藏起来**:
#     **/images/w7_*.png
#     **/images/_test_src.avi
#     **/images/_srcframe_*.png
#
# 那是**治标**:文件还在源码树里,还在被人看见、被误提交、被拷进构建目录。
# 而且"用 ignore 藏起来"会让人以为"已经处理好了"。
#
# 现在每个套件有自己的工作目录,**全在 build 里**:
#     ${CMAKE_CURRENT_BINARY_DIR}/test_work/<套件名>/
#
# 每个套件一份目录(不是共用一份)有两个理由:
#   1) 产物不再落进源码树(就是上面那 6 个套件的老毛病);
#   2) ctest -j 并行时各写各的,不会因为共用一个 CWD 而看见对方的产物。
#      ⚠ 这一条是**风险**不是**实测**:那 8 个共用 build/ 根目录的套件
#      实测都不写文件,所以这次没踩上。共用 CWD 本身就是这个风险,
#      写不写文件只是"今天碰巧没发生"。
#
# ## 数据怎么办
#
# 配方是**输入**(pcb_inspect 按相对路径 "recipes/xxx.json" 读),
# images/ 既是输入(demo 板图)又是测试的输出目录(算子画框图)——
# 所以工作目录里必须有一份。
#
# 这一份是**构建期暂存**的,不是第二份"定义":
#   配方和图都只存在于源码树一处,这里只是拷一份给测试用。
# 暂存由文件末尾的 w7_test_data 目标完成,**每次构建都重拷** ——
# 理由见那个目标的注释(POST_BUILD 会留下陈旧数据)。
#
# ⚠ "只存在于源码树一处"这句话,以前**对图是假的**:
#   W7_VisionMasterLite/images/ 里躺着 demo 板的另一份拷贝,
#   和 W6_PCB_DefectProject/images/ 一模一样 —— 那是生成器真正住的地方。
#   两份都进 git,改动只发生在 W6 那份上,W7 那份就静悄悄地停在原地。
#   2026-10-20 把 W7 那份删了:图只有一份,在生成器那里。
# ============================================================

# 套件名累积(给末尾的暂存目标用)
set(W7_TEST_SUITES "")

function(w7_add_test suite target)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/test_work/${suite}")
    add_test(NAME ${suite} COMMAND ${target} WORKING_DIRECTORY "${_dir}")
    set(W7_TEST_SUITES ${W7_TEST_SUITES} ${suite} PARENT_SCOPE)
endfunction()

# === Release-only (与 W1–W6 一致) ===
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "W7 must use Release. Debug triggers cv::debug_build_guard ABI mismatch.")
endif()

# === 三方依赖路径(Qt6 / OpenCV) ===
# 路径是**这台机器**的事实,不是项目的事实 ——
# 所以默认值写在 common/cmake/ThirdParty.cmake 里,CI 用 -D 覆盖:
#     -DCMAKE_PREFIX_PATH=<qt6 根目录>
#     -DOpenCV_DIR=<opencv>/lib  -DOpenCV_BIN_DIR=<opencv>/bin
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/ThirdParty.cmake)

# === Qt6 ===
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# === OpenCV (与 W1–W6 完全一致) ===
find_package(OpenCV REQUIRED)

# ============================================================
# === 可选 SDK 探测 (优雅降级)
#
# 有些算子依赖第三方 SDK,装了就用,没装也不该让整个项目编译不过。
# 这里用 find_path/find_library 真实探测,而不是把宏写死成"关"。
#
# 之前 image_source_camera 里的 HAS_MVS_SDK 从来没人定义过 ——
# 也就是说哪怕把 MVS SDK 装好、路径全配对,这个算子也永远是 no-op。
# 探测逻辑补上之后,装了就自动生效,不用改一行代码。
#
# 探测顺序:环境变量 → 默认路径
# ============================================================

# --- 海康 MVS SDK (工业相机源) ---
if(DEFINED ENV{MVS_DIR})
    set(_mvs_root "$ENV{MVS_DIR}")
else()
    set(_mvs_root "D:/MVS")
endif()
find_path(MVS_INCLUDE_DIR MvCameraControl.h
    HINTS "${_mvs_root}/Include" "${_mvs_root}/Includes" NO_DEFAULT_PATH)
find_library(MVS_LIBRARY NAMES MvCameraControl
    HINTS "${_mvs_root}/Lib/x64" "${_mvs_root}/Lib" NO_DEFAULT_PATH)

# --- ONNX Runtime (DL 推理算子) ---
if(DEFINED ENV{ONNXRUNTIME_DIR})
    set(_ort_root "$ENV{ONNXRUNTIME_DIR}")
else()
    set(_ort_root "D:/onnxruntime")
endif()
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${_ort_root}/include" NO_DEFAULT_PATH)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
    HINTS "${_ort_root}/lib" NO_DEFAULT_PATH)

if(MVS_INCLUDE_DIR AND MVS_LIBRARY)
    message(STATUS "✅ MVS SDK 已找到 — image_source_camera 启用真实相机采集")
else()
    message(STATUS "⚠️  MVS SDK 未找到 (找过 ${_mvs_root}) — image_source_camera 编译为空实现")
endif()

if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    message(STATUS "✅ ONNX Runtime 已找到 — dl_infer 启用真实推理")
else()
    message(STATUS "⚠️  ONNX Runtime 未找到 (找过 ${_ort_root}) — dl_infer 编译为空实现")
endif()

# ============================================================
# === 共享判定核心 (common/pcb_core)
#
# PCB 判定的真值(板型配方、颜色判据、逐条规则)在 common/pcb_core/ 下,
# W6(命令行)和 W7(算子)**共用同一份**。
#
# 为什么要共用:W6 那边原本是 `if (label == "R2")` 七处硬编码,
# 换块板规则全空转 —— 三块好板全被判废、两块废板输出与好板逐字相同。
# W7 这边把规则改成配方驱动 + 逐条标定,花了不少功夫。
# 也就是说能力在 W7、数据在 W6,而 W6 自己用不上。
#
# 硬把 W7 的算法抄一份到 W6,就是本项目栽过四遍的"抄副本" ——
# 抄出来的那份会随 W7 的标定一起漂,漂了没人知道。
#
# 这一层刻意不依赖 Qt:W6 不引 Qt,引了就是给一个学习项目加依赖;
# W7 的算子只做一层薄适配(QJsonObject <-> 纯 C++ 结构体)。
# ============================================================
set(PCB_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/pcb_core")

add_library(w7_pcb_core STATIC
    ${PCB_CORE_DIR}/pcb_core.cpp
    ${PCB_CORE_DIR}/mini_json.cpp
)

target_include_directories(w7_pcb_core PUBLIC
    ${PCB_CORE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_pcb_core PUBLIC ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w7_pcb_core PRIVATE /utf-8)
endif()

# ------------------------------------------------------------
# 共享 QR/OCR 核心 (common/qr_ocr)
#
# QR 解码 + tesseract 文字识别,W5 / W6 / W7 **共用同一份**。
#
# 这是本项目**第五次**抄同一个副本。前三次分别长在:
#   · W5 的 main.cpp(QR 解码 + tesseract 调用各一份)
#   · W6 的 w6_cli.cpp(抄 W5 的,修了宽字符转换和 --psm 11)
#   · W7 的 qr_decode 算子(又抄了一份 QR 解码)
#
# 抄出来的代价不是"多几行代码":
#   · W6 的 --psm 11 和 W5 的 --psm 6 是**两个不同的答案**,
#     谁也说不清哪个对 —— 它们是两份拷贝,不是一份的两个参数。
#   · W6 那份修了宽字符转换的坑(路径带中文会毁),
#     W5 那份还在逐元素窄化。修一处,另一处照旧坏着。
#
# 同样刻意不依赖 Qt —— W5 和 W6 都不引 Qt。
# ------------------------------------------------------------
set(QR_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/qr_ocr")

add_library(w7_qr_ocr STATIC
    ${QR_OCR_DIR}/qr_ocr.cpp
)

target_include_directories(w7_qr_ocr PUBLIC
    ${QR_OCR_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_qr_ocr PUBLIC ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w7_qr_ocr PRIVATE /utf-8)
endif()

# 同一份测试源码也链进 W6 和 W7 两个目标,理由和 pcb_core 一样:
#   · W7 那边是 VS 生成器,证明核心在 VS + Qt 环境下也能编;
#   · W6 那边是 Ninja + 无 Qt,证明核心能脱离 Qt 单独用。
# 两边都绿才说明"核心自己是对的,而且换个构建环境也没坏"。
add_executable(w7_qr_ocr_test
    ${QR_OCR_DIR}/tests/test_qr_ocr.cpp
)

target_include_directories(w7_qr_ocr_test PRIVATE ${QR_OCR_DIR})

target_link_libraries(w7_qr_ocr_test PRIVATE ${OpenCV_LIBS} w7_qr_ocr)

if(MSVC)
    target_compile_options(w7_qr_ocr_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_qr_ocr_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_qr_ocr_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(qr_ocr w7_qr_ocr_test)

# ------------------------------------------------------------
# 共享核心**自己**的测试
#
# 为什么单独有一个套件:
#   ① mini_json 是新写的 200 行 JSON 解析器,写它的时候一行测试都没有。
#      解析器"静默解析错"的代价特别大:配方被解析成半个样子 ->
#      判定输出"零缺陷 PASS" -> 废板判合格,整条链上**没有一处会报错**。
#   ② parseRecipeJson 的 6 类错误分支(缺字段 / 位号重复 / 规则名拼错 ...)
#      W7 原来那份 board_recipe.cpp 里也有,但同样没被测过 ——
#      现有套件只走 happy path。
#
# ⚠ 这个套件**不依赖 Qt**:共享核心本身就不依赖 Qt,测试要能证明
#   "核心能脱离 Qt 单独用"(这正是 W6 的用法)。
#   所以这里用普通的 add_executable,不是 qt_add_executable。
# ------------------------------------------------------------
add_executable(w7_core_test
    ${PCB_CORE_DIR}/tests/test_pcb_core.cpp
)

target_include_directories(w7_core_test PRIVATE ${PCB_CORE_DIR})

target_link_libraries(w7_core_test PRIVATE ${OpenCV_LIBS} w7_pcb_core)

if(MSVC)
    target_compile_options(w7_core_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_core_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_core_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 工作目录由 w7_add_test() 给(在 build/test_work/pcb_core/)。
# 测试自己从当前目录往上找仓库根(它会被链进 W6 和 W7 两个目标,
# 两个目标的 ctest 工作目录不同)—— 找法见 test_pcb_core.cpp。
w7_add_test(pcb_core w7_core_test)

# === 源码 ===
set(SOURCES
    main.cpp
    mainwindow.cpp
    pipeline_view.cpp
    operator_node.cpp
    connection_line.cpp
    pipeline_serialize.cpp # 存盘/读盘:边要记两端方向,否则竖线存一次就躺了
    pipeline_serialize.h
    connection_payload.h   # 头文件,列出来是为了 IDE 里能跳转 + 改完能触发重编
    select_delete_plan.h   # 批量删除的"计划"层:所有裸指针读取只发生在这一个函数里
    operator_library_groups.h  # 算子库分组:不变式是"没有算子会消失"
    run_policy.h           # Run 帧循环的三条"不许 runaway"规则,无头测试要 include
    frame_record.h         # 帧循环"记一笔" + 序号→帧号换算(可行为测试,见头文件)
    commands/command.cpp
    commands/node_commands.cpp
    commands/edge_commands.cpp
    recent_files.cpp
    operators/param_form.cpp
    operators/pin_types.cpp
    operators/operator_factory.cpp
    operators/color_segment.cpp
    operators/blob_count.cpp
    operators/count_check.cpp
    operators/template_match.cpp
    operators/qr_decode.cpp
    operators/image_source_dir.cpp
    operators/image_source_camera.cpp
    operators/image_source_video.cpp
    operators/pcb_inspect.cpp
    operators/pcb_defect_judge.cpp
    # 板型配方解析。进 SOURCES 而不是只在测试目标里链:
    # 主程序也要读 recipes/*.json,漏了这个是链接期 LNK2019。
    operators/board_recipe.cpp
    operators/dl_postprocess.cpp
    operators/dl_infer.cpp
    operators/line_fit.cpp
    operators/circle_fit.cpp
    operators/caliper_dist.cpp
    operators/point_point_dist.cpp
    operators/point_line_dist.cpp
    operators/angle.cpp
)

set(HEADERS
    mainwindow.h
    pipeline_view.h
    operator_node.h
    connection_line.h
    connection_payload.h
    run_policy.h
    commands/command.h
    commands/node_commands.h
    commands/edge_commands.h
    recent_files.h
    operators/operator_base.h
    operators/operator_defaults.h   # 旋钮默认值的唯一一份 —— 列出来才在 IDE 里看得见
    operators/param_read.h          # 带类型检查地读旋钮(类型不对 → 报错,不补默认值)
    operators/param_form.h
    operators/pin_types.h
    operators/operator_factory.h
    operators/color_segment.h
    operators/blob_count.h
    operators/count_check.h
    operators/template_match.h
    operators/qr_decode.h
    operators/image_source_dir.h
    operators/image_source_camera.h
    operators/image_source_video.h
    operators/pcb_inspect.h
    operators/pcb_defect_judge.h
    operators/board_recipe.h
    operators/dl_postprocess.h
    operators/dl_infer.h
    operators/line_fit.h
    operators/circle_fit.h
    operators/caliper_dist.h
    operators/point_point_dist.h
    operators/point_line_dist.h
    operators/angle.h
)

qt_add_executable(w7_vm_lite WIN32 ${SOURCES} ${HEADERS})

target_include_directories(w7_vm_lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_vm_lite PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
    w7_pcb_core
    w7_qr_ocr
)

if(MSVC)
    target_compile_options(w7_vm_lite PRIVATE /utf-8)
endif()

# --- 把探测结果接给编译器 + 链接器 ---
#
# 抽成函数,是因为**主程序和旋钮审计套件都要用同一份探测结果**。
# 旋钮审计要判断"当前构建走的是哪个分支"(比如 dl_infer 那两支
# run() 只有一支参与编译),判据就是算子自己的 runtimeAvailable()。
# 如果两个目标的探测结果不一致,审计会把分支认反 ——
# 那比没有审计更坏:它会自信地给出相反的结论。
function(w7_attach_external_sdks target)
    if(MVS_INCLUDE_DIR AND MVS_LIBRARY)
        target_compile_definitions(${target} PRIVATE HAS_MVS_SDK=1)
        target_include_directories(${target} PRIVATE ${MVS_INCLUDE_DIR})
        target_link_libraries(${target} PRIVATE ${MVS_LIBRARY})
    endif()

    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        target_compile_definitions(${target} PRIVATE HAS_ONNXRUNTIME=1)
        target_include_directories(${target} PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(${target} PRIVATE ${ONNXRUNTIME_LIBRARY})
    endif()
endfunction()

w7_attach_external_sdks(w7_vm_lite)

set_target_properties(w7_vm_lite PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# 拷 OpenCV DLL + 测试图(与 W1–W6 一致)
add_custom_command(TARGET w7_vm_lite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_vm_lite>
    COMMENT "Copying OpenCV DLLs to output directory"
)

add_custom_command(TARGET w7_vm_lite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/../W6_PCB_DefectProject/images
        $<TARGET_FILE_DIR:w7_vm_lite>/images
    COMMENT "Copying test images to output directory"
)

# ============================================================
# === 撤销/重做 回归测试 (headless) ===
#
# 只编译画布 + Command 层,不编译算子 —— 这些文件本身不依赖 cv::,
# 所以测试目标不需要 OpenCV,构建和运行都很快(约 1 秒)。
# 用 QT_QPA_PLATFORM=offscreen 跑,不需要显示器。
#
# 运行: build/w7_undo_test.exe   (退出码 0 = 全过)
# ============================================================
qt_add_executable(w7_undo_test
    tests/test_undo_redo.cpp
    pipeline_view.cpp
    operator_node.cpp
    connection_line.cpp
    # 存盘/读盘要能无头跑,才能测"竖线存一次再打开还竖着"
    pipeline_serialize.cpp
    commands/command.cpp
    commands/node_commands.cpp
    commands/edge_commands.cpp
    # pipeline_view 用到 pinTypeName 显示端口类型;
    # pin_types.cpp 只依赖 QtCore,不引入 OpenCV
    operators/pin_types.cpp
)

target_include_directories(w7_undo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(w7_undo_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(MSVC)
    target_compile_options(w7_undo_test PRIVATE /utf-8)
endif()

enable_testing()
w7_add_test(undo_redo w7_undo_test)

# ============================================================
# === PCB 算子 回归测试 (headless, 端到端跑 W6 样本)
#
# 这次要真跑 OpenCV,所以链接 OpenCV(与 undo 测试不同)。
# 测的是 W6 的 8 张样本图:baseline 必须零缺陷,
# 7 张缺陷图各自必须命中自己那类 —— 等于把 W6 当时的
# 肉眼验收清单变成可重复执行的测试。
#
# 算子会往 images/ 里写画框图,所以工作目录要能写 —— 由 w7_add_test()
# 指向 build/test_work/pcb_operators/,产物不再落进源码树。
# ============================================================
qt_add_executable(w7_pcb_test
    tests/test_pcb_operators.cpp
    operators/param_form.cpp
    operators/pcb_inspect.cpp
    operators/pcb_defect_judge.cpp
    # 板型配方解析:pcb_inspect 要读 recipes/*.json,必须一起链进去,
    # 否则链接期才报 LNK2019(症状和 CMakeLists 顶部那个 ABI 报错混在一起,难查)
    operators/board_recipe.cpp
)

target_include_directories(w7_pcb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_pcb_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
    w7_pcb_core
)

if(MSVC)
    target_compile_options(w7_pcb_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_pcb_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_pcb_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(pcb_operators w7_pcb_test)

# ============================================================
# === 板型配方随二进制一起拷一份
#
# pcb_inspect 按相对路径 "recipes/xxx.json" 读板型真值(和 images/ 同一套约定:
# 工作目录由调用方保证)。所以拷到输出目录旁边,免得换个地方起二进制就
# 读不到 —— 读不到的表现是"配方加载失败",不是"板型不支持",两种混起来
# 会让人查错方向。
# ============================================================
add_custom_command(TARGET w7_vm_lite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/recipes
        $<TARGET_FILE_DIR:w7_vm_lite>/recipes
    COMMENT "Copying board recipes to output directory"
)

add_custom_command(TARGET w7_pcb_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/recipes
        $<TARGET_FILE_DIR:w7_pcb_test>/recipes
    COMMENT "Copying board recipes to test output directory"
)

# ============================================================
# === DL 后处理 回归测试 (headless, 不需要模型也不需要 ONNX Runtime)
#
# dl_infer 的推理部分在本机跑不了(没装 ONNX Runtime),
# 但前后处理(letterbox / NMS / YOLOv5-v8 布局解析)是纯数学,
# 拿构造张量就能验。这部分恰恰是静默出错的重灾区 ——
# 布局搞反了不报错,只是全检测不出来。
#
# 结论:推理跑不了不算缺口,前后处理没测才算。
# ============================================================
qt_add_executable(w7_dl_test
    tests/test_dl_postprocess.cpp
    operators/dl_postprocess.cpp
)

target_include_directories(w7_dl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_dl_test PRIVATE Qt6::Core ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w7_dl_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_dl_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_dl_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(dl_postprocess w7_dl_test)

# ============================================================
# === 图像源 回归测试 (headless, 自造临时视频)
#
# 本次给 Run 引擎加了帧循环,契约是:
#   源输出 frame_count,引擎按 inputs["frame_index"] 逐轮调用。
# 这个契约以前只写在注释里没实现 —— image_source_dir 的注释
# 宣称"每张图都让下游算子跑一次",实际永远只跑第一张。
# 注释会骗人,断言不会。
# ============================================================
qt_add_executable(w7_source_test
    tests/test_image_sources.cpp
    operators/param_form.cpp
    operators/image_source_video.cpp
    operators/image_source_dir.cpp
)

target_include_directories(w7_source_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_source_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_source_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_source_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_source_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 测试会往 images/ 里造临时视频和图 —— 工作目录在
# build/test_work/image_sources/,不落源码树。
w7_add_test(image_sources w7_source_test)

# ============================================================
# === 每步结果查看 回归测试 (headless, 纯逻辑)
#
# 只测 OperatorNode —— 状态点判定 + UUID 键。
# 不测 MainWindow:那需要真窗口和真算子,测出来的多半是 Qt 自己
# 的行为,不是我们的逻辑。
#
# 状态点那段为什么值得测:它错了不崩,只是让一条全判废的流水线
# 画出一排绿点 —— 要到产线漏检才发现。
# ============================================================
qt_add_executable(w7_step_test
    tests/test_step_result.cpp
    operator_node.cpp
    # operator_node::itemChange 里会调 ConnectionLine::onNodesMoved(),
    # 只编 operator_node.cpp 会 LNK2019 —— 连线类的源必须跟节点一起进目标
    connection_line.cpp
    operators/pin_types.cpp
)

target_include_directories(w7_step_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(w7_step_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(MSVC)
    target_compile_options(w7_step_test PRIVATE /utf-8)
endif()

w7_add_test(step_result w7_step_test)

# ============================================================
# === pin 类型检查 + 最近文件列表 回归测试 (headless, 纯 QtCore)
#
# 这两块都不碰界面,也不需要 OpenCV,所以这个目标只要 Qt6::Core。
# 重点是"哪些该拦、哪些不该拦":
#   放错了 → 用户明明连对了却被弹窗挡住
#   拦漏了 → 静默算错(比如图像源接数量判定,count 拿不到 → 判 FAIL,
#             而日志上看不出任何异常)
# ============================================================
qt_add_executable(w7_pin_test
    tests/test_pin_and_recent.cpp
    operators/pin_types.cpp
    recent_files.cpp
)

target_include_directories(w7_pin_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(w7_pin_test PRIVATE Qt6::Core)

if(MSVC)
    target_compile_options(w7_pin_test PRIVATE /utf-8)
endif()

w7_add_test(pin_and_recent w7_pin_test)

# ============================================================
# === line_fit 回归测试 (headless, 纯几何)
#
# 这个算子只有两行公式,验算术没意义 —— 值得测的是**约定**:
# 角度 0~180 还是 0~360、两点重合报不报错、没给图报不报错。
# 这些都不崩,只是结果悄悄变了,而测量结果直接决定判废。
#
# 不需要图片文件,工作目录给不给都行 —— 还是给一个
# (build/test_work/line_fit/),免得有的套件在源码树跑、有的在 build 跑。
# ============================================================
qt_add_executable(w7_line_test
    tests/test_line_fit.cpp
    operators/param_form.cpp
    operators/line_fit.cpp
)

target_include_directories(w7_line_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_line_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_line_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_line_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_line_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(line_fit w7_line_test)

# ============================================================
# === 测量类算子 回归测试 (headless, circle_fit + caliper_dist)
#
# 和 line_test 合并成两个套件(而不是拆成三个)是因为这两个
# 考的是同一种东西:**公式简单,但约定和退化情形容易错**。
#
# circle_fit 自造 mask 写到 images/,工作目录在
# build/test_work/measurement/。
# ============================================================
qt_add_executable(w7_measure_test
    tests/test_measurement.cpp
    operators/param_form.cpp
    operators/circle_fit.cpp
    operators/caliper_dist.cpp
    operators/point_point_dist.cpp
    operators/point_line_dist.cpp
    operators/angle.cpp
)

target_include_directories(w7_measure_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_measure_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_measure_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_measure_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_measure_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(measurement w7_measure_test)

# ============================================================
# === Run 帧循环策略 回归测试 (headless, 纯逻辑 + 一个真日志控件)
#
# 2026-10-08"跑批卡死"的修复配套。这三条规则(帧数封顶 / UI 抽帧限频 /
# 可取消+禁重入)的特点是**都不崩** —— 不封顶照样跑完,只是要等;
# 不限频照样出结果,只是界面卡。所以"跑一遍看有没有报错"永远发现不了,
# 必须把规则本身拆成纯函数钉住。
#
# 用例 5/6 会真的构造一个 QPlainTextEdit 写几千行,验证我依赖的
# "setMaximumBlockCount 真的能把文档卡住"这个假设 —— 不验它,
# 整个修复就建在一个没验证过的前提上。
#
# 注意这个目标必须排在下面 set_tests_properties 之前:
# 那里的 ENVIRONMENT 要按名字引用 run_policy,add_test 还没执行时
# 引用一个不存在的测试名,CMake 直接报 "Configuring incomplete"。
# ============================================================
qt_add_executable(w7_run_test
    tests/test_run_policy.cpp
)

target_include_directories(w7_run_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/operators   # artifact_path.h(run_policy 也测它)
)

target_link_libraries(w7_run_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# === 告诉测试源码目录在哪 ===
#
# run_policy 里有两条**源码级守卫**,它们要读 mainwindow.cpp 的源码。
# 以前是"从当前目录往上找 mainwindow.cpp" —— 那个做法有个前提:
# **CWD 在源码树里**。
#
# 2026-10-20 这个前提不成立了:
#   · 测试工作目录挪到了 build/test_work/<套件名>/
#   · CI 的构建目录是 build/ci/W7_VisionMasterLite/ ——
#     它和**源码目录同名**!从 build/ci/W7_VisionMasterLite/test_work/
#     往上走,经过的是**构建目录**,一路走到 E:/ 都不会碰到源码目录。
#
# 于是守卫直接失效(而且是"恒真式地失效":报红,但红得莫名其妙 ——
# 消息说"往上 8 层都找不到",其实源码就在 6 层外,只是那个 6 层
# 是**另一个**同名目录)。
#
# 现在不猜了:**构建系统知道源码在哪,直接告诉测试**。
target_compile_definitions(w7_run_test PRIVATE
    W7_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(w7_run_test PRIVATE /utf-8)
endif()

# 工作目录必须显式给:run_policy 里有一条**源码级守卫**
# (test_mainwindow_records_every_frame)要读 mainwindow.cpp 源码。
# 不给的话 ctest 会在 build/ 下跑,找不到文件 —— 而那条守卫
# 第一版写成了"打不开就 return",于是它变成**恒真的空断言**:
# 测试绿着,而守卫根本没生效。这正是"看着在验证其实没验证"。
#
# ⚠ 所以这里不是"给个目录就完事":守卫必须**自己去找**源码,
#   不能靠 CWD 恰好是工程根。改成从当前目录往上找 mainwindow.cpp
#   (见 test_run_policy.cpp 的 findMainWindowCpp()),
#   这样工作目录挪到 build/test_work/ 之后守卫照样真的在跑。
w7_add_test(run_policy w7_run_test)

# ============================================================
# === 检测类算子 回归测试 (color_segment / blob_count / count_check)
#
# 这三个是**启动时那条默认流水线**上的算子,而前 9 个套件一个都没测过它们。
# 两个共性决定了这层最难靠手点发现:
#   1) 参数从 JSON 来、形状不保证(旧工程/手改文件)→ run() 里越界
#   2) 中间结果靠落盘文件名传递 → 文件名不唯一就互相覆盖,
#      下游读到别人的结果,不崩不报错,数就是错的
#
# 测试要往 images/ 写临时图,工作目录在 build/test_work/detection_operators/。
# ============================================================
qt_add_executable(w7_detect_test
    tests/test_detection_operators.cpp
    operators/param_form.cpp
    operators/color_segment.cpp
    operators/blob_count.cpp
    operators/count_check.cpp
)

target_include_directories(w7_detect_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_detect_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_detect_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_detect_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_detect_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(detection_operators w7_detect_test)

# ============================================================
# === template_match 回归测试 (headless, 自造带纹理的图和模板)
#
# 这个算子此前一个测试都没有,而它是判断类算子里最容易"安静地算错"的:
# 三种匹配方法的**值方向不一样**,代码却当成一样处理。
#
#   TM_SQDIFF_NORMED 越小越像,代码取 -minVal 得到非正数,
#   再和 [0,1] 区间的 threshold(默认 0.9)比 → 恒判 NG。
#
# 加上模板比原图大时 cv::matchTemplate 抛 cv::Exception 直接崩程序
# (Run 引擎调用点没有 try/catch),以及纯色模板 + CCOEFF = NaN 恒判不通过。
#
# 三条都不报错、状态点都是绿的,只有结论是错的 —— 产线上要到漏检才发现。
#
# 测试要往 images/ 写临时图,工作目录在 build/test_work/template_match/。
# ============================================================
qt_add_executable(w7_tpl_test
    tests/test_template_match.cpp
    operators/param_form.cpp
    operators/template_match.cpp
)

target_include_directories(w7_tpl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_tpl_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_tpl_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_tpl_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_tpl_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(template_match w7_tpl_test)

# ============================================================
# === 算子库分组 回归测试 (headless, 纯 QtCore)
#
# 以前分组是硬编码的 5 个 if/else,而注册表里写了 category = "dl"。
# 6 个分类只认 5 个 → 树节点指针停在 nullptr → `if (parent)` 不成立
# → dl_infer 凭空消失,连日志都不打。
#
# 值得测的不是"dl_infer 出现了"(那是自证),而是
# **"没有算子会消失"这个不变式对任何输入都成立** ——
# 包括传一个分组表里根本没有的分类进去。
#
# 纯函数 + 只依赖 QtCore,所以这个目标只要 Qt6::Core,
# 而且连 QApplication 都不用,起得比别套件还快。
# ============================================================
qt_add_executable(w7_lib_test
    tests/test_operator_library.cpp
)

target_include_directories(w7_lib_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(w7_lib_test PRIVATE Qt6::Core)

if(MSVC)
    target_compile_options(w7_lib_test PRIVATE /utf-8)
endif()

w7_add_test(operator_library w7_lib_test)

# ============================================================
# === 旋钮审计 回归测试 (headless)
#
# 查的是「界面上摆着的旋钮,和背后那段代码到底对不对得上」:
#   1. 死旋钮       —— defaultParams() 登记了,run() 却没读
#   2. 没登记的旋钮 —— run() 读了,defaultParams() 没登记
#   3. 默认值抄多份 —— defaultParams() 里出现裸字面量
# 另外两条行为测试:空参数跑出来的结果,必须和声明的默认参数一样。
#
# 这三样都不是假想敌。落地前实测已经有三处副本漂了:
#   blob_count.min_area      声明 20   兜底 50
#   count_check.expected_min 声明 1    兜底 0
#   count_check.expected_max 声明 100  兜底 999
# 还有一处"登记的和读的不是同一个 key":pcb_inspect 登记 min_area、
# 读 min_area_pct,而 min_area_pct 从来没登记过。
#
# 为什么这个目标要链**全部算子**:用例 5 是真跑算子。
# 只扫源码抓不到"兜底值和声明值不一样"这种病 ——
# 源码里两处都写着 20,扫源码看不出 run() 读的是 50。
#
# 审计要读 operators/*.cpp 的源码,所以同样需要 W7_SOURCE_DIR
# (理由见上面 run_policy 那段:构建目录和源码目录同名,
# 从 CWD 往上找会找到另一个同名目录)。
# ============================================================
qt_add_executable(w7_knob_test
    tests/test_operator_knobs.cpp
    operators/param_form.cpp
    operators/pin_types.cpp
    operators/operator_factory.cpp
    operators/color_segment.cpp
    operators/blob_count.cpp
    operators/count_check.cpp
    operators/template_match.cpp
    operators/qr_decode.cpp
    operators/image_source_dir.cpp
    operators/image_source_camera.cpp
    operators/image_source_video.cpp
    operators/pcb_inspect.cpp
    operators/pcb_defect_judge.cpp
    operators/board_recipe.cpp
    operators/dl_postprocess.cpp
    operators/dl_infer.cpp
    operators/line_fit.cpp
    operators/circle_fit.cpp
    operators/caliper_dist.cpp
    operators/point_point_dist.cpp
    operators/point_line_dist.cpp
    operators/angle.cpp
)

target_include_directories(w7_knob_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/operators
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_knob_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
    w7_pcb_core
    w7_qr_ocr
)

target_compile_definitions(w7_knob_test PRIVATE
    W7_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# 外部 SDK 的探测结果必须和主程序一致 —— 理由见 w7_attach_external_sdks。
w7_attach_external_sdks(w7_knob_test)

if(MSVC)
    target_compile_options(w7_knob_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_knob_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w7_knob_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

w7_add_test(operator_knobs w7_knob_test)

# ============================================================
# 让测试失败时能看到是哪条断言挂了
#
# Qt 6 在 Windows 上默认把 qInfo/qDebug 送到 Windows 调试通道而不是 stderr,
# 结果是 ctest --output-on-failure 只能看到一个非零退出码,断言详情一条都不打。
# 实测踩过:angle 算子退化时返回 45°(约定是 0°)导致 measurement 挂了,
# 输出里却只有 OpenCV 自己的 WARN,连 "[FAIL]" 都看不到,只能靠猜。
#
# QT_FORCE_STDERR_LOGGING=1 把日志打回 stderr,失败详情才进得了 ctest 输出。
# ============================================================
set_tests_properties(
    undo_redo pcb_operators dl_postprocess image_sources
    step_result pin_and_recent line_fit measurement run_policy detection_operators
    template_match operator_library operator_knobs
    PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
)

# ============================================================
# === 暂存测试数据(每次构建都重拷)
#
# ## 为什么不是 POST_BUILD
#
# POST_BUILD 挂在目标上,**只在目标重新链接时才跑**。
# 改了 recipes/demo.json、或者换了一张测试图,而代码一行没动
# -> 不重编 -> 不重拷 -> 测试读到的还是**上一次的旧数据**。
#
# 旧数据照样能把测试跑绿,而且**一声不响**:
#   配方改了判据,测试拿旧配方判,结论是旧配方下的正确答案;
#   测试图换了,测试拿旧图,判的还是旧图。
# 这就是本项目最怕的那种"静默失效"—— 看着在验证,其实没在验证。
#
# ## 为什么不是 configure 时拷
#
# configure 时拷 = 只在"有人跑 cmake"时才更新。
# 改了图直接 ctest,拿到的还是旧数据,而且没有任何提示。
#
# ## 那为什么每次都重拷
#
# add_custom_target 没有输出文件,CMake 永远认为它"过期",
# 所以**每次构建都会跑**。数据永远不会是陈的。
#
# 代价:每次构建多几十个 cmake -E copy_directory(实测 1 秒上下)。
# 换来的是"改了数据,下一次构建就生效",而且这个性质是**无条件**的。
# ============================================================
set(_stage_cmds "")
foreach(_suite IN LISTS W7_TEST_SUITES)
    set(_d "${CMAKE_CURRENT_BINARY_DIR}/test_work/${_suite}")
    list(APPEND _stage_cmds
        # 板型配方(pcb_inspect 按相对路径 "recipes/xxx.json" 读)
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/recipes" "${_d}/recipes"
        # 测试图:**唯一来源是 W6 的 images/**(那边才是生成器所在地)。
        # 一次拷全 —— complex/ 三块板本来就在这个目录下面,不用再单独拷一次。
        #
        # ⚠ 2026-10-20 之前这里拷的是 "${CMAKE_SOURCE_DIR}/images",
        #   也就是 W7 自己那份**副本**。那份副本停在 2026-09-14:
        #   W6 那边重画了短路缺陷图、改了配方,W7 的测试拿到的还是**旧图**,
        #   旧图判出来的结论"照样自洽" —— 没人知道数据根本没跟上。
        #   判据变了而数据没变,现象和"判据写错了"一模一样,
        #   这就是最贵的那类 bug:看起来是代码问题,其实是数据问题。
        #
        # 所以这里只允许有一份图,生成器说在哪就在哪。
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/../W6_PCB_DefectProject/images" "${_d}/images")
endforeach()

add_custom_target(w7_test_data ALL ${_stage_cmds}
    COMMENT "Staging W7 test data into build/test_work/")
