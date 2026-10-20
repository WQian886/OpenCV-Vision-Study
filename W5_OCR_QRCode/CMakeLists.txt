cmake_minimum_required(VERSION 3.21)
project(w5_ocr_qrcode LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "W5 must use Release. Debug triggers cv::debug_build_guard ABI mismatch.")
endif()

set(OpenCV_DIR "D:/opencv-src/build/install/lib")
find_package(OpenCV REQUIRED)

set(SOURCES main.cpp)
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

add_library(w5_qr_ocr STATIC
    ${QR_OCR_DIR}/qr_ocr.cpp
)

target_include_directories(w5_qr_ocr PUBLIC
    ${QR_OCR_DIR}
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
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w5_ocr_qrcode>
    COMMENT "Copying OpenCV DLLs to output directory"
)