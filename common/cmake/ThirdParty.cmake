# ============================================================
# 三方依赖路径 —— 本机默认值,CI 可覆盖
#
# ## 为什么要有这个文件
#
# 七个工程的 CMakeLists 里曾经**写死**了本机的路径:
#
#     set(OpenCV_DIR "D:/opencv-src/build/install/lib")
#     set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
#     D:/opencv-src/build/install/bin      (拷 DLL 用,出现 10 多次)
#
# 本机能跑,别人跑不了 —— 因为**路径是这台机器的事实,不是项目的事实**。
# 一旦要上 CI(CI runner 上既没有 D 盘也没有 Qt),这些行就得逐个改,
# 而且改的时候没人会注意到漏掉的那一个。
#
# 改法:把路径变成 **CMake 缓存变量**,默认值还是本机的,
#       需要时用 -D 覆盖:
#
#         -DOpenCV_DIR=<...>/lib       OpenCV 的 lib 目录
#         -DOpenCV_BIN_DIR=<...>/bin   OpenCV 的 bin 目录(拷 DLL 用)
#         -DCMAKE_PREFIX_PATH=<...>    Qt6 根目录(只有 W7 需要)
#
# ## ⚠ 为什么用 set(... CACHE ...) 而不是 set(...)
#
# CMake 里**普通变量会盖掉缓存变量**。所以:
#
#     set(OpenCV_DIR "D:/...")          # 普通变量
#     # cmake -DOpenCV_DIR=C:/opencv/lib # 命令行传的值在缓存里
#
# 这一行会把 -D 传进来的值**盖掉** —— "-D 覆盖"看着写了、其实没生效。
# 这是本项目栽过的那类坑(字段在 JSON 边界上丢了,同一个形状)。
#
# 缓存变量不会:命令行 -D 的优先级最高,set(CACHE) 不带 FORCE 时
# 也不会覆盖已存在的缓存项。
#
# ## 缓存的副作用(知道就行)
#
# 缓存变量是**粘**的:第一次 configure 之后,再改这个文件里的默认值
# 不生效了(缓存里已经存着旧值)。要换路径就:
#
#     cmake -S . -B build -DOpenCV_DIR=<新的>
# 或直接删掉 build 目录重新 configure。
#
# ============================================================

# ------------------------------------------------------------
# OpenCV
# ------------------------------------------------------------
#
# ⚠ OpenCV_DIR 必须指向 **lib 子目录**,不是安装根目录。
#   install/ 下的 OpenCVConfig.cmake 是 vcpkg 风格的包装器,
#   会做平台/运行时检测,它的正则不认 MSVC 19.51(VS 2026 / VS 18);
#   install/lib/OpenCVConfig.cmake 才是真正的 config。
if(NOT DEFINED OpenCV_DIR)
    set(OpenCV_DIR "D:/opencv-src/build/install/lib"
        CACHE PATH "OpenCV 安装目录下的 lib 子目录(不是安装根目录)")
endif()

# DLL 目录:从 OpenCV_DIR 退一级推出来。
# install 布局是 <prefix>/lib + <prefix>/bin,所以退一级正好是 prefix。
get_filename_component(_ocv_prefix "${OpenCV_DIR}" DIRECTORY)
if(NOT DEFINED OpenCV_BIN_DIR)
    set(OpenCV_BIN_DIR "${_ocv_prefix}/bin"
        CACHE PATH "OpenCV 安装目录下的 bin 子目录(拷 DLL 用)")
endif()

# ------------------------------------------------------------
# Qt6
# ------------------------------------------------------------
#
# 只有 W7 需要。
#
# 这里**故意不用 CACHE**:CMAKE_PREFIX_PATH 会被环境变量初始化成
# 一个缓存项,用 set(CACHE) 反而可能跟环境变量打架。
# 普通变量 + if 守卫:命令行 -D 给的值会被缓存项挡住(env 优先级更高),
# 都没给时才用本机默认值 —— 而且不会像缓存那样"粘住"。
if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
endif()
