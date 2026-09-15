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
├── CMakeLists.txt    # 找 OpenCV + 编译
├── main.cpp          # Hello OpenCV 主程序
├── images/           # 测试图片(自动生成)
└── README.md         # 本文件
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