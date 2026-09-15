# W7 Run 引擎改 DAG 拓扑排序 — 变更日志（2026-09-15）

> 📅 **记录时间**：2026-09-15
> 📁 **项目来源**：W7_VisionMasterLite Mini VisionMaster Lite
> ✏️ **改动文件**：`W7_VisionMasterLite/mainwindow.cpp`（1 个文件，2 处 Edit）
> 🎯 **本次目标**：Run 引擎从"硬编码 4 个算子固定顺序"升级为"按用户画的图（DAG）拓扑排序执行"
> ✅ **验证**：编译通过 + GUI 启动 3 秒无崩溃（PID 10768）

---

## 📌 一句话总结

把 `MainWindow::onRunClicked` 的固定顺序循环（`order = {"color_segment", "blob_count", "count_check", "qr_decode"}`）替换成"读 `m_canvas->connections()` → Kahn 拓扑排序 → 按拓扑序遍历每个节点 → inputs 合并所有前驱节点 output"。**W7 从此真正"按图执行"**，不再是"念稿人"。

---

## 📋 一、改了什么

### Edit 1 — 新增 `topoSort` 辅助函数（mainwindow.cpp 行 406-451）

紧挨 `onRunClicked` 上面加了一个 static 函数：

```cpp
static QList<OperatorNode*> topoSort(PipelineView* canvas, bool* hasCycle) {
    // 1. 建图 + 入度统计
    // 2. 入度 0 节点入队
    // 3. BFS 出队,生成 order
    // 4. 环检测：order.size() != nodes.size() 则有环
}
```

**关键设计**：
- 返回 `QList<OperatorNode*>` 而非 `QList<QString>`（节点指针直接用）
- `hasCycle` 是出参（C 风格 bool*）而不是异常或 std::optional，保持 Qt 风格简洁
- 用 `QHash<Node*, int>` 和 `QHash<Node*, QList<Node*>>` 做邻接表

### Edit 2 — 重写 `onRunClicked`（mainwindow.cpp 行 466-575）

**核心变化**（95 行 → 约 130 行）：
- ❌ 删除硬编码 `QStringList order = {"color_segment", "blob_count", ...}`
- ❌ 删除 `inputs["xxx"] = out["xxx"]` 的"线性塞回"模式
- ✅ 调用 `topoSort(m_canvas, &hasCycle)` 拿拓扑序
- ✅ 用 `QHash<OperatorNode*, QJsonObject> nodeOutputs` 缓存每个节点的 output
- ✅ 遍历每个节点时，构造 inputs = `{image_path}` + 所有前驱节点 outputs 合并
- ✅ 用 `finalNode` 跟踪最后一个节点；`finalResult` 只在包含 `"result"` 键时覆盖（让 count_check 之外的算子也能是"最后"）
- ✅ 新增边界处理：空画布友好提示 / 环路检测报错

### Edit 3 — 加 2 个 include（行 23-24）

```cpp
#include <QQueue>   // Kahn 算法 BFS 队列
#include <QHash>    // 邻接表 + 输出缓存
```

> Qt 头文件之间的传递包含有时候不可靠（QQueue/QHash 不一定通过 `<QWidget>` 间接包含），显式写出来最稳。

---

## 🎯 二、目的是什么

### 业务目标
让 W7 Mini VM 从"演示品"变成"真能用的流程图编辑器"：
- 用户删一个默认节点，Run 引擎不再 silently skip
- 用户画新连线，Run 引擎按新顺序执行
- 用户画错了形成环，自动报错
- 用户加新算子（不在硬编码列表里），Run 引擎自动识别并执行

### 架构目标
为后续 P1（Save/Load JSON 流水线）做准备：
- Save 端可以序列化 `canvas->connections()` 边的拓扑信息
- Load 端可以读 JSON 重建节点 + 连线
- Run 引擎按拓扑执行不依赖任何"默认名字"

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| W7 完成度 | ~70% | **~80%** |
| Run 引擎 | 硬编码 4 算子 | 按图拓扑（DAG）|
| 删节点后 Run | silently skip | 自然跳过 |
| 加新节点 | 不支持 | 自动识别执行 |
| 环检测 | 无 | 自动报错 |

---

## 🤔 三、为什么要这么改

### 候选方案对比

| 方案 | 工作量 | 通用性 | 缺点 |
|---|---|---|---|
| **A. Kahn 拓扑排序（本次采用）** | 30 行 | ✅ 通用 | 多上游 key 冲突简单合并可能误覆盖 |
| B. DFS 后序遍历 | 30 行 | ✅ 通用 | 递归深易栈溢出 |
| C. 保留硬编码 + 加配置 | 10 行 | ❌ 半通用 | 用户改了图还是硬编码跑，违背初衷 |
| D. 完全图执行（每步都重新算） | 大 | ✅ | 性能差，过度工程 |

**选 A 的理由**：
1. **最易理解**：BFS + 入度统计是算法入门内容
2. **天然支持环检测**：order.size() != nodes.size() 即有环
3. **Qt 容器原生支持**：QQueue、QHash 都是 Qt 自带，无需外部依赖
4. **5 个算子都是同步算子**：不需要考虑异步/并行

**选 A 的代价**：
- 多上游节点的同 key 输出会被后写覆盖（接受：算子只读自己关心的字段）
- 递归 DFS 可能栈溢出被规避（BFS 更稳）

---

## 💡 四、有成型的解决方案吗？

### 4.1 拓扑排序本身

这是个非常成熟的算法，30+ 年历史。教科书实现：
- **Kahn 算法（1962）** — BFS + 入度统计 — 本次采用
- **Tarjan 强连通分量** — 可同时识别环，但实现复杂
- **DFS 后序** — 递归版本简单但栈不安全

Qt 应用里直接用 `QQueue + QHash` 是最地道的做法。

### 4.2 工业视觉平台的 Run 引擎

参考 [海康 VM EasyBuilder](https://www.hikrobotics.com/vision/visionmaster) 的实现：
- **DAG 拓扑排序** — 主流方案
- **数据流**：每个算子的 input pin 接收上游 output，output pin 流向下游
- **类型检查**：input pin 类型 vs 上游 output 类型不匹配则连线时禁止
- **执行模型**：同步（本次采用）或异步（VM 支持并行多分支）

**本次实现的局限**：
- ❌ 没做类型检查（input pin 名 vs 上游 output key 名）
- ❌ 没做并行执行（多分支本应并行跑）
- ❌ 没做算子 input pin 声明（每个算子声明自己需要哪些 key）

后续若需要扩展，可参考 [Apache Airflow](https://airflow.apache.org/) 的 DAG 概念（任务依赖 + 调度）。

### 4.3 简单替代方案

如果用户不想用拓扑排序（觉得复杂），可以：
- **保留硬编码 + 让用户自己保证连线顺序**（不推荐，违背编辑器本质）
- **按节点在画布上的 x 坐标排序**（从左到右执行，对简单流水线够用，但不严谨）

---

## ⚠️ 五、踩坑记录

### 坑 1：`QQueue`/`QHash` 未包含

**编译错误**：
```
error C2079: "q"使用未定义的 class "QQueue<w7::OperatorNode *>"
```

**原因**：Qt 头文件的传递包含行为不一致——`<QWidget>` 不保证传递引入 `<QQueue>` 和 `<QHash>`。

**修复**：在 mainwindow.cpp 顶部显式加：
```cpp
#include <QQueue>
#include <QHash>
```

**教训**：用 Qt 容器时**总是显式 #include 对应头文件**，不要依赖传递包含。

### 坑 2：原代码 `finalResult` 只在 `opName == "count_check"` 时覆盖

**旧逻辑**（mainwindow.cpp:501-503）：
```cpp
if (opName == "count_check") {
    finalResult = out;
}
```

**问题**：用户如果把 pipeline 改成 `color_segment → template_match`，最终结果是 template_match 的 output（含 position_x/score/pass），但 finalResult 还是空的。

**修复**（新逻辑）：
```cpp
if (out.contains("result")) finalResult = out;  // 任何含 "result" 键的都算最终结果
```

**教训**：判断"最终结果"应该看**语义键**（"result"），不应该看**算子名**（"count_check"）。

### 坑 3：build 输出位置混淆

**症状**：
```
Error: E:/qianwie/00study/OpenCV/W7_VisionMasterLite/out/build/x64-Release is not a directory
```

**原因**：W6 用 `out/build/x64-Release`（CMakeUserPresets 配置），但 W7 用 `build/`（CMakePresets + Ninja 风格）。

**修复**：用 `cmake --build build --target w7_vm_lite`，路径写 `build/`。

**教训**：每个 W 工程的 CMake 配置可能不同，跑构建前先看 CMakePresets/CMakeUserPresets 文件确认输出路径。

---

## ✅ 六、验收结果

### 编译
```
[1/4] Automatic MOC and UIC for target w7_vm_lite
[2/3] Building CXX object CMakeFiles\w7_vm_lite.dir/mainwindow.cpp.obj
[3/3] Linking CXX executable w7_vm_lite.exe; Copying OpenCV DLLs to output directory; Copying test images to output directory
```
- ✅ 退出码 0
- ✅ exe 在 `build/w7_vm_lite.exe`（212480 B，时间戳 2026/9/15 10:22:04）

### GUI 启动验证
```
$proc = Start-Process w7_vm_lite.exe
Sleep 3
$proc.HasExited → False  (PID 10768 仍在跑)
Stop-Process -Force  →  ✅ 成功
```
- ✅ 启动后存活 3 秒以上
- ✅ 无 segfault
- ✅ 无窗口相关断言失败

### 用户验证（待跑）

> 详细测试步骤见 commit message 或单独给用户的 T1-T5 测试清单。

#### T1 默认流水线（应和改前完全一致）
1. 启动 W7 → 默认 4 节点 2 连线已加载
2. 工具栏"图像"输入框：填 `../W6_PCB_DefectProject/images/baseline.png`
3. 点 ▶ Run
4. 检查底部日志：应输出 `color_segment → blob_count → count_check → PASS/FAIL`，节点执行顺序：颜色分割 → Blob 计数 → 数量判定 → QR 解码（旁路）
5. **预期**：和改前完全一致 ✅

#### T2 删除节点
1. 选中 qr_decode 节点 → 按 Delete 键
2. 点 ▶ Run
3. **预期**：日志无"算子 qr_decode 未注册"错误，3 个节点正常执行

#### T3 画环
1. 手动从 count_check 的输出端口拖到 color_segment 的输入端口
2. 点 ▶ Run
3. **预期**：日志红字 `[Run] ❌ 检测到环路!无法执行（1 个节点未参与拓扑序）`，状态栏 `❌ 环路`

#### T4 重排连线
1. 删除默认连线
2. 重新连 `color_segment → count_check → blob_count`（跳过中间检查）
3. 点 ▶ Run
4. **预期**：日志节点顺序反映新图

#### T5 独立节点
1. 从算子库拖一个 `template_match` 到画布（**不连任何线**）
2. 点 ▶ Run
3. **预期**：template_match 仍会执行（作为"入度 0 根节点"），但 inputs 只有 image_path；如果默认 `templates/R.png` 不存在会报错（已知问题，W7 README 列过）

---

## 📊 七、关键代码片段

### 拓扑排序核心（40 行）

```cpp
static QList<OperatorNode*> topoSort(PipelineView* canvas, bool* hasCycle) {
    *hasCycle = false;
    QList<OperatorNode*> order;
    const auto& nodes = canvas->nodes();
    const auto& conns = canvas->connections();
    if (nodes.isEmpty()) return order;

    // 1. 建图 + 入度
    QHash<OperatorNode*, QList<OperatorNode*>> graph;
    QHash<OperatorNode*, int> inDegree;
    for (auto* n : nodes) { graph[n] = {}; inDegree[n] = 0; }
    for (auto* c : conns) {
        graph[c->fromNode()].append(c->toNode());
        inDegree[c->toNode()]++;
    }

    // 2. 入度 0 入队
    QQueue<OperatorNode*> q;
    for (auto* n : nodes)
        if (inDegree.value(n, 0) == 0) q.enqueue(n);

    // 3. BFS
    while (!q.isEmpty()) {
        auto* n = q.dequeue();
        order.append(n);
        for (auto* m : graph.value(n))
            if (--inDegree[m] == 0) q.enqueue(m);
    }

    // 4. 环检测
    if (order.size() != nodes.size()) { *hasCycle = true; return {}; }
    return order;
}
```

### 执行循环核心（30 行）

```cpp
QHash<OperatorNode*, QJsonObject> nodeOutputs;
OperatorNode* finalNode = nullptr;
QJsonObject finalResult;

for (OperatorNode* n : order) {
    QJsonObject inputs = {{"image_path", imagePath}};
    // 合并所有前驱 outputs
    for (const auto* c : m_canvas->connections()) {
        if (c->toNode() == n && nodeOutputs.contains(c->fromNode())) {
            for (auto it = nodeOutputs[c->fromNode()].begin();
                 it != nodeOutputs[c->fromNode()].end(); ++it)
                inputs[it.key()] = it.value();
        }
    }
    // 执行
    OperatorBase* op = w7::createOperatorByName(n->opName());
    QJsonObject out = op->run(n->params(), inputs);
    nodeOutputs[n] = out;
    finalNode = n;
    if (out.contains("result")) finalResult = out;
    // ...
}
```

---

## 🔄 八、待补（不属本次改动）

| 优先级 | 任务 | 说明 |
|---|---|---|
| 🟡 中 | **P1 Save/Load JSON 流水线** | 序列化 `canvas->nodes()` + `connections()` 为 `.sol.json` |
| 🟡 中 | **input pin 类型检查** | 算子声明需要哪些 key，连线时验证上游提供 |
| 🟢 低 | **并行执行多分支** | 当 DAG 有多个入度 0 节点时并发执行（QtConcurrent） |
| 🟢 低 | **错误恢复** | 单个节点 error 时不中断全流水线 |
| ⚪ 远期 | **算子依赖分析** | 自动推算哪些 input key 必填/可选 |

---

## 🔗 九、相关引用

| 引用 | 路径 |
|---|---|
| 本次改动文件 | `W7_VisionMasterLite/mainwindow.cpp` |
| 已实现算子 | `W7_VisionMasterLite/operators/*.cpp` |
| 画布视图 | `W7_VisionMasterLite/pipeline_view.cpp` |
| W7 README | `W7_VisionMasterLite/README.md` |
| 上次笔记（S1 拖拽） | `notes/notes_Qt_DragDrop_2026-09.md` |
| 上次笔记（S2 连线 + S3 修 bug） | `notes/notes_Qt_NodeConnection_BugFix_2026-09.md` |
| W6 修改日志 | `notes/notes_W6_ComponentRecognition_2026-09.md` |

---

> 💬 **写在最后**：本次改动是 W7 "流程图编辑器"定位的关键转折点。改之前 Run 引擎是"念稿人"，改之后是"听话的工人"。**接下来如果做 P1 Save/Load JSON，Run 引擎本身不需要任何修改**——它已经是图驱动、声明式的。这也是为什么"先打通数据流，再替换实现"在 Run 引擎这种核心架构层同样适用。
