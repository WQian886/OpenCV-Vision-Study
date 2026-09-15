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

set(SOURCES main.cpp)
add_executable(w6_pcb_defect ${SOURCES})

target_include_directories(w6_pcb_defect PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(w6_pcb_defect PRIVATE ${OpenCV_LIBS})

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

# 拷 OpenCV DLL 到产物目录
add_custom_command(TARGET w6_pcb_defect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        D:/opencv-src/build/install/bin
        $<TARGET_FILE_DIR:w6_pcb_defect>
    COMMENT "Copying OpenCV DLLs to output directory"
)
