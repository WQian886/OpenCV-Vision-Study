# W5: OCR + 二维码识别

## 🎯 学习目标
1. 掌握 `cv::QRCodeDetector` 检测 + 解码 QR 码
2. 理解 QR 码的 3 个定位标记 + 4 个角点
3. 学会把 OpenCV 预处理（灰度化 + 自适应阈值）输出给 Tesseract
4. 掌握在 Windows 上用 `CreateProcessW` 调用外部命令行程序
5. **实战：读 PCB 上的元件丝印 + 序列号 QR**

## 📁 工程结构
```
W5_OCR_QRCode/
├── CMakeLists.txt           # Release-only / OpenCV / utf-8
├── main.cpp                 # qr / ocr 两个子模式
├── gen_qr.py                # 生成 QR 测试图
├── gen_ocr_test.py          # 生成 OCR 测试图（带丝印文字）
├── images/
│   ├── qrcode_test.png      ← QR 测试图
│   ├── ocr_test.png         ← OCR 测试图
│   ├── w5_qr_result.png     ← QR 识别结果
│   ├── w5_ocr_result.png    ← OCR 标注结果
│   └── ocr_preprocessed.png ← 传给 Tesseract 的二值图
└── README.md                # 本文件
```

## 🛠️ 构建（VS 方式，同 W4）
1. VS 2022/2026 → **打开本地文件夹** → 选 `E:\qianwie\00study\OpenCV\W5_OCR_QRCode`
2. CMake 自动配置 → **配置切到 x64-Release**（必须 Release！Debug 会触发 OpenCV ABI 不匹配）
3. F7 编译 → Ctrl+F5 运行（不要 F5）

> 💡 Tesseract 路径在 `main.cpp` 里硬编码 `D:\Program Files\Tesseract-OCR\tesseract.exe`，改机器请同步改这个常量。

## 🛠️ 命令行构建（备选）
```powershell
cd E:\qianwie\00study\OpenCV\W5_OCR_QRCode
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\w5_ocr_qrcode.exe qr    # 跑 QR 识别
build\w5_ocr_qrcode.exe ocr   # 跑 OCR（默认也是 ocr）
```

## 📚 知识点笔记

### cv::QRCodeDetector（核心模块自带，不依赖 contrib）

```cpp
cv::QRCodeDetector detector;
std::vector<cv::Point2f> points;
std::string decoded = detector.detectAndDecode(img, points);
// decoded = 解码内容（空字符串 = 没找到）
// points  = 4 个角点（左上 → 右上 → 右下 → 左下）
```

**支持的格式**：QR Code、Aztec、CODABAR 等（不只 QR）。

### 4 个角点的用途

| 用途 | 代码 |
|---|---|
| 画四边形 | `cv::line(marked, points[i], points[(i+1)%4], ...)` |
| 算中心 | `((points[0] + points[2]) / 2)` |
| 算 ROI | `cv::Rect(topLeft.x, topLeft.y, points[2].x-topLeft.x, points[2].y-topLeft.y)` |

### 检测 vs 解码

| | detect | decode |
|---|---|---|
| 做什么 | 找 QR 在图里的位置 | 把 QR 内容读出来 |
| API | `detector.detect(img, points)` | `detector.decode(img, points)` |
| 返回 | bool + 4 个角点 | string（解码内容）|

**一行搞定**：`detectAndDecode` = 检测 + 解码（最常用）。

### QRCodeDetector vs wechat_qrcode

| | QRCodeDetector（核心） | wechat_qrcode::WeChatQRCode（contrib） |
|---|---|---|
| 抗模糊 | 一般 | ⭐⭐⭐⭐ 强 |
| 多码识别 | ❌ 单码 | ✅ 多个同时 |
| 距离容忍 | 一般 | ⭐⭐⭐ 大小都能识别 |
| 安装 | OpenCV 自带 | 需 contrib |

**本工程用核心版**——PCB 序列号识别够用。

### OpenCV 预处理 → Tesseract

Tesseract 直接吃 RGB 不理想，先做二值化：

```cpp
cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
cv::adaptiveThreshold(gray, thresh, 255,
                      cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                      cv::THRESH_BINARY, 31, 10);
```

**为什么用自适应阈值**：PCB 上的丝印经常有局部光照不均，全局阈值会丢字。

### CreateProcessW 调用 Tesseract

Windows 上调用外部 exe 遇到路径含空格要用 `CreateProcessW`：

```cpp
STARTUPINFOW si = {};
si.cb = sizeof(si);
PROCESS_INFORMATION pi = {};
std::wstring cmdLine = L"\"" + tesseract + L"\" " + args;
// CreateProcessW 要求命令行可写,必须拷贝到 vector<wchar_t>
std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
cmdBuf.push_back(L'\0');
CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
WaitForSingleObject(pi.hProcess, INFINITE);
```

## ⚠️ 常见踩坑

| 现象 | 原因 | 修复 |
|---|---|---|
| `Debug` 编译失败 | OpenCV ABI 不匹配 | 切到 x64-Release |
| QR 解码为空 | 图太模糊 / QR 被遮挡 | 先 `GaussianBlur` 预处理，或换大尺寸 QR |
| `CreateProcessW` 失败 (错误码 2) | Tesseract 路径不存在 | 改 `main.cpp` 里的硬编码路径 |
| OCR 输出乱码 | 控制台不是 UTF-8 | 头里加 `SetConsoleOutputCP(CP_UTF8)` + CMake 加 `/utf-8` |
| OCR 识别率低 | 字体 / 光照不均 | 加大二值化核或换 `--psm` 模式 |

## ✏️ 课后练习

### 基础（W5a QR）
1. **画 4 个角点连线**：把 QR 的 4 个角点用红线连起来
2. **写文字**：在 QR 旁边用 `putText` 写"序列号：XXX"
3. **多 QR 检测**：把 `detectAndDecode` 改成循环，扫一张图里多个 QR
4. **错误处理**：故意把 QR 图删了，确认程序给出友好提示
5. **生成真实串号**：用 `gen_qr.py` 生成 `PCB-2026-001` 这种格式

### 进阶（W5b OCR）
6. **PSM 模式切换**：把 `--psm 6` 改成 `--psm 7`（单行）看输出变化
7. **多语言**：Tesseract 装了中文包就加 `-l eng+chi_sim` 试试
8. **白名单**：加 `-c tessedit_char_whitelist=0123456789RCOK-` 限制识别字符
9. **置信度**：用 Tesseract 的 `TSV` 输出模式拿到每个字符的置信度
10. **实战 OCR PCB**：找一张真实 PCB 照片，跑 OCR 读元件丝印

## 🎯 PCB 检测完整管线（W2 + W3 + W4 + W5）

```
原始 PCB 图
   ↓ bilateralFilter (W2)               ← 去噪保边
   ↓ cvtColor → HSV (W2)                ← 色彩空间
   ↓ inRange(金色HSV) → mask (W2)       ← 颜色提取
   ↓ morphologyEx OPEN (W3)             ← 去小噪点
   ↓ findContours (W3)                  ← 找焊点位置
   ↓ contourArea 过滤 (W3)              ← 区分焊点 vs 噪点
   ↓ boundingRect (W3)                  ← 给出每个元件 ROI
   ↓
   ↓ ┌─── W4a: matchTemplate(roi, template)
   ↓ │      → 识别"这是电阻还是电容"
   ↓ │
   ↓ └─── W4b: ORB + BFMatcher + findHomography
   ↓        → 抗旋转/缩放的元件识别
   ↓
   ↓ ┌─── W5a: detectAndDecode → 读 PCB 序列号 QR
   ↓ │
   ↓ └─── W5b: Tesseract → 读元件丝印文字
   ↓
"PCB-2026-001, 5 个电阻、2 个电容、1 个 IC, 序列号/丝印已记录"
```

## 🎓 下一节预告

W6: **PCB 缺陷检测综合项目**
- 把 W2–W5 串成完整管线
- 7 类缺陷判定：缺件/错件/极性反/焊桥/焊少/偏移/**短路**
- 输出 CSV 报告（per-board-per-component）
- Python 合成 baseline + 7 张缺陷变体
