cmake_minimum_required(VERSION 3.21)
project(w5_ocr_qrcode LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "W5 must use Release. Debug triggers cv::debug_build_guard ABI mismatch.")
endif()

# 路径是**这台机器**的事实,不是项目的事实 ——
# 所以默认值写在 common/cmake/ThirdParty.cmake 里,CI 用 -D 覆盖。
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/ThirdParty.cmake)
find_package(OpenCV REQUIRED)

set(SOURCES
    main.cpp
    # present_pipeline.cpp **链进两个目标**(demo 和测试),一份源码两个用法。
    #
    # 为什么不是"测试里再抄一遍画框的逻辑":
    #   抄出来的副本会随核心漂移 —— 改了画框的角点顺序,
    #   测试还在按旧顺序断言,于是测试「绿着」而 demo 已经画成了蝴蝶结。
    #   绿灯什么也没证明。
    #
    # 仓库里已有同样的做法:W2 的 color_pipeline.cpp、W3 的 edge_pipeline.cpp、
    # W4 的 feature_pipeline.cpp、W6 的 w6_cli.cpp。
    present_pipeline.cpp
)
add_executable(w5_ocr_qrcode ${SOURCES})

# ============================================================
# 共享 QR/OCR 核心 (common/qr_ocr)
#
# QR 解码 + tesseract 文字识别,W5 / W6 / W7 **共用同一份**。
#
# 这个项目原来自己写了一份,W6 抄了一份,W7 又抄了一份 ——
# 本项目栽过四遍的"抄副本",这是第五次。
#
# 抄出来的代价不是"多几行代码":
#   · W6 的 --psm 11 和 W5 的 --psm 6 是**两个不同的答案**,
#     谁也说不清哪个对 —— 它们是两份拷贝,不是一份的两个参数。
#   · W6 那份修了宽字符转换的坑(逐元素窄化会毁掉中文路径),
#     W5 这份还在用旧的写法。修一处,另一处照旧坏着。
#
# 现在 psm 是一个**参数**,W5 传 6、W6 传 11,改一处三边跟着动。
# ============================================================
set(QR_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/qr_ocr")

# 路径 <-> UTF-8 文本的**唯一一处定义**(纯头文件,没有 .cpp)。
# 共享核心里凡是要把路径变成文本(写 CSV / 拼错误消息),
# 或者把 UTF-8 文本变成路径,都走这一个头。
# 约定本身写在 utf8_path.h 的注释里 —— 为什么"窄串不承载路径"。
set(UTF8_PATH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/utf8_path")

add_library(w5_qr_ocr STATIC
    ${QR_OCR_DIR}/qr_ocr.cpp
)

target_include_directories(w5_qr_ocr PUBLIC
    ${QR_OCR_DIR}
    ${UTF8_PATH_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(w5_qr_ocr PUBLIC ${OpenCV_LIBS})

if(MSVC)
    target_compile_options(w5_qr_ocr PRIVATE /utf-8)
endif()

target_include_directories(w5_ocr_qrcode PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w5_ocr_qrcode PRIVATE ${OpenCV_LIBS} w5_qr_ocr)

if(MSVC)
    target_compile_options(w5_ocr_qrcode PRIVATE /utf-8)
endif()

set_target_properties(w5_ocr_qrcode PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_command(TARGET w5_ocr_qrcode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w5_ocr_qrcode>/images
)

add_custom_command(TARGET w5_ocr_qrcode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w5_ocr_qrcode>
    COMMENT "Copying OpenCV DLLs to output directory"
)

# ============================================================
# === 无头回归测试（2026-10-30 新增）
#
# W5 是欠账总账里「W3–W5 零测试缺口」的**最后一笔**
# (W1/W2 是 2026-10-26,W3 是 2026-10-28,W4 是 2026-10-29)。
#
# 补法不是"给 main.cpp 加个开关",而是把纯逻辑抽出来再测它 ——
# 见 present_pipeline.h 的开头为什么有这一层。
#
# ⚠ 这个套件**一条 tesseract 依赖的断言都没有** ——
#   断言总数是被文档门禁**引用**的数 ⇒ 一个随机器变的数会把门禁弄红。
#   (common/qr_ocr 的套件里那一段是需要 tesseract 的,没装就跳过。)
#
# 这个套件**不写任何文件、不开任何窗口**,
# 所以"产物不许落源码树"这条对它天然成立。
# 但工作目录照样给一份：以后有人往里加 imwrite,
# 落点已经在 build/ 里了（和 W1/W2/W3/W4/W6/W7 同一个规矩）。
# ============================================================
enable_testing()

add_executable(w5_ocr_qrcode_test
    tests/test_w5_ocr_qrcode.cpp
    present_pipeline.cpp
)

target_include_directories(w5_ocr_qrcode_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${QR_OCR_DIR}
    ${UTF8_PATH_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

# 链 w5_qr_ocr(共享核心)—— 端到端那几条要**真的**解码仓库里那张 QR 图。
target_link_libraries(w5_ocr_qrcode_test PRIVATE ${OpenCV_LIBS} w5_qr_ocr)

if(MSVC)
    target_compile_options(w5_ocr_qrcode_test PRIVATE /utf-8)
endif()

add_custom_command(TARGET w5_ocr_qrcode_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}
        $<TARGET_FILE_DIR:w5_ocr_qrcode_test>
    COMMENT "Copying OpenCV DLLs to test output directory"
)

# 工作目录必须在 configure 期就建好：
# ctest 遇到不存在的工作目录会直接报 "Unable to find executable"，
# 那是一条和测试本身无关的红 —— 看起来像代码坏了，其实一个测试都没跑。
set(W5_TEST_WORK "${CMAKE_CURRENT_BINARY_DIR}/test_work/w5_ocr_qrcode")
file(MAKE_DIRECTORY "${W5_TEST_WORK}")

add_test(NAME w5_ocr_qrcode
         COMMAND w5_ocr_qrcode_test
         WORKING_DIRECTORY "${W5_TEST_WORK}")