# W6: PCB 缺陷检测综合项目

## 🎯 学习目标
1. 把 W2（HSV 颜色提取）+ W3（边缘/轮廓）+ W4（模板/特征）+ W5（OCR/QR）**串成完整管线**
2. 实现 7 类 PCB 缺陷判定：**缺件 / 错件 / 极性反 / 焊桥 / 焊少 / 偏移 / 短路**
3. 用 Python 合成 baseline + 7 张缺陷变体,作为标准测试集
4. 输出 **CSV 报告**（per-board-per-component）
5. **实战：跑一张图 → 得到一份带缺陷清单的 CSV**

## 📁 工程结构
```
W6_PCB_DefectProject/
├── CMakeLists.txt              # Release-only / OpenCV / utf-8
├── main.cpp                    # 4 个子模式入口 + W6b~W6e 函数骨架
├── gen_pcb_dataset.py          # 用 Pillow 合成 baseline + 7 张缺陷图
├── README.md                   # 本文件
├── NOTES_W6.md                 # 术语速查卡
├── images/                     # 测试图 + 输出
│   ├── baseline.png            ← 标准板
│   ├── defect_missing.png
│   ├── defect_wrong.png
│   ├── defect_polarity.png
│   ├── defect_bridge.png
│   ├── defect_insufficient.png
│   ├── defect_shifted.png
│   ├── defect_short.png
│   ├── manifest.csv            ← 测试图元数据
│   └── w6_inspect_marked.png   ← inspect 输出
├── templates/                  # 元件模板（给 W4 匹配用,待补）
└── report/                     # CSV 报告输出
```

## 🛠️ 构建（VS 方式，同 W5）
1. VS 2022/2026 → **打开本地文件夹** → `E:\qianwie\00study\OpenCV\W6_PCB_DefectProject`
2. CMake 自动配置 → **配置切到 x64-Release**（必须 Release！Debug 会触发 OpenCV ABI 不匹配）
3. F7 编译 → Ctrl+F5 运行

## 🛠️ 命令行构建（备选）
```powershell
cd E:\qianwie\00study\OpenCV\W6_PCB_DefectProject
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 1. 生成测试数据（首次必跑）
python gen_pcb_dataset.py

# 2. 默认模式 = baseline 全管线
build\w6_pcb_defect.exe

# 3. inspect 单图
build\w6_pcb_defect.exe inspect images/baseline.png

# 4. 批量报告
build\w6_pcb_defect.exe report images

# 5. 重新生成数据
build\w6_pcb_defect.exe gen
```

## 📚 知识点（贯穿前 5 周）

### PCB 缺陷判定管线
```
images/baseline.png (BGR)
   ↓ cv::cvtColor → HSV (W2)
   ↓ cv::inRange(金色) → goldMask (W2)
   ↓ morphologyEx OPEN (W3)
   ↓ findContours (W3)
   ↓ ┌─── W6b: matchTemplate/ORB → 识别元件
   ↓ │       ├ 找到 → 标 green
   ↓ │       └ 没找到 → 候选 missing
   ↓ └─── W6c: 缺陷判定
   ↓        ├ 缺件:ROI 内 contour 数 == 0
   ↓        ├ 错件:label 不在 BOM
   ↓        ├ 极性反:旋转角差 > 阈值
   ↓        ├ 焊桥:相邻焊盘 bbox 距离 < 阈值
   ↓        ├ 焊少:焊点面积 < 阈值下限
   ↓        ├ 偏移:元件中心 vs 焊盘中心偏差 > 阈值
   ↓        └ 短路:连通域覆盖多条走线 (W3 connectedComponents)
   ↓ ┌─── W6d: OCR 丝印 + QR 串号 (W5)
   ↓ └─── W6e: 写 CSV 报告
```

### 7 类缺陷速查

| 缺陷 | 英文 | 判定依据 | 难度 |
|---|---|---|---|
| 缺件 | missing | 元件 ROI 内 contour 数 == 0 | ⭐ |
| 错件 | wrong | 模板匹配 label 与 BOM 不符 | ⭐⭐ |
| 极性反 | polarity | ORB 给出旋转角 vs 基准差 > 90° | ⭐⭐ |
| 焊桥 | bridge | 相邻焊盘 bbox 距离 < 阈值 | ⭐⭐ |
| 焊少 | insufficient | 焊点 contour 面积 < 阈值下限 | ⭐ |
| 偏移 | shifted | 元件中心 vs 焊盘中心偏差 > 阈值 | ⭐ |
| **短路** | **short** | **走线连通域跨过绝缘间隙** | ⭐⭐⭐ |

### 关键 API（不引入新依赖）

| API | 复用 | 用途 |
|---|---|---|
| `cv::inRange` | W2 | 提取金色 mask |
| `cv::findContours` | W3 | 数焊点 / 元件轮廓 |
| `cv::connectedComponentsWithStats` | W3 | **短路判定** |
| `cv::matchTemplate` | W4 | 元件识别（固定视角）|
| `cv::ORB + findHomography` | W4 | 抗旋转识别 + 测角度 |
| `cv::QRCodeDetector` | W5 | 读序列号 |
| `CreateProcessW + tesseract` | W5 | 读丝印文字 |

## ⚠️ 常见踩坑

| 现象 | 原因 | 修复 |
|---|---|---|
| `Debug` 编译失败 | OpenCV ABI 不匹配 | 切到 x64-Release |
| `imread failed: images/baseline.png` | 没跑 `gen_pcb_dataset.py` | 先 `python gen_pcb_dataset.py` |
| 短路漏检 | 走线不连续,连通域被切断 | 先 `morphologyEx CLOSE` 把断线粘起来 |
| 焊桥误检 | 阈值太严 | 放宽焊盘最小间距阈值 |
| OCR 输出乱码 | 控制台非 UTF-8 | CMake 加 `/utf-8` + `SetConsoleOutputCP` |

## ✏️ 课后练习

### 基础（W6a 数据 + 脚手架）
1. **改 PCB 配色**：把 `GREEN_PCB` 改成蓝色（#1a3d5d）看 inspect 输出
2. **加更多元件**：在 SITES 里加第 7 个元件,看 baseline 是否仍能跑通
3. **新缺陷类型**：加 "元件倾斜 45°" 作为第 8 类缺陷

### 进阶（W6b–W6e）
4. **填 W6b**：用 `matchTemplate` 识别 6 个元件,画绿框
5. **填 W6c.短路**：`connectedComponentsWithStats` 找出大于阈值的连通域
6. **填 W6c.焊桥**：遍历 Component,算相邻 ROI 中心距
7. **填 W6d.OCR**：把 W5 的 CreateProcessW 拷过来,读元件丝印
8. **填 W6e**：跑 `report` 模式,产出包含全部 8 张图结果的 report.csv

### 实战
9. **替换实拍图**：用手机拍一块 PCB,放到 images/ 下,跑 inspect
10. **加新缺陷**：自己造一张"元件错位 + 焊桥"的复合缺陷图,看检测结果

## 🎓 下一节预告

W7+（如果你想做）：
- 多个 PCB 同时检测（批处理）
- 与 PLC / MES 系统对接（写 XML/JSON 上传）
- 深度学习方案（YOLO 检测元件）
- 3D 检测（结构光 + 点云）
