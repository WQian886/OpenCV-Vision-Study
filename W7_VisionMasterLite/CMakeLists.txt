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

# === 源码 ===
set(SOURCES
    main.cpp
    mainwindow.cpp
    pipeline_view.cpp
    operator_node.cpp
    connection_line.cpp
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
    commands/command.h
    commands/node_commands.h
    commands/edge_commands.h
    recent_files.h
    operators/operator_base.h
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
)

if(MSVC)
    target_compile_options(w7_vm_lite PRIVATE /utf-8)
endif()

# --- 把探测结果接给编译器 + 链接器 ---
if(MVS_INCLUDE_DIR AND MVS_LIBRARY)
    target_compile_definitions(w7_vm_lite PRIVATE HAS_MVS_SDK=1)
    target_include_directories(w7_vm_lite PRIVATE ${MVS_INCLUDE_DIR})
    target_link_libraries(w7_vm_lite PRIVATE ${MVS_LIBRARY})
endif()

if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    target_compile_definitions(w7_vm_lite PRIVATE HAS_ONNXRUNTIME=1)
    target_include_directories(w7_vm_lite PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(w7_vm_lite PRIVATE ${ONNXRUNTIME_LIBRARY})
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
add_test(NAME undo_redo COMMAND w7_undo_test)

# ============================================================
# === PCB 算子 回归测试 (headless, 端到端跑 W6 样本)
#
# 这次要真跑 OpenCV,所以链接 OpenCV(与 undo 测试不同)。
# 测的是 W6 的 8 张样本图:baseline 必须零缺陷,
# 7 张缺陷图各自必须命中自己那类 —— 等于把 W6 当时的
# 肉眼验收清单变成可重复执行的测试。
#
# 算子会往 images/ 里写画框图,所以工作目录必须是工程根。
# ============================================================
qt_add_executable(w7_pcb_test
    tests/test_pcb_operators.cpp
    operators/param_form.cpp
    operators/pcb_inspect.cpp
    operators/pcb_defect_judge.cpp
)

target_include_directories(w7_pcb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w7_pcb_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${OpenCV_LIBS}
)

if(MSVC)
    target_compile_options(w7_pcb_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w7_pcb_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_pcb_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

add_test(NAME pcb_operators COMMAND w7_pcb_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_dl_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

add_test(NAME dl_postprocess COMMAND w7_dl_test)

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
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_source_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 测试会往 images/ 里造临时视频和图,工作目录必须是工程根
add_test(NAME image_sources COMMAND w7_source_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
    operators/pin_types.cpp
)

target_include_directories(w7_step_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(w7_step_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(MSVC)
    target_compile_options(w7_step_test PRIVATE /utf-8)
endif()

add_test(NAME step_result COMMAND w7_step_test)

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

add_test(NAME pin_and_recent COMMAND w7_pin_test)

# ============================================================
# === line_fit 回归测试 (headless, 纯几何)
#
# 这个算子只有两行公式,验算术没意义 —— 值得测的是**约定**:
# 角度 0~180 还是 0~360、两点重合报不报错、没给图报不报错。
# 这些都不崩,只是结果悄悄变了,而测量结果直接决定判废。
#
# 不需要图片文件,所以工作目录不设 WORKING_DIRECTORY。
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
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_line_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

add_test(NAME line_fit COMMAND w7_line_test)

# ============================================================
# === 测量类算子 回归测试 (headless, circle_fit + caliper_dist)
#
# 和 line_test 合并成两个套件(而不是拆成三个)是因为这两个
# 考的是同一种东西:**公式简单,但约定和退化情形容易错**。
#
# circle_fit 自造 mask 写到 images/,工作目录必须是工程根。
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
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w7_measure_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

add_test(NAME measurement COMMAND w7_measure_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
