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

target_include_directories(w5_ocr_qrcode PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w5_ocr_qrcode PRIVATE ${OpenCV_LIBS})

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