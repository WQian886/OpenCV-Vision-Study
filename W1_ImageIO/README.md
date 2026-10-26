# W1: 图像 I/O + 像素操作

## 🎯 学习目标
1. 掌握 `cv::imread` 读取图像
2. 理解 `cv::Mat` 的基本属性（尺寸/通道/位深/类型）
3. 学会像素读写（`at<>()` / `setTo` / ROI）
4. 掌握 `cv::imwrite` 保存图像
5. 掌握 `cv::imshow` 显示图像

## 📁 工程结构
```
W1_ImageIO/
├── CMakeLists.txt      # 找 OpenCV + 编译
├── main.cpp            # Hello OpenCV 主程序
├── pixel_ops.{h,cpp}   # 练习 3 的判据和涂黑逻辑（可测的那一层）
├── tests/
│   └── test_w1_imageio.cpp   # 无头回归测试
├── images/             # 测试图片(自动生成)
└── README.md           # 本文件
```

## 🛠️ 如何构建（VS 方式）

1. VS 2022/2026 → **打开本地文件夹** → 选 `E:\qianwie\00study\OpenCV\W1_ImageIO`
2. VS 自动识别 CMakeLists.txt，弹出"已为此文件夹配置 CMake"的提示
3. 等 CMake 配置完成（右下角状态栏会显示）
4. 按 **F7** 编译（生成 `out/build/x64-Debug/w1_imageio.exe`）
5. 按 **Ctrl+F5**（开始执行不调试）→ 看到图片窗口

> 💡 必须用 **Ctrl+F5** 而不是 F5，否则调试器启动慢且会停在断点。

## 🛠️ 命令行构建（备选，VS 的 CMake 也支持）

如果你装了 cmake 命令行工具：
```powershell
cd E:\qianwie\00study\OpenCV\W1_ImageIO
D:\Qt\Tools\Ninja\ninja.exe  # 用的就是你 OpenCV 编译时的 Ninja
# 或者用 cmake:
cmake -S . -B build -G Ninja
cmake --build build
build\w1_imageio.exe
```

## 🧪 跑测试

W1 有 **1 个 ctest 套件、29 条断言**（2026-10-26 之前是 **0 个**）。

```powershell
cd E:\qianwie\00study\OpenCV\W1_ImageIO
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# ⚠ 必须带 -C Release,否则套件全报 Not Run
ctest --test-dir build -C Release --output-on-failure
```

| 套件 | 断言 | 测什么 |
|---|---|---|
| `w1_imageio` | 29 | **练习 3 那段代码自己的测试**:造出来的图对不对、**BGR 不是 RGB**(判据写成 RGB 会把白底整片判成蓝)、**边界是闭的**(B 正好 100 不算蓝、G/R 正好 150 不算蓝)、**涂黑是幂等的**(再涂一次是 0 个)、**imwrite→imread 往返逐像素相同**(PNG 无损)、读不存在的文件得到空 Mat 而不是崩溃 |

> 🔧 **为什么练习 3 的代码被拆出去了**：
> 它原本整段写在 `main.cpp` 里 —— 一个只有 20 行的程序没人测，
> 不是因为它不重要，是因为**它写在了 `main()` 里**：
> 写测试的人得先把它拆出来。
>
> 现在判据在 `pixel_ops.cpp` 的 `isBluePixel()`，涂黑在 `paintBlueToBlack()`，
> **demo 和测试调的是同一份源码**（同一个 `pixel_ops.cpp` 链进两个目标）——
> 不在测试里再抄一遍判据。抄出来的副本会随核心漂移：
> 改了判据，测试还在按旧判据断言，于是测试「绿着」而 demo 已经变了。
> **绿灯什么也没证明。**

> 🧹 **测试产物落在 `build/test_work/w1_imageio/`,不落源码树。**
> 那个套件会写一个 PNG 再读回来（断"存盘→读盘"这一链），
> 读完就删掉 —— 源码树一个字节都不变，
> CI 的"源码树必须干净"那一步对这套测试不是空转。

> 🤖 **一条命令跑完"构建 + 全部测试 + 检查源码树没被写脏"**：
> ```powershell
> powershell -ExecutionPolicy Bypass -File ..\tools\ci.ps1
> ```

## 📚 知识点笔记

### cv::Mat 是什么？
- OpenCV 的图像容器，本质是**带元信息的内存块**
- 元信息：宽、高、通道数、位深、步长（每行字节数）
- 数据：连续的（`isContinuous()`）或带 padding 的

### BGR vs RGB（重要！）
- OpenCV **默认 BGR 顺序**，不是 RGB！
- 这是历史包袱（BGR 是 Windows BMP 的原生顺序）
- 工业相机厂商也常用 BGR
- 跟 matplotlib/PIL 等用 RGB 的库对接时要转换：`cv::cvtColor(img, img, cv::COLOR_BGR2RGB)`

### at<Vec3b>(y, x) 的陷阱
- 注意是 **(y, x)** 不是 (x, y)！at<> 第一个参数是行号，第二个是列号
- 类型必须严格匹配：CV_8UC3 → `Vec3b`，CV_32FC3 → `Vec3f`
- 单点访问慢，循环里推荐用 `ptr<>()` 或 OpenCV 内置函数

### cv::imread 第二个参数
- `cv::IMREAD_COLOR` (1)：强制读 3 通道彩色
- `cv::IMREAD_GRAYSCALE` (0)：强制读灰度
- `cv::IMREAD_UNCHANGED` (-1)：保持原样（含 alpha 通道）

## ✏️ 课后练习

1. **改大小**：把左上红色方块从 100x100 改成 50x200 长条
2. **画网格**：用 `cv::line` 在图像上画一个 8x8 网格
3. **遍历像素**：遍历整张图，把蓝色通道值 > 100 的像素涂成纯黑
4. **读多种格式**：找一张 PNG/BMP 图片，验证 imread 都能读
5. **错误处理**：故意把 `imgPath` 改成不存在的路径，确认你的程序会优雅退出

## 🎓 下一节预告

W2: **色彩空间 + 滤波 + 阈值**
- BGR ↔ HSV ↔ Gray 转换
- 高斯滤波 / 中值滤波 / 双边滤波
- 全局阈值 / 自适应阈值 / Otsu
- 实战：从 PCB 图片提取金色焊盘区域