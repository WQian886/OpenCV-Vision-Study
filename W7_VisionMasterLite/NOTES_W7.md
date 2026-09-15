# W7 学习笔记:Mini VisionMaster — 术语速查卡

> 📌 **W7 出现的核心概念**,每个讲"是什么 / 对标 VM / Qt 实现 / 用途"。

---

## 1️⃣ Mini VisionMaster / Mini VM

**是什么**:仿照海康 VisionMaster 设计的**学习版视觉算法平台**。

**对标 VM**:VM 是商业版,Mini VM 是开源学习版。

**Qt 实现**:用 `QMainWindow` + `QDockWidget` + `QGraphicsView` 三件套搭出 VM 的"工作区"。

**用途**:理解工业视觉平台的设计思路,为后续接触 HALCON / VM / Cognex 打基础。

---

## 2️⃣ 算子(Operator)

**是什么**:平台上可调用的**最小算法单元**。在 VM 里叫"模块/算法",在 Mini VM 里是继承 `OperatorBase` 的 C++ 类。

**对标 VM**:VM 的 1000+ 自研算子(定位 / 测量 / 识别 / 检测)。

**Qt 实现**:
```cpp
class OperatorBase {
    virtual QString name() = 0;
    virtual QJsonObject run(params, inputs) = 0;
};
```

**用途**:把零散的 OpenCV 调用封装成可复用的"积木块"。

---

## 3️⃣ 流水线(Pipeline)

**是什么**:**多个算子的有序组合**,前一节点的输出作为后一节点的输入,类似 PLC 的梯形图。

**对标 VM**:VM 的"流程图"(`.sol` 文件),Mini VM 用画布上的 `OperatorNode` 序列。

**Qt 实现**:每个节点是 `QGraphicsItem`,节点间画 `QLineF`。

**类比**:PLC 的梯形图 / 数据流图(DFD) / 函数调用图。

---

## 4️⃣ QGraphicsView / QGraphicsScene / QGraphicsItem

**是什么**:Qt6 的**2D 图形视图框架**,分 3 层:

| 组件 | 角色 |
|---|---|
| `QGraphicsScene` | 数据层,容纳所有图元 |
| `QGraphicsView`   | 视图层,负责渲染 + 滚动 + 缩放 |
| `QGraphicsItem`   | 图元层,具体一个节点/连线/矩形 |

**对标 VM**:VM 的流程图编辑面板。

**用途**:画流程图、画电子地图、画电路图…… 凡是"节点 + 连线"的图都能用。

---

## 5️⃣ QDockWidget

**是什么**:可**停靠 + 浮动 + 隐藏**的面板,是 Qt 里做"工作区自定义"的标准控件。

**对标 VM**:VM 的 4 个区域(算子库 / 画布 / 参数 / 结果)都可以拖拽。

**API**:
```cpp
QDockWidget* dock = new QDockWidget("算子库", this);
dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
addDockWidget(Qt::LeftDockWidgetArea, dock);
```

---

## 6️⃣ QJsonObject / QJsonArray

**是什么**:Qt 提供的 **JSON 数据结构**,用于算子间的**数据传递**。

**对标 VM**:VM 内部用二进制格式传递数据;Mini VM 用 JSON 简化(便于调试 + 序列化)。

**算子接口**:
```cpp
QJsonObject run(params, inputs);   // 输出也是 JSON
```

**典型数据**:
- `image_path`:图像路径(字符串)
- `mask_path`:二值图路径
- `count`:整数
- `result`:"PASS"/"FAIL"

---

## 7️⃣ 对标 VM 算子清单(Mini VM 5 个 vs VM 1000+ 个)

| Mini VM 算子 | VM 对标 | OpenCV 实现 | 复杂度 |
|---|---|---|---|
| color_segment | 颜色分割 | `cv::inRange` | ⭐ |
| blob_count | Blob 分析 | `cv::connectedComponentsWithStats` | ⭐ |
| count_check | 数量判定 | 纯逻辑 | ⭐ |
| template_match | 模板匹配 | `cv::matchTemplate + minMaxLoc` | ⭐⭐ |
| qr_decode | 二维码识别 | `cv::QRCodeDetector` | ⭐⭐ |

下一轮可加:`circle_find` (HoughCircles) / `line_fit` (fitLine) / `contour_find` (findContours) / `image_diff` (absdiff) / `edge_find` (Canny)。

---

## 8️⃣ 海康 VM 的 4 大模块(对标学习版)

| VM 模块 | Mini VM 算子(已有) | 可补 |
|---|---|---|
| **定位 Positioning** | template_match | circle_find / line_find / corner_find / shape_match |
| **测量 Measurement** | (暂无) | caliper / distance / angle / area / line_fit / circle_fit |
| **识别 Identification** | qr_decode | barcode / ocr / color_classify |
| **检测 Detection** | color_segment / blob_count / count_check | image_diff / edge_defect / texture_defect |

---

## 9️⃣ MVP 限制(本轮)

- 5 个算子(对比 VM 的 1000+)
- 默认流水线硬编码(对比 VM 的可视化拖拽)
- 无 JSON 持久化(对比 VM 的 `.sol` 项目文件)
- 无参数面板动态切换(对比 VM 的智能表单)
- 无撤销 / 重做(对比 VM 的 Ctrl+Z)

**学习意义**:即便是 MVP,也已经覆盖了 VM 的核心架构(算子 + 流水线 + GUI 三层)。

---

## 📚 一句话总结 W7

**Mini VisionMaster = 算子基类 + QGraphicsView 流程图 + QDockWidget 工作区,用 1000 行 Qt6 代码复刻海康 VM 的核心思路。**

---

## 🎓 自我检测

1. **Mini VM 的 3 层架构是什么?**
   → 算子层(OperatorBase 子类) + 流水线层(QGraphicsScene 节点连线) + GUI 层(QMainWindow + QDockWidget)

2. **QDockWidget 有什么用?**
   → 让 4 个区域(算子库/画布/参数/结果)可拖拽停靠,跟 VM 一样自定义工作区

3. **QGraphicsView 3 层是什么?**
   → Scene(数据) / View(渲染) / Item(图元)

4. **OperatorBase 的统一接口是什么?**
   → name() / run(params, inputs) / createParamWidget()

5. **Mini VM 跟 VM 的差距在哪?**
   → 算子数量(5 vs 1000+)、可视化拖拽、JSON 项目文件、参数面板动态生成、性能优化
