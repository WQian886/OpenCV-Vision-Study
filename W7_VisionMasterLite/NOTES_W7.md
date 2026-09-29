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

**是什么**:**多个算子的有向无环组合**,前一节点的输出作为后一节点的输入,类似 PLC 的梯形图。

**对标 VM**:VM 的"流程图"(`.sol` 文件),Mini VM 用画布上的 `OperatorNode` + 连线。

**Qt 实现**:每个节点是 `QGraphicsItem`(QGraphicsObject 子类,靠 `QGraphicsScene::changed` 信号自动重画),节点间用三次贝塞尔曲线连。

> ⚠️ 早期版本这里写的是"OperatorNode **序列**、节点间画 `QLineF`" ——
> 那时候流水线是硬编码的,画布只是装饰。现在是真的 **DAG**:
> Run 引擎用 Kahn 算法拓扑排序出执行顺序,顺序由**连线**决定。
> 改了画布就能改执行顺序,这才是"流水线"三个字的意思。

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
- `frame_count` / `frame_index`:帧循环契约,见第 🔟 节

> **`inputs` 和 `params` 的区别**:`params` 是这个算子自己的参数(用户拖滑块设的),
> `inputs` 是上游传下来的。所以算子体里**只能**读 `inputs["count"]` 这类
> 上游字段 —— 一旦改成从 `params` 读,连错线时就拿不到值,
> 而且不报错(见第 9️⃣ 节那个"静默判 FAIL"的例子)。

---

## 7️⃣ 对标 VM 算子清单(Mini VM 12 个 vs VM 1000+ 个)

| Mini VM 算子 | VM 对标 | OpenCV 实现 | 吃 → 吐 | 复杂度 |
|---|---|---|---|---|
| image_source_dir | 文件夹图像 | `imread` | — → 图像 | ⭐⭐ |
| image_source_video | 视频图像 | `VideoCapture` | — → 图像 | ⭐⭐ |
| image_source_camera | 相机采集 | MVS SDK | — → 图像 | ⭐⭐⭐ |
| color_segment | 颜色分割 | `cv::inRange` | 图像 → 图像 | ⭐ |
| blob_count | Blob 分析 | `cv::connectedComponentsWithStats` | 图像 → 数据 | ⭐ |
| count_check | 数量判定 | 纯逻辑 | 数据 → 文本 | ⭐ |
| template_match | 模板匹配 | `cv::matchTemplate + minMaxLoc` | 图像 → 数据 | ⭐⭐ |
| qr_decode | 二维码识别 | `cv::QRCodeDetector` | 图像 → 文本 | ⭐⭐ |
| line_fit | 直线测量 | 纯几何(不用 fitLine) | 图像 → 数据 | ⭐ |
| pcb_inspect | — | `findContours` / `moments` | 图像 → 数据 | ⭐⭐⭐ |
| pcb_defect_judge | — | `inRange` / `morphologyEx` | 数据 → 文本 | ⭐⭐⭐ |
| dl_infer | — | ONNX Runtime | 图像 → 数据 | ⭐⭐⭐ |

还能加:`circle_find` (HoughCircles) / `image_diff` (absdiff) / `edge_find` (Canny)。

> 三个图像源都是 `isSource()` = true:它们没有输入口。
> 注意 `dl_postprocess` 不在这个表里 —— 它不是画布上的节点,
> 而是 `dl_infer` 内部用的前后处理模块(见 README 的"DL 算子的分层")。

---

## 8️⃣ 海康 VM 的 4 大模块(对标学习版)

| VM 模块 | Mini VM 算子(已有) | 可补 |
|---|---|---|
| **定位 Positioning** | template_match / pcb_inspect | circle_find / line_find / corner_find / shape_match |
| **测量 Measurement** | line_fit | caliper / distance / angle / area / circle_fit |
| **识别 Identification** | qr_decode / dl_infer | barcode / ocr / color_classify |
| **检测 Detection** | color_segment / blob_count / count_check / pcb_defect_judge | image_diff / edge_defect / texture_defect |

> 采集(acquisition)不算 VM 的四大模块,但 VM 确实有对应的"图像源" ——
> 本项目做了 4 种:本地单图 / 文件夹 / 视频 / 工业相机。

---

## 9️⃣ pin 类型(Pin Type)

**是什么**:每个算子端口声明的数据类型。VM 里连错类型会直接拒绝。

**本项目的三类 + 一个兜底**:

| 类型 | 中文 | 谁在用 |
|---|---|---|
| `Image` | 图像 | 图像源吐 image_path;颜色分割吐 mask |
| `Data` | 数据 | count、坐标得分、元件清单、检测框 |
| `Text` | 文本 | PASS/FAIL、解码内容、缺陷清单 |
| `Any` | 任意 | **默认值 = 不检查** |

**两条设计要点**:

1. **默认给 `Any` 而不是具体类型。**"没写声明"应该意味着"不检查",
   而不是"拒绝"。默认给具体类型的话,每个新算子忘了覆盖那两个虚函数,
   连正常的线都会被弹窗挡住 —— 那比不做检查更烦人。
2. **只拦"用户主动连线",不拦加载和撤销重做。** 加载旧工程、撤销重做走的是
   `addConnection`,属于"恢复既有状态"而不是"在做新决策"。
   以前合法的连线,升级后也得能打开。

**最有价值的一条检查**:「图像源 → 数量判定」。
`count_check` 只读 `inputs["count"]`;接图像源时 count 拿不到 → actual=0
→ **静默判 FAIL**,而日志上看不出任何异常。这类"不报错但结果错"的问题
是流水线工具最该拦的 —— 拦在搭流水线的时候,而不是拦在产线上。

---

## 🔟 帧循环(Frame Loop)

**是什么**:一个图像源产出 N 张图,流水线对每张跑一遍。
文件夹批量和视频源本质是同一件事。

**契约**(源算子和引擎各管一半):

| 谁 | 做什么 |
|---|---|
| 源算子 | 输出 `frame_count`(一共几帧) |
| 引擎 | 每轮设 `inputs["frame_index"] = frame`,源据此返回第几张 |
| 源算子 | 越界时怎么处理(文件夹源:回落到最后一张,不报错) |

**为什么要汇总 PASS/FAIL 帧数**:抽检 30 帧里 1 帧有问题,
末帧正常的话单看末帧就是**漏检**。产线判废看的是整体,不是抽一张。

> 这条契约以前只写在注释里、根本没实现 —— `image_source_dir` 的注释
> 宣称"每张图都让下游算子跑一次",代码里却写死 index 0。
> **注释会骗人,断言不会。**

---

## 1️⃣1️⃣ Command 模式 + 节点 UUID

**是什么**:把"改画布"包装成对象,压进双栈,实现撤销/重做。

**关键设计:按 `QUuid` 定位节点,而不是裸指针。**

因为撤销删除会"重建"节点 —— 重建后是新地址,存旧指针的话,
下一次撤销就去操作已释放的内存了。

**同一条原则的另一个体现**:节点**不能自己 `delete this`**。
`PipelineView::m_nodes` 还持有该指针,留下悬垂指针后
Run 引擎遍历会段错误。正确做法是发信号,由 MainWindow 统一走
`PipelineView::removeNode`(它已经实现了"收集关联线 → 从容器和 scene
移除 → 统一 delete"的顺序)。

**重入防护**:undo/redo 会改节点位置 → 触发 `nodeMoved` 信号 → 可能又生成
新的 Command。所以 undo 期间要打标志(`m_applyingHistory`),让那批事件
不落进命令栈,否则一次 Ctrl+Z 会连带压回一堆 `MoveNodeCmd`。

---

## 1️⃣2️⃣ 优雅降级(Graceful Degradation)

**是什么**:第三方 SDK 装了就用,没装也不该让整个项目编译不过。

**本项目的做法**:CMake 里用 `find_path` / `find_library` **真实探测**,
再把结果通过 `target_compile_definitions` 喂给编译器,
代码里用 `#ifdef HAS_MVS_SDK` / `#ifdef HAS_ONNXRUNTIME` 分支。

> ⚠️ 反面教材:`HAS_MVS_SDK` 一度从来没人定义过 ——
> 也就是说**哪怕把 MVS SDK 装好、路径全配对,`image_source_camera`
> 也永远是 no-op。**"优雅降级"写成把宏写死成"关",
> 等于"永远不装也能编",于是永远没人发现它没生效。

---

## 1️⃣3️⃣ 特征测试(Characterization Test)

**是什么**:给"看起来像 bug、其实是既有行为"的现象钉一条断言,
防止它无声漂移。

**本项目的例子**:`short.png` 会同时报出 `short` 和 `wrong` 两种缺陷。
这**不是**搬算子时搬出来的 bug,W6 当时就是这样。原因在样本数据:

- 生成脚本里 `GOLD = (30,180,220)` BGR —— 也就是**青色**(直觉上以为是金色)
- `judgeWrong` 的 BGR 范围 `(150,150,0)-(255,255,80)`,青色也满足
- `defect_short` 把短路桥画在 x 418..664,而 R3 的 ROI 是 x 640..760

钉住它的意义:哪天有人"顺手修好"了这条,测试会告诉他
"你改的是样本数据的行为,不是 bug"。换真实产线数据时要重新标定。

---

## 1️⃣4️⃣ W7 最终版做到了什么

> 下面这些当初都写着"MVP 暂未实现" / "下一轮再做",现在都做完了。

- 12 个算子(从 5 个)
- 可视化拖拽建节点 / 连线 / 删除(不是硬编码流水线)
- JSON 持久化 `.sol.json`(节点 + 连线 + 参数 + UUID + 图像路径)
- 参数面板按算子动态生成
- 撤销 / 重做(Command 模式 + 双栈 + 全操作覆盖)
- **pin 类型检查**
- **每步结果查看**(节点状态点 + 右键回看任意一步)
- **帧循环 + 批量汇总**
- **最近工程列表**
- 6 个无头回归测试 / 216 条断言
- **测量类算子** `line_fit`(长度 + 角度),四大模块集齐

**还缺的**:真正的卡尺(亚像素/成对测量)、子图复用、多前驱合并策略、
Linux/macOS 移植。

---

## 1️⃣5️⃣ 角度约定:有向 vs 无向

**是什么**:量一条线时,角度报 0~180 还是 0~360?

**`line_fit` 选的是 0~180(无向直线)。** 因为 (10,0)→(0,0) 和
(0,0)→(10,0) 是**同一条线**,报有向角会让"量一条边"出现两个答案。
对标 VM 的 line fit 就是无向的。

**归一化只要两步**(`atan2` 返回 `(-180, 180]`):

```cpp
if (angle < 0.0)    angle += 180.0;   // (-180,0) → (0,180)
if (angle >= 180.0) angle -= 180.0;   // 180 和 0 是同一条线
```

> 为什么值得单独讲:公式本身简单到不可能错,**会错的是约定**。
> 半年后有人看到 `atan2` 返回 180° 觉得"这明显该改成 180 而不是 0",
> 就"顺手优化"掉了 —— 而 `test_line_fit.cpp` 用例 2 会先红,
> 告诉他这是有意为之。这类测试叫**特征测试**。

**顺带一条工程约定**:两点重合时给 `warning` 而**不是** `error`。
用户在参数面板上还没把两个点拉开,是常态不是异常,
流水线该照常跑完 —— 报 error 会让画布上的节点亮橙点,
看起来像流水线坏了。

---

## 📚 一句话总结 W7

**Mini VisionMaster = 算子基类 + QGraphicsView 流程图 + QDockWidget 工作区,
用约 6700 行 Qt6/OpenCV 代码复刻海康 VM 的核心思路。**

---

## 🎓 自我检测

1. **Mini VM 的 3 层架构是什么?**
   → 算子层(OperatorBase 子类) + 流水线层(QGraphicsScene 节点连线) + GUI 层(QMainWindow + QDockWidget)

2. **QDockWidget 有什么用?**
   → 让 4 个区域(算子库/画布/参数/结果)可拖拽停靠,跟 VM 一样自定义工作区

3. **QGraphicsView 3 层是什么?**
   → Scene(数据) / View(渲染) / Item(图元)

4. **OperatorBase 的统一接口是什么?**
   → name() / run(params, inputs) / createParamWidget() / inputPinType() / outputPinType() / isSource()

5. **为什么 Command 要按 UUID 定位而不是裸指针?**
   → 撤销删除会重建节点(新地址),存旧指针下次撤销就操作已释放内存。
     同理节点不能自己 `delete this` —— 容器的 `m_nodes` 还持有它

6. **pin 类型默认给 `Any` 有什么好处?**
   → "没写声明"= 不检查。新算子忘写也能用,只是少一层保护。
     默认给具体类型的话,忘写就会被莫名拦住,比不检查更烦人

7. **「图像源 → 数量判定」为什么值得在连线时拦?**
   → count_check 只读 `inputs["count"]`,接图像源时 count 拿不到 →
     actual=0 → **静默判 FAIL**,日志上看不出任何异常

8. **优雅降级为什么必须真探测?**
   → 宏写死成"关"等于"永远不装也能编",于是永远没人发现它没生效
     (本项目的 `HAS_MVS_SDK` 就这么坑过一回)

9. **Mini VM 跟 VM 的差距还在哪?**
   → 算子数量(12 vs 1000+)、真正的卡尺(亚像素/成对测量)、
     子图复用、多分支合并策略、性能优化、Linux/macOS 移植

10. **加一个新算子要走哪几步?**
    → ① 写 .h/.cpp(实现 6 个纯虚函数)② `operator_factory.cpp` 加一行
      ③ `mainwindow.cpp` 的 `m_operatorInfo.append` ④ `CMakeLists.txt`
      ⑤ 写测试。**②③④ 配错不报错**,只会"算子没出现在左侧列表里"

11. **为什么 `line_fit` 的测试 24 条里大半不是测算术?**
    → 公式就两行,不会错。会错的是约定:角度 0~180 还是 0~360、
      两点重合报不报 error、没给图报不报 error。
      这些都不崩,只是结果悄悄变了,而测量直接决定判废
