# W6 学习笔记：PCB 缺陷检测 — 术语速查卡

> 📌 **W6 出现的核心术语**,每个讲"是什么 / API / 类比 / 用途"。

---

## 1️⃣ PCB 缺陷检测

**是什么**：用机器视觉自动判断 PCB 上有没有装错、漏装、焊反、短路等问题。

**类比**：生产线末端的"电子 QC 工人",用相机代替人眼。

**典型流程**：
1. 拍照（工业相机 / 扫描仪）
2. 定位（找 PCB 边缘、对齐基准）
3. 检测（识别元件 + 数焊点 + 找走线）
4. 判定（与 BOM/Golden Sample 对比）
5. 报告（CSV/JSON/PDF）

---

## 2️⃣ Golden Sample / Baseline

**是什么**：一块**已知合格的 PCB**,作为比对基准。

**用途**：把所有待检板和它做 `cv::absdiff`,差异 > 阈值 → 缺陷。

**工程做法**：合成 baseline 图（用已知尺寸/走线/元件）→ 后续缺陷图都基于它的变体。

---

## 3️⃣ BOM（Bill of Materials）

**是什么**：物料清单,写明每个位置该装什么元件。

```
位置    类型    标称值
R1      R       100Ω
C1      C       10µF
U1      IC      STM32
```

**用途**：检测时把识别结果和 BOM 比对 → 不一致 = 错件 / 缺件。

---

## 4️⃣ HSV 金色 mask（复用 W2）

**是什么**：把 PCB 图转 HSV,提取金色像素 = 走线/焊盘/元件金属面。

```cpp
cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
cv::inRange(hsv, Scalar(10, 80, 80), Scalar(40, 255, 255), goldMask);
```

**为什么用 HSV**：金色在不同光照下 RGB 差异大,但 HSV 的 H 通道稳定。

---

## 5️⃣ 形态学清理（复用 W3）

| 操作 | 公式 | 用途 |
|---|---|---|
| 开运算 OPEN | erode→dilate | 去小噪点 |
| 闭运算 CLOSE | dilate→erode | **补断线（短路判定前必做）** |
| 顶帽 TOPHAT | src - OPEN | 提小目标 |

**短路判定前的关键步骤**：CLOSE 把相邻走线粘起来,再 connectedComponents 数连通域。

---

## 6️⃣ connectedComponentsWithStats — 短路判定核心 ⭐⭐⭐

**是什么**：在二值图里找所有连通域,给每个域一个 ID + 统计信息。

```cpp
cv::Mat labels, stats, centroids;
int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids);
// stats 每行: [x, y, width, height, area]
```

**短路判定逻辑**：
1. 走线 mask 做 CLOSE
2. connectedComponentsWithStats
3. 遍历每个连通域：
   - 若面积 > 阈值（如 >5000 px）
   - 且 bbox 宽度跨越两条"应绝缘"的走线 → 短路

**电气类比**：像电路板"连通性测试",看哪些不该连的点意外连上了。

---

## 7️⃣ matchTemplate + minMaxLoc（复用 W4a）

**是什么**：在 PCB 上滑动小图(元件模板),找最相似位置。

**PCB 用途**：固定视角识别元件（电阻/电容/IC）。

```cpp
cv::matchTemplate(roi, tpl, result, cv::TM_CCOEFF_NORMED);
cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);
if (maxVal > 0.9) → 匹配成功
```

---

## 8️⃣ ORB + findHomography（复用 W4b）

**是什么**：特征点匹配 + 单应性矩阵,能给出**旋转角度**。

**PCB 用途**：
- 元件倾斜时仍能识别
- **算极性反**：算出的旋转角 vs 基准角差 > 90° → 极性反

```cpp
cv::Mat H = cv::findHomography(srcPts, dstPts, cv::RANSAC);
double angle = atan2(H.at<double>(1, 0), H.at<double>(0, 0)) * 180 / CV_PI;
```

---

## 9️⃣ 缺陷类型 — 7 类速查

| 缺陷 | 判定依据 | 主要 API |
|---|---|---|
| 缺件 missing | ROI 内 contour == 0 | findContours |
| 错件 wrong | matchTemplate label 不在 BOM | matchTemplate |
| 极性反 polarity | ORB 旋转角差 > 90° | findHomography |
| 焊桥 bridge | 相邻焊盘 bbox 距离 < 阈值 | boundingRect |
| 焊少 insufficient | 焊点 contour 面积 < 下限 | contourArea |
| 偏移 shifted | 元件中心 vs 焊盘中心偏差 > 阈值 | moments + boundingRect |
| 短路 short | 走线连通域跨过绝缘间隙 | connectedComponentsWithStats |

---

## 🔟 CSV 报告格式

```csv
board_file,component,kind,defect,description,silk,qr,result
images/defect_bridge.png,R1,R,bridge,solder bridge between pads,R1,PCB-2026-001,FAIL
images/defect_short.png,-,-,short,trace short at y=480,short detected,PCB-2026-001,FAIL
images/baseline.png,R1,R,ok,-,R1,PCB-2026-001,PASS
```

**字段含义**：
- `component` = `R1` / `C2` / `U1` / `-`（全局缺陷如短路用 `-`）
- `defect` = 7 类之一 + `ok`
- `result` = `PASS` / `FAIL`

---

## 📚 一句话总结 W6

**把 W2–W5 串成管线,识别元件(W4)+ 数焊点(W3)+ 判短路(连通域)+ 读串号(W5)+ 写 CSV。**

---

## 🎯 PCB 检测完整管线（W2–W6 全链路）

```
原始 PCB 图 (BGR)
   ↓ cv::cvtColor → HSV                          [W2]
   ↓ cv::inRange(金色) → goldMask                [W2]
   ↓ morphologyEx OPEN/CLOSE                     [W3]
   ↓ findContours + connectedComponents           [W3/W6]
   ↓
   ↓ ┌─── W6b: matchTemplate / ORB              [W4]
   ↓ │      → 元件清单 (label, kind, roi, angle)
   ↓ │
   ↓ └─── W6c: 7 类缺陷判定
   ↓        ├ missing / wrong / polarity
   ↓        ├ bridge / insufficient / shifted
   ↓        └ short (连通域分析)
   ↓
   ↓ ┌─── W6d: QRCodeDetector                    [W5]
   ↓ │      → 序列号 "PCB-2026-001"
   ↓ │
   ↓ └─── W6d: Tesseract                         [W5]
   ↓        → 丝印文字 "R1" / "C2" / "U1"
   ↓
   ↓ W6e: 写 CSV 报告
   ↓
images/report.csv
```

---

## 🎓 自我检测

1. **短路判定用哪个 API？**
   → `cv::connectedComponentsWithStats`（数连通域 + 算面积/位置）

2. **缺件 vs 错件怎么区分？**
   → 缺件 = ROI 内 contour == 0；错件 = contour 存在但 template 匹配 label 与 BOM 不一致

3. **极性反判定用哪个？**
   → ORB + findHomography 算旋转角，与基准差 > 90° = 反

4. **CSV 里 `component=-, defect=short` 是什么意思？**
   → 短路是**全局型**缺陷,不属于某个元件,所以 component 留空

5. **W6 引入新依赖了吗？**
   → 没有。复用 W2–W5 的所有 API,只多写一个 Python 合成器
