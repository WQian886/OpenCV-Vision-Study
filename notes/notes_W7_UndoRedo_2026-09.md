# W7 M6 撤销/重做 + P0 悬垂指针修复 — 变更日志（2026-09-29）

> 📅 **记录时间**：2026-09-29
> 📁 **项目来源**：W7_VisionMasterLite Mini VisionMaster Lite
> ✏️ **改动文件**：8 个（commands/ ×4 + 3 个界面文件 + CMakeLists + 2 份 README）
> 🎯 **本次目标**：修一个真 bug（右键删节点后 Run 段错误）+ 把 M6 撤销重做做完，让 W7 收官

---

## 📌 一句话总结

修掉 P4 遗留的**悬垂指针崩溃**，并把 M6 撤销/重做从「只有地基」推进到「5 个 Command + 双栈 + 快捷键全部可用」。顺带补上**节点 UUID**，一次性解决 Command 定位和 Save/Load 同名歧义两个问题。**W7 从 97% → 100%**。

---

## 📋 一、改了什么

### Edit 1 — `operator_node.h` / `.cpp`（P0 修复 + UUID + 拖动信号）
- **P0**：`contextMenuEvent` 不再 `delete this`，改为 `emit deleteRequested(this)`
- **M6**：构造函数加 `m_uuid(QUuid::createUuid())`；`mousePressEvent` 记录 `m_dragStartPos`；新增 `mouseReleaseEvent`，位置真变了才 `emit nodeMoved(this, old, new)`
- 新增 2 个信号：`deleteRequested` / `nodeMoved`

### Edit 2 — `pipeline_view.h` / `.cpp`（信号转发 + UUID 查询）
- `addNode` 里 connect 两个新信号并转发
- 新增 `findNodeByUuid()` / `findConnection()` 两个查询接口
- `dropEvent` 不再直接 `addNode` / `addConnection`，改为 `emit nodeAddRequested` / `connectionAddRequested`（**关键：否则新增动作绕过命令栈，Ctrl+Z 撤不掉**）

### Edit 3 — `commands/node_commands.h` / `.cpp`（新增，3 个 Command）
- `AddNodeCmd` / `DeleteNodeCmd` / `MoveNodeCmd`

### Edit 4 — `commands/edge_commands.h` / `.cpp`（新增，2 个 Command）
- `AddEdgeCmd` / `RemoveEdgeCmd`

### Edit 5 — `mainwindow.h` / `.cpp`（接线）
- 工具栏加 `↶ Undo` / `↷ Redo` 两个 QAction（自带快捷键 + 置灰态）
- `execUndo` / `execRedo` 加 `m_applyingHistory` 重入防护
- 新增 `clearUndoRedoStacks()` / `updateUndoRedoActions()` / `doAddNode` / `doDeleteNode` / `doMoveNode` / `doConnect`
- `loadDefaultPipeline` 和 `onLoadProject` 末尾清空命令栈
- 析构函数里 `clearUndoRedoStacks()`（QStack 不析构元素，必须手动 delete）
- `deleteSelectedItems` 改为走 Command
- Save/Load 加 UUID 字段

---

## 🎯 二、目的是什么

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| W7 进度 | 97% | **100%** |
| 算子数 | 7 | 7（不变） |
| Command 子类 | **0** | **5** |
| 崩溃 bug | 1（右键删后 Run） | **0** |
| 节点身份 | op 名 + FIFO | **QUuid** |
| 快捷键 | 仅 Run/Save/Load | + `Ctrl+Z` / `Ctrl+Y` |

### 业务目标
- Ctrl+Z 是**桌面软件的肌肉记忆**。VM / Halcon / Visio / Photoshop 全都有 —— 缺了它 W7 就不像个"正经工具"
- 一个会崩的工具，"完成度 97%" 是虚的。先止血，再谈功能

---

## 🤔 三、为什么要这么改

### 为什么先修 P0 而不是直接做撤销重做？

因为**撤销重做的每一步都在遍历节点**。悬垂指针状态下，`DeleteNodeCmd` 构造函数会去读 `node->pos()`、`node->params()`、遍历 `m_canvas->connections()` 找边 —— 拿着野指针读，等于把一个崩溃换成另一个崩溃。**地基不稳不能盖楼**。

### 为什么在 M6 里顺带加 UUID？（范围蔓延吗？）

不是蔓延，是**正确性前提**。设想没有 UUID 的世界：

```
用户操作：移动节点A → 删除节点A → 撤销(删除) → 撤销(移动)
```

- 撤销删除会**重建**节点A → 内存分配器很可能给出**不同地址**
- 栈里那个 MoveNodeCmd 握着**旧地址**
- 再撤销移动 → `findNode(旧地址)` → 访问已释放内存 → 段错误

节点被销毁重建，地址就失效了 —— 这是 `new/delete` 的固有性质，加多少层包装都绕不开。**唯一稳定的身份是 UUID**：创建时生成，重建时沿用，命令永远能找到正确的节点。

而 UUID 恰好也是 README 里那条「仍待补：节点 UUID（解决同名节点 edge 引用歧义）」。**一个改动消掉两个债**。

### 为什么用 QUuid 而不是 QString id？

`QUuid` 是 Qt 内置类型，128 位随机数，`QUuid::createUuid()` 一行生成，`toString(WithoutBraces)` 直接进 JSON。无需自己管理唯一性。

### 为什么 `dropEvent` 改成「发请求」而不是「直接建」？

这是最容易被忽略、但**不改就白做**的一处。`dropEvent` 在 `PipelineView` 里，它直接调 `addNode` 的话，节点就被"偷偷"加上了 —— 命令栈里什么都没有，Ctrl+Z 按下去毫无反应。**必须把决策权交给 MainWindow**，由它包成 `AddNodeCmd` 再执行。

同理 `connectionAddRequested`。信号名带 `Requested` 就是这个意思。

### 为什么 `loadDefaultPipeline` 也要清栈？

默认流水线是**起始状态**，不是用户操作。如果不清，用户启动后第一件事是拖一个节点，然后连按 3 次 Ctrl+Z —— 期望是回到默认流水线，实际会把默认流水线本身拆掉。**没有历史 = 正确的历史**。

### 为什么 Load 之后清栈、而不是让旧命令 no-op？

旧命令的 UUID 在新画布里根本不存在。按 Ctrl+Z 会「没反应」但按钮是亮的 —— 比崩溃更让人困惑（用户会以为程序卡了）。清栈 + 按钮置灰，行为可预期。

---

## 💡 四、有成型的解决方案吗？

### 4.1 撤销重做的三种范式

| 方案 | 做法 | 优缺点 | 本项目 |
|---|---|---|---|
| **A. Command 模式** | 每次操作包成对象，双栈 | 教科书解法，可加日志/事务 | ✅ 采用 |
| B. `QUndoStack` + `QUndoCommand` | 用 Qt 自带 | 少写样板代码，但混入 Qt 体系 | ❌ M6-1 已定 Command 基类 |
| C. 快照式（每次存全图 JSON） | undo = 恢复上一份 JSON | 实现极简，但内存/性能随规模爆炸 | ❌ 节点多了就卡 |

Command 模式胜在**内存开销与操作数成正比**（移动节点只存两个 QPointF，不存整张图），且天然支持"重做"和操作日志 —— 我们的 `desc()` 就是给日志用的。

### 4.2 节点身份的四种选择

| 方案 | 优缺点 |
|---|---|
| op 名 | 同名冲突（两个 color_segment） |
| 内存地址 | 重建即失效 ← **就是我们要避开的坑** |
| 自增整数 id | 可行，但需全局计数器 + 持久化时防冲突 |
| **QUuid** | 无需管理，天然唯一，JSON 友好 ✅ |

### 4.3 成熟平台的做法对照

| 平台 | 删除后撤销 | 节点身份 |
|---|---|---|
| 海康 VM | 支持 | 内部 ID |
| Halcon HDevelop | 支持 | 图元句柄 |
| Visio / draw.io | 支持 | 稳定的 shape id |

**共同点**：都是「稳定 ID + 命令栈」，不是「指针 + 快照」。我们这版已经和它们对齐了。

---

## ⚠️ 五、踩坑记录

### 坑 1：undo 会触发 nodeMoved，导致一次 Ctrl+Z 压回一堆命令

**症状**：撤销一次删除，命令栈深度反而暴涨。

**原因**：`MoveNodeCmd::undo()` 调 `node->setPos(oldPos)` → Qt 发位置变化 → `mouseReleaseEvent` 可能被触发 → `nodeMoved` 信号 → MainWindow 又 `execCommand(MoveNodeCmd)`。**撤销动作自己产生的位移，又被当成新操作记了一笔。**

**修复**：`m_applyingHistory` 标志。`execUndo`/`execRedo` 期间置 true，`doMoveNode` 里 `if (m_applyingHistory) return;`。

> 教训：任何「改变状态 → 触发信号 → 信号又改状态」的双向链路，都需要一个方向锁。Qt 的 `QSignalBlocker` 是同类问题的通用解法。

### 坑 2：QStack 不析构元素 → Command 内存泄漏

`QStack<T>` 继承 `QVector<T>`，只管指针不管内存。栈里的 `new AddNodeCmd` 永远不会被 delete。必须在 `clearUndoRedoStacks()` 和析构函数里 `qDeleteAll`。

### 坑 3：多选删除时，同一条线被删两次

**症状**：Delete 键删「节点A + 与A相连的线」，命令栈里出现重复记录，撤销后线又冒出来。

**原因**：`connsToDelete` 同时包含了「被选中的线」和「沾到待删节点的线」。走 Command 后，`DeleteNodeCmd::exec` 已经连带删了后一部分，若再对它们 `execCommand(RemoveEdgeCmd)`，就重复了。

**修复**：遍历 `connsToDelete` 时检查该线是否沾了待删节点，沾了就 `continue`（它已经被 `DeleteNodeCmd` 记录在其 `m_edges` 里，undo 时会一并重建）。

### 坑 4：Load 新工程后旧命令变成「哑弹」

见「三、为什么 Load 之后清栈」。清栈后按钮同步置灰，行为可预期。

### 坑 5：README 里的悬空引用

`notes_W7_ImageSourceCamera_2026-09.md` 引用了 `notes/notes_W7_路线C_2026-09.md` —— **该文件不存在**（路线 C 计划在某次整理中丢了）。本次改为指向本篇。

### 坑 6：`command.h` 的前置声明在全局作用域

**症状**：写测试时把 `pipeline_view.h` 和 `command.h` 一起 include，编译爆了一屏：

```
error C2872: "PipelineView": 不明确的符号
  notes: 可能是 "PipelineView"      ← 全局作用域那个
  notes: 或 "w7::PipelineView"      ← 真正的类
```

**原因**：`command.h` 里写的是 `class PipelineView;`（**全局**），而实际类在 `namespace w7` 里。这是 M6-1 遗留的一个多余前置声明 —— 基类根本不用 `PipelineView`，只有子类实现才需要。

**为什么之前没暴露**：`mainwindow.h` 里 `command.h` 排在 `pipeline_view.h` **后面**，真实类先声明，全局那个被"遮住"了不报错。测试的 include 顺序不同，冲突就现形了。

**修复**：把它挪进 `namespace w7`，并加注释说明只有子类实现才需要。

> 教训：**include 顺序能掩盖的错误，换个人换个顺序就会炸。**
> 前置声明要么放在正确的命名空间，要么干脆删掉。

---

## ✅ 六、验收结果

### 编译
```
[1/10] Automatic MOC and UIC for target w7_vm_lite
[2/10] Building CXX object commands\edge_commands.cpp.obj
[3/10] Building CXX object connection_line.cpp.obj
[4/10] Building CXX object operator_node.cpp.obj
[5/10] Building CXX object commands\node_commands.cpp.obj
[6/10] Building CXX object main.cpp.obj
[7/10] Building CXX object pipeline_view.cpp.obj
[8/10] Building CXX object w7_vm_lite_autogen\mocs_compilation.cpp.obj
[9/10] Building CXX object mainwindow.cpp.obj
[10/10] Linking CXX executable w7_vm_lite.exe
```
- ✅ 0 error

### 启动
- ✅ exe 启动 6 秒无崩（PID 45388），默认流水线正常加载

### 🧪 无头回归测试（`tests/test_undo_redo.cpp`）

> 补记：本文档初版写的是「7 条清单需手动点鼠标验证」。实测发现
> 大部分状态转换**不需要鼠标就能验**，故补了无头测试。
> 教训：写「TODO：手动验证」之前先想想能不能自动化 —— 手动清单
> 不可重复，改一行代码就得重点一遍。

```powershell
cmake --build build --target w7_undo_test
$env:QT_QPA_PLATFORM="offscreen"; build\w7_undo_test.exe
```

- ✅ **50 断言全过，退出码 0**（`SUMMARY: pass=50 fail=0 RESULT=PASS`）
- ✅ 只编译画布 + Command 层（这 4 个文件不依赖 `cv::`），**测试目标不链接 OpenCV**，构建约 3 秒、运行约 1 秒
- ✅ `QT_QPA_PLATFORM=offscreen` 无需显示器
- ✅ 已挂 `ctest`（`add_test(NAME undo_redo ...)`）

| # | 用例 | 验证点 |
|---|---|---|
| 1 | 增删节点往返 | 基本语义 |
| 2 | AddNode redo UUID 稳定 | 重做出的节点能和已有连线对上 |
| 3 | 删中间节点连带删线，撤销后线恢复 | 连线的生命周期归谁管 |
| 4 | 删节点保留 params | 撤销后参数面板不能是空的 |
| 5 | **移动→删除→撤销删除→撤销移动** | **悬垂指针回归** |
| 6 | 连线增删往返 | 端点顺序正确 |
| 7 | 单独删连线往返 | — |
| 8 | 新操作清空 redo 栈 | 标准 Command 语义 |
| 9 | 节点消失后命令静默 no-op | Load 后按 Ctrl+Z 不崩 |
| 10 | 50 个同名节点 UUID 全不同 | UUID 地基 |
| 11 | 同名节点连线按 UUID 精确指认 | 旧 FIFO 方案会错的场景 |
| 12 | clearPipeline 不留悬垂指针 | scene / 容器一致性 |

**用例 5 的实测输出**（本次最有价值的一条）：

```
(重建前后地址: 0x150f5374740 → 0x150f53748a0)
```

同一个逻辑节点，删除再撤销后**地址确实变了**。这从实证上确认了：
`MoveNodeCmd` 存裸指针就是错的，**UUID 不是过度设计，是硬需求**。

### 仍需手动确认的部分

以下依赖真实鼠标交互 / 视觉反馈，测试覆盖不到：
- [ ] 拖节点 / 连线的**手感**（拖拽是否跟手、落点容差 50px 是否顺手）
- [ ] 撤销移动后**命令栈深度不暴涨**（坑 1 的方向锁已加，但方向锁本身在 MainWindow 层，测试复刻的是 Command 语义，不含信号重入）
- [ ] 参数面板在撤销后是否正确刷新
- [ ] 工具栏 Undo/Redo 按钮的置灰态视觉

---

## 📊 七、关键代码片段

### 撤销时的方向锁（坑 1 的解法）

```cpp
void MainWindow::execUndo() {
    if (m_undoStack.isEmpty()) { ... return; }
    // undo 改位置 → 触发 nodeMoved → 不加锁会又生成 MoveNodeCmd
    m_applyingHistory = true;
    Command* cmd = m_undoStack.pop();
    cmd->undo();
    m_redoStack.push(cmd);
    m_applyingHistory = false;
    ...
}

void MainWindow::doMoveNode(OperatorNode* node, const QPointF& oldPos, const QPointF& newPos) {
    if (!node) return;
    if (m_applyingHistory) return;   // ← 撤销自身引起的位移不记录
    execCommand(new w7::MoveNodeCmd(m_canvas, node->uuid(), oldPos, newPos));
}
```

### Command 按 UUID 定位（而不是裸指针）

```cpp
void MoveNodeCmd::undo() {
    // 地址会变（节点可能已删除重建），UUID 不会
    if (OperatorNode* n = m_canvas->findNodeByUuid(m_nodeId)) {
        n->setPos(m_oldPos);
    }
    // 找不到 → 静默 no-op，不崩（比如 Load 清空后按了 Ctrl+Z）
}
```

### DeleteNodeCmd 的快照（撤销要重建节点）

```cpp
DeleteNodeCmd::DeleteNodeCmd(PipelineView* canvas, OperatorNode* node) {
    // 节点马上要被 delete，所有重建所需状态先存下来
    m_opName = node->opName();  m_params = node->params();
    m_pos    = node->pos();     m_uuid   = node->uuid();
    // 连带删除的线也要记住，undo 时重建
    for (auto* line : canvas->connections()) {
        if (line->fromNode() == node || line->toNode() == node)
            m_edges.append({line->fromNode()->uuid(), line->toNode()->uuid()});
    }
}
```

### 画布不自己改图（否则绕过命令栈）

```cpp
// PipelineView::dropEvent
emit nodeAddRequested(meta.name, meta.display, meta.category, scenePos);
// 曾经是: addNode(...)  ← 这样 Ctrl+Z 撤不掉
```

---

## 🔄 八、仍待补

| 优先级 | 任务 | 估时 |
|---|---|---|
| 🟡 中 | 手动验收剩下 4 条交互项（手感 / 方向锁 / 参数面板刷新 / 按钮态） | 20 min |
| 🟡 中 | input pin 类型检查（算子声明需要哪些 key，连线时验证） | 1-2 h |
| 🟢 低 | 最近文件列表（启动显示最近 5 个 `.sol.json`） | 30 min |
| 🟢 低 | `image_source_video`（视频文件源，补齐 VM 的 4 种图像源） | 1 h |
| 🟢 低 | 复杂子板 SITES 适配（不同尺寸/布局的 PCB） | 2 h |
| ⚪ 远期 | 真实 PCB 测试 + HSV 颜色校准 | 半天 |
| ⚪ 远期 | 算子插件化（DLL 热加载） | 1-2 天 |
| ⚪ 远期 | 每个节点右键查看该步输出图（VM 的核心体验） | 1 h |

### 建议的下一步

W7 已 100%，**核心目标（理解工业视觉平台的三层架构）达成**。接下来有两条路：

1. **把 W6 深度接进 W7** —— 把 W6 的 PCB 检测算法包成算子挂到画布上，形成完整产线仿真。学习价值最高。
2. **开 W8** —— 新方向（如 DL 算子 / ONNX 推理 / 跨平台移植）。

---

## 🔗 九、相关引用

| 引用 | 路径 |
|---|---|
| 本次新增 | `W7_VisionMasterLite/commands/{node,edge}_commands.{h,cpp}` |
| 本次新增 | `W7_VisionMasterLite/tests/test_undo_redo.cpp`（12 用例 / 50 断言）|
| 本次修改 | `W7_VisionMasterLite/{mainwindow,pipeline_view,operator_node}.{h,cpp}` |
| M6-1 基类 | `W7_VisionMasterLite/commands/command.h` |
| 上次笔记 | `notes/notes_W7_ImageSourceCamera_2026-09.md` |
| P4 遗留问题记录 | `notes/notes_W7_P2P5_Finalize_2026-09.md`（坑 2 首次记录了悬垂指针）|
| W7 README | `W7_VisionMasterLite/README.md` |

---

> 💬 **写在最后**：P4 当时为了「MVP 演示够用」用了 `delete this` 的简化方案，笔记里也如实记了「已知限制」。这次补上信号转发，`PipelineView::removeNode` 里早就写好的正确清理逻辑终于被真正用上了 —— 绕了一圈，答案其实早就在仓库里，只是当时没接上。
>
> 撤销重做看着是"加功能"，实际它逼出了一个更底层的问题：**对象身份不能靠地址**。这个认知比 Ctrl+Z 本身值钱。
