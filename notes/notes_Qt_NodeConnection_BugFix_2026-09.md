# W7 Mini VM S2 — 节点连线与 Bug 修复 学习笔记

> 📅 **整理时间**:2026-09-14
> 📁 **项目来源**:W7 Mini VisionMaster Lite S2(节点间真实连线)+ 3 个 bug 修复
> 🎯 **学习目标**:沉淀"Qt QGraphicsScene 高级用法"+"bug 调查方法论"

---

## 📌 一句话总览

**S2 = 节点端口 hit test + QDrag 启 drag + QPainterPath 画贝塞尔连线 + scene itemChange 让连线跟随节点移动。3 个 bug 修复让我深刻理解了 Qt 框架的"事件生命周期"和"对象所有权"。**

---

## 1️⃣ S2 要解决的问题

S1 完成后画布上节点能拖,但节点之间没有连线(S2 前的 mainwindow.cpp 是硬编码 3 条 QLineF)。S2 解决两个核心问题:

```
问题 1: 节点如何连?
   - 节点 A 的输出端口(右圆) → 拖到 → 节点 B 的输入端口(左圆) → 生成连线

问题 2: 节点移动时连线如何跟随?
   - 节点位置变化 → 连线两端自动重画
```

---

## 2️⃣ 核心知识点 5 个

### 2.1 QGraphicsObject vs QGraphicsItem

**关键区别**:`QGraphicsItem` **不继承 QObject**,而 `QGraphicsObject` **继承自 QGraphicsItem + QObject**。

📁 `operator_node.h`:
```cpp
// ❌ 错误:无法给 QDrag 当 parent
class OperatorNode : public QGraphicsItem { };

// ✅ 正确:QGraphicsObject 自带 QObject,可作为 QDrag 的 parent
class OperatorNode : public QGraphicsObject {
    Q_OBJECT
public:
    // ...
};
```

**为什么需要 QObject?** `QDrag` 的构造函数需要 `QObject* parent`(用于父子对象树内存管理):

```cpp
QDrag* drag = new QDrag(this);   // ← this 必须是 QObject*
```

**踩坑**:我一开始用 `QGraphicsItem`,然后写 `new QDrag(this)` 编译报"C2665 无法转换",就是因为 QGraphicsItem 不是 QObject。改继承为 `QGraphicsObject` + 加 `Q_OBJECT` 宏即可。

> 🔌 **PLC 类比**:`QGraphicsItem` 是"纯几何图元",`QGraphicsObject` 是"带信号的图元"——后者多了事件 / 信号槽能力,可以和 Qt 信号系统集成。

---

### 2.2 QPainterPath 画贝塞尔曲线

**什么是 QPainterPath**?Qt 的"路径对象",可以画直线、贝塞尔曲线、矩形、任意多边形,最后用 `QPainter::drawPath()` 一气呵成画出来。

📁 `connection_line.cpp`:
```cpp
QPointF start = mapFromScene(p1);
QPointF end   = mapFromScene(p2);

// 控制点:从起点向右拉,终点向左拉(做出"S 形"贝塞尔)
QPointF ctrl1(start.x() + 60, start.y());
QPointF ctrl2(end.x()   - 60, end.y());

QPainterPath path(start);
path.cubicTo(ctrl1, ctrl2, end);    // ← 三阶贝塞尔

p->setPen(QPen(QColor(80, 80, 80), 2));
p->drawPath(path);                   // ← 一行画完整条曲线
```

**三阶贝塞尔 (`cubicTo`) 的 4 个点**:
- 起点(已 moveTo)
- ctrl1:起点切线方向的控制点
- ctrl2:终点切线方向的控制点
- 终点

**为什么 ctrl1/ctrl2 要水平拉?** 因为节点是水平排布的,水平方向的控制点能让曲线看起来是"S 形"(像 VM EasyBuilder 那样)。

> 🔌 **PLC 类比**:贝塞尔曲线 = "用 4 个点描述一条平滑曲线",起点终点是必到,2 个控制点是"引力"——拉力越大,曲线越弯。

---

### 2.3 QPainterPathStroker 加宽 hit 区(关键!)

**问题**:S2 完成后,用户报告"无法选中连线"。原因——曲线只有 **2px 宽**,鼠标几乎点不中。

**解决**:override `shape()` 返回一个**加宽的"路径"**,作为 hit 测试区域。`QPainterPathStroker` 可以把任意路径"描边"成给定宽度的带状区域。

📁 `connection_line.cpp`:
```cpp
QPainterPath ConnectionLine::shape() const {
    // 先画一条细的贝塞尔路径
    QPainterPath path;
    path.moveTo(start);
    path.cubicTo(ctrl1, ctrl2, end);

    // 用 stroker 把路径加宽(16 像素的"命中带")
    QPainterPathStroker stroker;
    stroker.setWidth(16);                 // 16 像素宽
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(path);    // ← 加宽后的路径
}
```

**效果**:用户鼠标只要在曲线**附近 16px 范围内**,就算"命中"。

**踩坑**:一开始我以为 shape() 默认会"自动包含 boundingRect",结果默认只是 boundingRect 的矩形,根本不会包含细曲线本身。

---

### 2.4 shape() vs boundingRect() 区别

| API | 用途 | 默认行为 |
|---|---|---|
| `boundingRect()` | paint 区域(决定 Qt 何时重画) | 整个 item 的矩形 |
| `shape()` | hit test 区域(决定鼠标点中哪) | 同 boundingRect(矩形) |
| `contains(point)` | 自定义点测试(可选 override) | 委托给 shape() |

**核心规则**:
- 想让 paint 更精细 → override `paint()`
- 想让 hit 区更宽或更准 → override `shape()`
- `boundingRect()` 必须**包含** `shape()`,否则 hit 区域超出 paint 区域会出问题

**Mini VM 应用**:
- ConnectionLine:boundingRect 是"曲线 + 30px 余量"(画线区域),shape 是"16px 描边的曲线"(命中区)
- 两者大小不同,这是 Qt 允许的(只要 shape ⊆ boundingRect)

---

### 2.5 setFlag(ItemIsSelectable) 才能选中

**问题**:S2 完成后连线**根本无法选中**——根因是 `ConnectionLine` 构造函数**漏了这行**:

📁 `connection_line.cpp`:
```cpp
ConnectionLine::ConnectionLine(...) {
    setZValue(-1);
    setFlag(QGraphicsItem::ItemIsSelectable, true);   // ← 漏了就选不中!
}
```

**Qt QGraphicsItem 标志位**:
| Flag | 作用 |
|---|---|
| `ItemIsMovable` | 鼠标可拖动 |
| `ItemIsSelectable` | 鼠标可选中(默认 false!) |
| `ItemIsFocusable` | 可获键盘焦点 |
| `ItemSendsScenePositionChanges` | 触发 itemChange |

**为什么默认 selectable = false?** Qt 设计哲学:不是所有图元都该可选(比如装饰性背景),所以默认禁用。

> 🔌 **PLC 类比**:相当于 PLC 里某个 bit 默认 0——要主动 set 才能用。

---

## 3️⃣ 节点跟随移动(itemChange)

**问题 2 解答**:节点移动时,连线怎么知道要重画?

📁 `operator_node.cpp`:
```cpp
QVariant OperatorNode::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionChange) {
        // === 修复 1: 拦截位置变更,把节点限制在 sceneRect 内 ===
        QPointF newPos = value.toPointF();
        if (scene()) {
            QRectF bounds = scene()->sceneRect().adjusted(20, 20, -20, -20);
            newPos.setX(qBound(bounds.left(),   newPos.x(), bounds.right()));
            newPos.setY(qBound(bounds.top(),    newPos.y(), bounds.bottom()));
        }
        return newPos;
    }
    if (change == ItemPositionHasChanged) {
        // 节点位置变化后,通知 scene 重画(连线会自动重画)
        if (scene()) scene()->update();
    }
    return QGraphicsItem::itemChange(change, value);
}
```

**原理**:
- `ItemPositionChange`:**位置变更前**调用 → 可以**拦截 / 修改**新位置(用来限制范围)
- `ItemPositionHasChanged`:**位置变更后**调用 → 只能**通知**重画
- 必须 `setFlag(ItemSendsScenePositionChanges, true)` 才能触发

**为什么能限制范围?** `ItemPositionChange` 返回的 QVariant 就是 Qt 用来设置的位置,**返回修改后的位置就是"过滤"**。

---

## 4️⃣ 5 个踩过的坑(现象 → 原因 → 修复)

| # | 现象 | 原因 | 修复 |
|---|---|---|---|
| 1 | `QDrag::QDrag`: 无法从 QWidget* 转换 | Qt6 的 QDrag 构造函数参数是 `QObject*`,不是 `QWidget*` | 改 OperatorNode 继承 `QGraphicsObject`(自带 QObject) |
| 2 | "使用了未定义类型 QGraphicsScene" | operator_node.cpp 用了 `scene()` 但没 include | 加 `#include <QGraphicsScene>` |
| 3 | 节点能拖出画布边界 | `ItemIsMovable` 只让能拖,不限制范围;`sceneRect` 设太大 | override `itemChange`,在 `ItemPositionChange` 用 `qBound` clamp 位置 |
| 4 | Delete 节点程序崩溃 | 循环里 `delete *it` + `erase`,Qt scene 回调访问野指针 | "先从容器全部移除,再统一 delete"(collect-then-delete) |
| 5 | 连线无法选中 | ConnectionLine 没 `setFlag(ItemIsSelectable)`,且 shape() 默认只返回 boundingRect(细线几乎点不中) | 加 selectable flag + override shape() 用 QPainterPathStroker 加宽到 16px |

---

## 5️⃣ Bug 调查方法论(可复用)

每个 bug 都按"3 步走"调查:

```
1. 复现:用户报告 → 我尝试复现 → 确认现象
2. 定位:读代码找根因(从用户描述的"现象"反推"代码位置")
3. 修复:写最小修复 + 编译验证 + 让用户回归测试
```

### 调查工具箱

| 工具 | 用途 |
|---|---|
| `git log` / `git diff` | 看最近改了什么 |
| **代码搜索**(Grep) | 找所有调用可疑函数的地方 |
| **读头文件**(Read) | 看类的完整接口 |
| **断点调试** | Qt Creator / VS 调试器单步跟踪 |
| **printf 日志** | 在关键位置加 `qDebug() << ...`,看执行流程 |

### 调查 Bug 4(删除崩溃)的思路

```
现象:删除节点后程序崩溃
↓
猜:可能是双重释放 / 野指针访问 / scene 回调问题
↓
看代码:PipelineView::removeNode 循环里 delete + erase
↓
验证:Qt scene 在 delete 时触发 selectionChanged / update 回调
↓
查文档:Qt 文档说"从 scene remove 后再 delete 是安全的"——但实际上有 update race
↓
修法:改成 collect-then-delete(先从容器移除,再统一 delete)
```

### 调查 Bug 5(连线不可选中)的思路

```
现象:点击曲线没反应
↓
猜:可能 hit test 区域不对 / selectable flag 没设
↓
查文档:QGraphicsItem 默认 ItemIsSelectable = false
↓
看代码:ConnectionLine 构造函数没 setFlag
↓
测试:加上 flag → 能选中了!但只能选中"线本身"(2px 太细)
↓
查文档:shape() 默认用 boundingRect,对细线不友好
↓
修法:override shape() 用 QPainterPathStroker 加宽到 16px
```

---

## 6️⃣ 删除安全模式(collect-then-delete)

**核心原则**:Qt scene 内部会通过信号/回调访问 item。**在循环里 delete item 会和回调竞争**。

**正确模式**:分 5 步走

```cpp
void MainWindow::deleteSelectedItems() {
    // 1. 收集
    QSet<OperatorNode*>   nodesToDelete;
    QSet<ConnectionLine*> connsToDelete;
    for (auto* item : scene->selectedItems()) {
        if (auto* line = dynamic_cast<ConnectionLine*>(item)) connsToDelete.insert(line);
        if (auto* node = dynamic_cast<OperatorNode*>(item))   nodesToDelete.insert(node);
    }

    // 2. 扩展(涉及的连线也加进来)
    for (auto* line : m_canvas->connections()) {
        if (nodesToDelete.contains(line->fromNode()) ||
            nodesToDelete.contains(line->toNode())) {
            connsToDelete.insert(line);
        }
    }

    // 3. 从 scene 移除(不 delete)
    for (auto* line : connsToDelete) scene->removeItem(line);
    for (auto* node : nodesToDelete) scene->removeItem(node);

    // 4. 统一 delete(此时 scene 已不再引用,安全)
    for (auto* line : connsToDelete) delete line;
    for (auto* node : nodesToDelete) delete node;

    // 5. 从内部 list 清掉
    m_canvas->connectionsRef().removeAll(...);
    m_canvas->nodesRef().removeAll(...);
}
```

**关键点**:
- ❌ 不要在循环里 `delete *it; it = erase(it);`
- ✅ 先收集,再统一 `delete` / `qDeleteAll`
- ✅ `scene->removeItem` 不等于 `delete`,只是解除关联

> 🔌 **PLC 类比**:工厂流水线,**先把零件从传送带摘下来**(removeItem),再扔到废料箱(delete)。不能"零件还在传送带上就扔"(会卡死传送带)。

---

## 7️⃣ 完整 ASCII 流程图(节点移动时连线跟随)

```
用户拖动 node A
    │
    ▼
QGraphicsItem::mouseMoveEvent
    │
    ▼
QGraphicsItem 自动算新位置 (x, y)
    │
    ▼
QGraphicsItem::itemChange(ItemPositionChange, value)
    │  ← 我们在这里 clamp 位置(防止越界)
    │  ← 返回 clamp 后的位置
    ▼
位置更新到 QGraphicsScene
    │
    ▼
QGraphicsItem::itemChange(ItemPositionHasChanged, value)
    │  ← 我们在这里调 scene->update()
    ▼
scene->update() 调度重绘
    │
    ▼
Qt 事件循环下次迭代
    │
    ▼
所有 item 重画(包含 ConnectionLine)
    │
    ▼
ConnectionLine::paint() 调用
    │  ← m_from->outputPortScenePos() 读新位置
    │  ← m_to->inputPortScenePos() 读新位置
    │  ← cubicTo(...) 画新贝塞尔
    ▼
屏幕显示:节点移动,连线两端跟随 ✅
```

---

## 8️⃣ Troubleshooting 速查表

| 现象 | 原因 | 修复 |
|---|---|---|
| 编译错 "QDrag 参数无法从 QGraphicsItem 转换" | QGraphicsItem 不是 QObject | 改继承 QGraphicsObject + 加 Q_OBJECT |
| 编译错 "未定义类型 QGraphicsScene" | 用了 scene() 但没 include | 加 `#include <QGraphicsScene>` |
| 节点能拖出画布 | 没限制位置范围 | override itemChange + qBound |
| 连线选不中 | 没设 ItemIsSelectable | setFlag(ItemIsSelectable, true) |
| 连线选不中(设了 flag 后) | shape() 太细,点不到 | override shape() 用 QPainterPathStroker 加宽 |
| 删除节点崩溃 | 循环里 delete 触发 scene 回调 | 改成 collect-then-delete 模式 |
| 节点移动连线不跟随 | 没监听 ItemPositionHasChanged | override itemChange + 调 scene->update() |
| 连线显示"虚的"或"不显示" | zValue 没设,被节点挡住 | setZValue(-1) |

---

## 9️⃣ Qt 关键 API 一览表

| API | 用途 | 在 S2 的用法 |
|---|---|---|
| `QGraphicsObject` | 带 QObject 的图元 | OperatorNode 继承它,能给 QDrag 当 parent |
| `QDrag(QObject*)` | 启动拖拽 | OperatorNode 输出端口上启动 |
| `QGraphicsSceneMouseEvent` | 场景鼠标事件 | OperatorNode::mousePressEvent 用 |
| `QPainterPath::cubicTo` | 三阶贝塞尔 | ConnectionLine 画曲线 |
| `QPainterPathStroker::createStroke` | 路径描边 | shape() 加宽 16px hit 区 |
| `QGraphicsItem::shape()` | hit test 形状 | override 加宽命中区 |
| `setFlag(ItemIsSelectable)` | 启用可选中 | ConnectionLine 必加 |
| `itemChange(GraphicsItemChange, QVariant)` | 位置变化监听 | OperatorNode 限制范围 + scene->update |
| `scene()->removeItem(item)` | 解除 scene 关联 | 删除前先调,避免回调 |
| `qDeleteAll(container)` | 批量 delete | clearPipeline 用 |

---

## 🔟 自测题(确认真的懂了)

**Q1**:为什么 OperatorNode 必须是 QGraphicsObject 而不是 QGraphicsItem?
> QDrag 需要 QObject* parent;QGraphicsObject 自带 QObject 接口,QGraphicsItem 没有。

**Q2**:三阶贝塞尔 `cubicTo` 需要几个参数?分别是什么?
> 3 个:ctrl1(起点切线)、ctrl2(终点切线)、end(终点);起点由前面的 moveTo 决定。

**Q3**:shape() 和 boundingRect() 的区别是什么?
> boundingRect 是 paint 区域(决定重画时机),shape 是 hit test 区域(决定鼠标点中);shape ⊆ boundingRect。

**Q4**:为什么默认 ItemIsSelectable = false?
> Qt 设计哲学:不是所有图元都该可选(背景装饰不该可选);要主动 setFlag 才能选中。

**Q5**:为什么不能在循环里 delete item?
> Qt scene 内部通过信号 / update 回调访问 item,循环里 delete 会让回调访问正在析构的对象 → 崩溃。改用 collect-then-delete(先从容器移除,再统一 delete)。

**Q6**:itemChange 的 ItemPositionChange 和 ItemPositionHasChanged 区别?
> 前者是"变更前"(可拦截 / 修改),后者是"变更后"(只能通知);前者用来 clamp 位置,后者用来触发 scene->update()。

**Q7**:QPainterPathStroker 的作用?
> 把任意路径"描边"成给定宽度的带状区域,用于 shape() 加宽 hit 区,让细线也能被点中。

---

## 1️⃣1️⃣ 一句话总结(贴在墙上)

> **S2 = QGraphicsObject + QPainterPath 画曲线 + shape() 加宽 hit + itemChange 跟随。3 个 bug 修复让我学会"Qt 对象所有权"和"delete 必须先从 scene 移除再统一 delete"。**

---

## 1️⃣2️⃣ 下次回顾要点(2 周后回来)

| 主题 | 核心点 | 一句话 |
|---|---|---|
| QGraphicsObject vs QGraphicsItem | 前者带 QObject | 需要信号 / 当 QDrag parent 时用前者 |
| 三阶贝塞尔 | cubicTo(p1, p2, end) | 4 个点决定一条平滑曲线 |
| shape() 加宽 | QPainterPathStroker | 让 2px 细线也能被点中 |
| ItemIsSelectable | 默认 false | 必须 setFlag 才能选中 |
| itemChange | PositionChange 可拦截,HasChanged 只通知 | 一个限制范围,一个触发重画 |
| 删除安全 | 先 remove 再 delete | 避免 scene 回调访问野指针 |

---

## 📚 相关代码位置(W7 Mini VM S2)

| 文件 | 关键改动 |
|---|---|
| `W7_VisionMasterLite/operator_node.h` | 继承改 QGraphicsObject + 端口 hit test API |
| `W7_VisionMasterLite/operator_node.cpp` | mousePressEvent 启动 QDrag + itemChange 限制范围 |
| `W7_VisionMasterLite/connection_line.h` | shape() override + HIT_WIDTH 常量 |
| `W7_VisionMasterLite/connection_line.cpp` | 贝塞尔曲线 + ItemIsSelectable + 加宽 hit 区 |
| `W7_VisionMasterLite/pipeline_view.h/.cpp` | addConnection + removeNode/removeConnection(5 步走) |
| `W7_VisionMasterLite/mainwindow.cpp` | deleteSelectedItems 重写为 collect-then-delete |

---

## 🔗 相关笔记

| 笔记 | 主题 |
|---|---|
| `notes_Qt_DragDrop_2026-09.md` | S1 拖拽原理(工厂传送带 + 4 阶段 + MimeData) |
| `notes_Qt_NodeConnection_BugFix_2026-09.md` | **本笔记** — S2 连线 + 3 个 bug 修复 |

---

## ⏭️ 下一步可做的事

| 任务 | 难度 | 价值 |
|---|---|---|
| **S3: 参数面板动态切换** | 中 | 选中节点能看到算子参数 |
| **改进 S2: 连线选中后支持拖动端点** | 大 | 更灵活,但复杂 |
| **Ctrl+Z 撤销** | 大 | 命令栈 + undo/redo |
| **M1: DAG 流水线执行引擎** | 大 | Run 按钮根据连线自动拓扑排序 |
