# W1 学习笔记：图像 I/O + 像素操作

> 📌 **这份笔记是给电气工程师的"OpenCV 入门全景图"**——不只是W1 学了什么，而是让你理解"我们到底在干啥、为啥这样学"。

---

## 🌍 Part 1：OpenCV 是干啥的？

### 一句话定义
**OpenCV**（Open Source Computer Vision Library）是一个开源的、跨平台的**计算机视觉库**。

### 类比电气工程师的工具箱
| 你的工具 | OpenCV 对应 |
|---|---|
| 万用表（测电压/电阻）| `cv::Mat` 数据结构（装图像）|
| 示波器（看波形）| `cv::imshow`（看图像）|
| 信号发生器（生成信号）| `cv::Mat` 构造（生成图像）|
| DSP 滤波器（FFT/FIR）| `cv::GaussianBlur` / `cv::filter2D`（图像滤波）|
| 串口协议解析（Modbus）| `cv::HoughCircles` / `cv::findContours`（形状识别）|

### 工业上 OpenCV 干啥的？

| 行业 | 应用 |
|---|---|
| 电子制造 | PCB 缺陷检测、芯片丝印 OCR、连接器位置检测 |
| 汽车 | 车牌识别、零件装配检测、ADAS 视觉辅助 |
| 电力 | 仪表读数（指针/数字）、输电线巡检、设备状态识别 |
| 医疗 | 影像分析、细胞计数、X 光辅助 |
| 物流 | 二维码/条形码、体积测量、包裹分拣 |
| 安防 | 人脸识别、行为分析、异常检测 |

**OpenCV 干的事就是：把摄像头（或图片）变成可决策的数据。**

---

## 🎯 Part 2：我们在干啥？

### 整体目标
**6 周时间，让你从 OpenCV 零基础，到能用 OpenCV 做 PCB 缺陷检测和 OCR/二维码识别。**

### 学习路线总览

```
W1 ─▶ W2 ─▶ W3 ─▶ W4 ─▶ W5 ─▶ W6
 │     │     │     │     │     │
 像素  颜色  边缘  匹配  OCR  综合
 操作  滤波  轮廓  特征  二维码 项目
```

| 周 | 主题 | 你能用 OpenCV 干啥 |
|---|---|---|
| **W1 ✅ 已完成** | 图像 I/O + 像素操作 | 读图、改图、存图、显示 |
| W2 | 色彩空间 + 滤波 + 阈值 | 把 PCB 元件从背景分离 |
| W3 | 边缘 + 轮廓 + 形态学 | 检测元件边界、形状 |
| W4 | 模板匹配 + 特征点 (ORB) | PCB 元件是否漏装/错装 |
| W5 | OCR (Tesseract) + 二维码 | 读 PCB 丝印、读工业标签 |
| W6 | **PCB 缺陷检测流水线** | 综合实战 |

### 你的技术栈
- **语言**：C++ 17（你之前 Qt HMI 用的就是这个）
- **平台**：Windows 11 + VS 2026（MSVC 19.51）
- **工具链**：CMake + Ninja + vcpkg
- **OpenCV**：4.13.0（你自己编译的，在 D 盘）
- **工作目录**：`E:\qianwie\00study\OpenCV`（每周一个小项目）

---

## 📚 Part 3：W1 学了什么？有什么用？

### W1 的 5 个核心知识

#### ① `cv::Mat` —— OpenCV 的图像容器
**是什么**：OpenCV 里所有图像的"容器类"，像 `std::vector` 之于数组。

**关键属性**（从你截图的输出就能看到）：
| 属性 | 含义 | 你的截图值 |
|---|---|---|
| `cols`, `rows` | 宽、高（像素）| 640 × 480 |
| `channels()` | 通道数（1=灰度, 3=BGR彩色, 4=BGRA）| 3 |
| `depth()` | 位深（0=CV_8U=8位无符号）| 0 |
| `type()` | 综合类型（CV_8UC3=8位无符号3通道）| 16 |
| `data` | 内存首地址 | 0x... |

**为什么重要**：后面所有 OpenCV 操作都是围绕 `cv::Mat` 转的。理解它 = 理解 OpenCV 的 50%。

#### ② imread / imwrite / imshow —— 图像三件套
```cpp
cv::Mat img = cv::imread("path.jpg");   // 读
cv::imwrite("out.png", img);             // 写
cv::imshow("window", img);               // 显
cv::waitKey(0);                          // 等（不写窗口会闪退）
```

**imread 第二个参数**：
- `IMREAD_COLOR (1)`：3 通道彩色（默认）
- `IMREAD_GRAYSCALE (0)`：1 通道灰度
- `IMREAD_UNCHANGED (-1)`：保持原样（含 alpha 通道）

#### ③ BGR vs RGB —— OpenCV 第一大坑
- OpenCV 用 **BGR**（蓝绿红）
- 其他库（matplotlib/PIL/浏览器）用 **RGB**（红绿蓝）
- 工业相机大多输出 RGB/BGR/YUV，要看手册

**后果**：跟其他系统对接时颜色不对，需要 `cv::cvtColor` 转换。

#### ④ 像素操作 —— 改图的3 种方式
| 方式 | 代码示例 | 速度 | 用途 |
|---|---|---|---|
| `setTo()` | `img.setTo(cv::Scalar(0,0,255))` | 最快（SIMD）| 整块填充 |
| `at<>()` | `img.at<cv::Vec3b>(y,x)[2] = 255` | 慢（边界检查）| 单点 |
| `ptr<>()` | `img.ptr<cv::Vec3b>(y)[x*3+2] = 255` | 最快（裸指针）| 循环 |

注意 `at<>(y, x)` 第一个参数是 **y（行号）**，不是 x。

#### ⑤ ROI（Region of Interest）—— 不复制数据的"窗口"
```cpp
cv::Rect roi(0, 0, 100, 100);
cv::Mat block = img(roi);       // 不复制！共享内存
block.setTo(cv::Scalar(0,0,255)); // 改 block 就是改 img
```
像示波器的"触发窗口"——换个观察视角，底层数据没动。

---

## 💡 Part 4：这些知识有啥用？（回到你的兴趣）

### 工业场景：PCB 缺陷检测的完整链路

```
摄像头 ──▶ W1 读图 ──▶ W2 颜色过滤 ──▶ W3 找边界
                                            │
                                            ▼
                                    W4 模板比对 ──▶ W5 OCR 丝印
                                            │            │
                                            ▼            ▼
                                       缺件？错件？  字符错？
                                            │            │
                                            └────┬───────┘
                                                 ▼
                                          W6 综合判定 OK/NG
                                                 ▼
                                         MES 系统 / 报警灯
```

**W1 在这个链路里的位置**：**入口**。没有"读图"什么都干不了。

**W2-W6 的工作**：处理这张图，给出"OK / NG / 报警"的判断。

---

## 🛠️ Part 5：今天踩过的坑（工程经验）

> 这些坑你 W2-W6 还会遇到，提前记下来

| 坑 | 现象 | 解法 |
|---|---|---|
| OpenCV CMake config 不识别 MSVC 19.51 | `OpenCV_FOUND to FALSE` | `set(OpenCV_DIR ".../install/lib")` 不指 install/ |
| OpenCV 只编了 Release，Debug 配置链接失败 | LNK2019 unresolved external | VS 配 Release |
| 找不到 opencv_xxx4130.dll | 启动报系统错误 | CMakeLists 加 POST_BUILD 复制 bin/ |
| 终端中文乱码 | std::cout 中文显示成?? | main 开头 `SetConsoleOutputCP(CP_UTF8)` |
| Qt 显示 OpenCV 图像 | 跟 W4+ PCB 检测相关 | `cv::Mat` → `QImage` 再放 `QLabel` |

---

## 🎯 Part 6：W1 跑通的标志

✅ 你已经做到：
- [x] 工作目录 `E:\qianwie\00study\OpenCV` 创建
- [x] OpenCV 4.13 编译 + 安装到 D 盘
- [x] W1 工程 CMakeLists + main.cpp 写好
- [x] VS 2026 编译通过
- [x] 程序跑起来，显示图片窗口
- [x] 控制台打印图像信息（640×480、3通道、CV_8UC3 等）
- [x] 左上角 100×100 涂红成功
- [x] 保存了 output_w1.png

**W1 完成度：100% 🎉**

---

## 🚀 Part 7：下一步建议

| 选项 | 适合啥时候 |
|---|---|
| **A. 直接进 W2** | 想快点实战 PCB 检测 |
| **B. 先做完 W1 课后5个练习** | 想把像素操作打牢 |
| **C. 把今天学的写成自己的笔记** | 觉得这份笔记不够"你" |

电气工程师建议：**A**。
- W2 的色彩空间转换（HSV）和阈值分割是 PCB 元件识别的前置，比像素操作更基础。
- 像素操作到 W3-W4 会反复用，不需要现在死磕。

---

## 📖 推荐课后阅读

- OpenCV 官方教程：<https://docs.opencv.org/4.x/d9/df8/tutorial_root.html>
- 《学习 OpenCV4》（清华大学出版社）—— 当字典用
- B 站"贾志刚 OpenCV 入门"—— 视频版对照

---

## 🧾 工程速查卡（保存到本地用）

```cpp
// ===== W1 速查 =====
cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);  // 读
std::cout << img.cols << "x" << img.rows;          // 尺寸
img.at<cv::Vec3b>(y, x);                            // 单像素 (y行, x列)
img.setTo(cv::Scalar(B, G, R));                     // 全填
img(cv::Rect(x, y, w, h)).setTo(...);              // ROI
cv::imwrite(path, img);                            // 写
cv::imshow("win", img); cv::waitKey(0);            // 显示
```

---

> 💬 **写在最后**：你现在已经有了一个完整可运行的 OpenCV 工程链路（编译→运行→显示→保存），从这一行开始，每个 W 都是在往这条链上加新能力。W2 我们加"看穿颜色"的能力，W3 加"找边界"的能力，W4 加"识别形状"的能力，W5 加"读文字"的能力，W6 把所有能力串成一个完整的检测流水线。

> 🎯 **一句话总结 W1**：我们搭好了一个能"看图、画图、存图"的 OpenCV 工程，未来 5 周都在这个工程基础上叠加视觉能力。