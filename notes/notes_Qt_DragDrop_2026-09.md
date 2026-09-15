# Qt 拖拽机制原理与代码实现 — 学习笔记

> 📅 **整理时间**:2026-09-14
> 📁 **项目来源**:W7 Mini VisionMaster Lite 节点拖拽创建功能(S1)
> 🎯 **学习目标**:搞懂 Qt 拖拽的 4 阶段原理 + C++ 代码实现 + Qt 事件循环怎么把它们串起来

---

## 📌 一句话总览

**拖拽 = 源端装料(mimeData)+ Qt 传送 + 目标端卸料(dropEvent)。两端通过自定义 MIME 标签约定"装的是什么"。**

---

## 1️⃣ 为什么需要懂这个?

Mini VisionMaster 的核心交互是"从算子库拖到画布创建节点"。W7 跑通 S1 后,我用工厂传送带类比 + Qt 事件机制,把整个过程拆解了一遍。这份笔记把对话里的精华沉淀下来,以后忘了直接翻。

---

## 2️⃣ 工厂传送带类比(核心心智模型)

> **拖拽 = 把料盒从源工位搬到目标工位**

```
源工位           传送带            目标工位
OpTreeWidget     Qt 拖拽系统       PipelineView
   │               │                 │
装料(mimeData) ──▶ 运送 ──────────▶  验签(dragEnter)
                  跟鼠标移动          卸料(dropEvent)
```

- **源工位装料**:OpTreeWidget 重写 `mimeData()`,把"颜色分割"对应的英文名 "color_segment" 放进 `QMimeData`
- **传送带搬运**:Qt 内置的 QDrag 对象,鼠标拖到哪它就到哪
- **目标工位验签**:PipelineView 的 `dragEnterEvent` 检查 MIME 标签对不对
- **目标工位卸料**:`dropEvent` 解析数据 + 转换坐标 + 创建节点

---

## 3️⃣ 拖拽的 4 个阶段(ASCII 流程)

```
[1. 按下] mousePressEvent
   │  Qt 检查: setDragEnabled(true)?
   │  按住距离 > 阈值 → 进入拖拽模式
   ▼
[2. 拖动] mouseMoveEvent (Qt 自动)
   │  系统调 mimeData() 拿数据 ──┐
   │                              │
   ▼                              ▼
[3. 悬停] dragEnterEvent      返回 QMimeData*
   │  目标端问:你的 mime 对得上吗?
   │  对得上 → acceptProposedAction()
   │  对不上 → event->ignore()
   ▼
[4. 释放] dropEvent
   │  解析 mime 数据 → 算子英文名
   │  mapToScene(鼠标点) → 画布坐标
   │  addNode(...) 创建节点
   ▼
[完成] Qt 自动清理 QMimeData (delete)
```

---

## 4️⃣ MimeData 是什么?

**本质**:一个**装数据的快递盒**,源端装、目标端拆。

```
QMimeData 快递盒
   │
   ├─ text/plain:        "颜色分割"   ← Qt 默认就有的(给系统用)
   ├─ text/html:         "<b>...</b>" ← 可选
   └─ application/x-vm-operator: "color_segment"  ← 你自定义的
                                                          (画布只认这个)
```

### 为什么要自定义 MIME 类型?

你可以同时拖文字、图、文件、自定义数据,**目标端只接收它想要的**。

- 文件管理器拖文件 → mimeData 里有 `text/uri-list`(文件路径)
- 我们拖算子 → mimeData 里有 `application/x-vm-operator`(算子英文名)
- **目标端只看自己订阅的 mime 类型**,对不上的直接 ignore

> 💡 MIME 全称 Multipurpose Internet Mail Extensions(多用途互联网邮件扩展),Qt 把它借用到拖拽上,本质就是"多格式数据装一个盒子里,接收方按需取"。

---

## 5️⃣ 4 阶段 + Qt API 对照

| 阶段 | 触发条件 | 源端要做 | 目标端要做 |
|---|---|---|---|
| **1. 按下** | 鼠标按下 | `setDragEnabled(true)` | (无) |
| **2. 拖动** | 鼠标移动>阈值 | (Qt 自动) | `setAcceptDrops(true)` |
| **3. 悬停** | 鼠标进入目标 | 提供 `mimeData()` | `dragEnterEvent` + `dragMoveEvent` |
| **4. 释放** | 鼠标松开 | (无) | `dropEvent` |

**关键点**:
- 源端只要**声明能拖**(`setDragEnabled`) + **提供数据**(`mimeData()`)
- 目标端要**声明能收**(`setAcceptDrops`) + **响应 3 个事件**(dragEnter/dragMove/drop)

---

## 6️⃣ 5 段关键 C++ 代码解读

### 6.1 装料:OpTreeWidget::mimeData() (源端打包)

📁 `mainwindow.h`:
```cpp
class OpTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        auto* md = new QMimeData;
        for (auto* it : items) {
            QString opName = it->data(0, Qt::UserRole).toString();
            if (!opName.isEmpty()) {
                md->setData("application/x-vm-operator", opName.toUtf8());
                break;
            }
        }
        return md;
    }
};
```

| 行 | 做了什么 |
|---|---|
| `class OpTreeWidget : public QTreeWidget` | 继承 QTreeWidget,**只为了 override mimeData** |
| `QMimeData* mimeData(...) override` | Qt 的**虚函数钩子**,开始拖时自动调用 |
| `auto* md = new QMimeData` | 创建快递盒(堆内存) |
| `md->setData("application/x-vm-operator", opName.toUtf8())` | 把算子英文名塞进快递盒 |
| `break` | 只取第一个选中的算子 |
| `return md` | 快递盒交给 Qt 传送带 |

> 🔌 **类比**:装料工把零件贴上"VM 专用标签"放进料盒,交给传送带。

---

### 6.2 开闸:两个开关调用(两端各开一个)

📁 `mainwindow.cpp` `setupOperatorLibrary()` 末尾:
```cpp
m_opLib->setDragEnabled(true);
m_opLib->setDragDropMode(QAbstractItemView::DragOnly);
m_opLib->setSelectionMode(QAbstractItemView::SingleSelection);
```

📁 `pipeline_view.cpp` 构造函数:
```cpp
setAcceptDrops(true);
```

### 这 4 行做了什么?

| 调用 | 内部做了什么 | 不写会怎样 |
|---|---|---|
| `setDragEnabled(true)` | 注册 `mousePressEvent` 事件过滤器,检测"按下+移动>阈值"就启动 QDrag | 按下没反应 |
| `setDragDropMode(DragOnly)` | 告诉 Qt "我是源不是目标",不要在自己内部接收 drop | 可能触发自己的 dropEvent 死循环 |
| `setSelectionMode(SingleSelection)` | 限制一次只能选 1 个 | 拖多个时 break 只取第一个 |
| `setAcceptDrops(true)` | 安装 dragEnter/Move/Leave/Drop 事件监听器 | 拖进来没反应 |

> ⚡ 这 4 个 setter **没有立刻执行任何拖拽逻辑**,它们只是**改了对象内部的 flag**,Qt 事件分发器看到 flag 后才会把对应事件转发过来。等价于 PLC 里的"使能信号"——使能了,后续事件才会触发。

---

### 6.3 验签:dragEnterEvent / dragMoveEvent (目标端检查)

📁 `pipeline_view.cpp`:
```cpp
void PipelineView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(MIME_OPERATOR)) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PipelineView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat(MIME_OPERATOR)) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}
```

| 行 | 做了什么 |
|---|---|
| `void dragEnterEvent(QDragEnterEvent*)` | **Qt 虚函数钩子**,鼠标拖入 widget 边界时自动调 |
| `event->mimeData()` | 拿到源端装好的快递盒 |
| `hasFormat(MIME_OPERATOR)` | 问快递盒:"你有没有贴 `application/x-vm-operator` 这个标签?" |
| `acceptProposedAction()` | **告诉 Qt:我接受这个 drop** |
| `event->ignore()` | **告诉 Qt:我不要** |

### 🎯 为什么两个事件都要写?
- `dragEnterEvent`:鼠标**第一次**进入 widget 时调(决定能不能接收)
- `dragMoveEvent`:鼠标**在内部移动**时**反复**调(持续确认意愿)
- 两者逻辑一样 → 复制粘贴

> 🔌 **类比**:卸料工看到料盒,先看标签——是"VM 专用"吗?是 → 解锁传送带;不是 → 拒绝。

---

### 6.4 卸料:dropEvent (目标端真正处理)

📁 `pipeline_view.cpp`:
```cpp
void PipelineView::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasFormat(MIME_OPERATOR)) {
        event->ignore();
        return;
    }

    QByteArray payload = event->mimeData()->data(MIME_OPERATOR);
    QString opName = QString::fromUtf8(payload);

    OpMeta meta;
    if (m_registry.contains(opName)) {
        meta = m_registry.value(opName);
    } else {
        meta.name = opName;
        meta.display = opName;
        meta.category = "detection";
    }

    QPointF scenePos = mapToScene(event->position().toPoint());
    addNode(meta.name, meta.display, meta.category, scenePos);

    event->acceptProposedAction();
}
```

| 行 | 做了什么 | 没写会怎样 |
|---|---|---|
| `dropEvent(QDropEvent*)` | **Qt 虚函数钩子**,鼠标松开时调 | 松开不创建节点 |
| `mimeData->data(MIME_OPERATOR)` | 从快递盒**拆出字节数据** | 不知道拖的是什么 |
| `QString::fromUtf8(payload)` | 字节 → QString | 拿到的是乱码 |
| `m_registry.value(opName)` | 查算子表拿 display/category | 节点没颜色,显示英文名 |
| `mapToScene(event->position().toPoint())` | **关键!屏幕坐标 → 画布坐标** | 节点出现在错位置 |
| `addNode(...)` | 调用 PipelineView::addNode 创建 OperatorNode | 没节点 |
| `acceptProposedAction()` | 告诉 Qt "drop 成功" | Qt 可能以为 drop 失败 |

---

## 7️⃣ 坐标转换:mapToScene 为什么必须?

📁 `dropEvent` 里的关键一行:
```cpp
QPointF scenePos = mapToScene(event->position().toPoint());
```

### 涉及 3 套坐标系

```
[屏幕坐标] event->position()              ← 鼠标点在屏幕哪个像素
       │
       │ mapToScene()
       ▼
[视图坐标] QGraphicsView                 ← 考虑滚动条 + 缩放
       │
       │ (内部自动)
       ▼
[场景坐标] QGraphicsScene                 ← 节点最终坐标(画布上的"世界坐标")
```

### 为什么不能直接用屏幕坐标?
1. **滚动条**:画布可能滚动了 200px,屏幕坐标 (500, 300) 实际在画布是 (700, 300)
2. **缩放**:用户 Ctrl+滚轮缩放 2x,屏幕 (500, 300) 在画布是 (250, 150)
3. **窗口移动**:窗口不在屏幕原点 (0,0)

`mapToScene()` **一次性处理这 3 个变换**,返回节点应放的真实位置。

> 🔌 **PLC 类比**:传感器返回的是"机柜坐标",PLC 要把它换算成"工厂坐标"才能正确控制电机——mapToScene 就是这个坐标变换。

---

## 8️⃣ Qt 事件循环(怎么把它们串起来)

```
         QApplication event loop
                  │
   ┌──────────────┼──────────────┐
   ▼              ▼              ▼
 mousePress    mouseMove      mouseRelease
   │              │              │
   ▼              ▼              ▼
QTreeWidget   QDrag (内部)   QDropEvent
mousePressEvent              │
   │                         │ 路由到 PipelineView
   │ 距离>阈值                │ (setAcceptDrops 决定了)
   ▼                         │
启动 QDrag             dragEnterEvent
   │                         │
   │ 调用 mimeData()         │ 验签
   ▼                         │
返回 QMimeData ──────────────┘
                              │
                          松开鼠标
                              ▼
                          dropEvent
                              │
                              ▼
                          创建节点
```

### 核心要点
- Qt 是**事件驱动**(event-driven)
- `QApplication::exec()` 进入主事件循环
- 每个事件从 OS 收下来 → Qt 分发到对应 widget → 调对应 event handler
- 拖拽本质是 Qt 把"鼠标按下→移动→释放"这一串事件**包装成一个 QDrag 对象**,自动调源/目标两端的方法

---

## 9️⃣ 信号 vs 事件 vs 回调(三个概念区别)

| 机制 | 触发方式 | Qt 用法 | Mini VM 用到没 |
|---|---|---|---|
| **信号 Signal** | `emit` 触发,多对多 | `connect(signal, slot)` | ✅ 算子库 `itemSelectionChanged` |
| **事件 Event** | OS 触发(Qt 包装),1 对 1 | `override event()` 函数 | ✅ 拖拽 4 个 event |
| **回调 Callback** | 函数指针传入,1 对 1 | 第三方库(C 语言风格) | ❌ Mini VM 没用 |

### Mini VM 实际混用了 2 种
- **算子库选中变化** → `connect(itemSelectionChanged, onOperatorSelected)`(信号)
- **拖拽 4 个阶段** → `override dragEnter/dropEvent`(事件)

### 一句话区分
> 信号是**对象主动喊**("我变了!"),事件是**系统敲门**("轮到你了!"),回调是**塞个函数给你**(你内部自己调)。

---

## 🔟 Troubleshooting:为什么拖不动?

| 现象 | 原因 | 修复 |
|---|---|---|
| 鼠标按下没反应 | 源端没 `setDragEnabled(true)` | 加 `m_opLib->setDragEnabled(true)` |
| 拖了不显示"幽灵" | 源端 `mimeData()` 返回 nullptr | 重写 `mimeData()` 返回 `new QMimeData` |
| 拖到目标上没反应 | 目标端没 `setAcceptDrops(true)` | 加 `setAcceptDrops(true)` |
| 拖到了但没 drop | `dragEnterEvent` 没调 `acceptProposedAction()` | 加 `event->acceptProposedAction()` |
| 拖到了但 drop 没效果 | `dropEvent` 写错了 | 检查 `mimeData->data()` 取值 + `mapToScene()` 坐标 |
| 拖到节点上而不是画布 | `mapToScene` 写错 | 用 `event->position().toPoint()` 拿鼠标点 |
| 自定义数据取不到 | mime 类型字符串拼写不一致 | 源端 / 目标端必须**完全相同**的字符串 |
| 多选拖了只创建 1 个 | 没限制 SingleSelection | 加 `setSelectionMode(SingleSelection)` |

---

## 1️⃣1️⃣ 进阶话题(可深挖)

| 话题 | 价值 | Mini VM 暂时没用到 |
|---|---|---|
| **QDrag vs QMimeData** | QDrag 包装了 MimeData + "幽灵图标" | ✅(我们只用了 QMimeData) |
| **DropIndicator 视觉反馈** | 拖到位置画"+号"或"高亮条" | ❌ |
| **跨进程拖拽** | 浏览器拖文件到 Qt 程序 | ❌ |
| **QGraphicsView 特殊点** | drop 坐标系需 mapToScene 转换 | ✅(我们已经处理) |
| **拖拽异步 vs 同步** | dropEvent 是同步,大量数据会卡 | ❌(数据量小,不用考虑) |

---

## 1️⃣2️⃣ 自测题(确认真的懂了)

**Q1**:为什么 `mimeData()` 要返回 `new QMimeData` 而不是栈对象?
> 因为 Qt 在 drop 完成后会自动 `delete` 它,栈对象会被自动析构,容易双重释放。

**Q2**:`dragEnterEvent` 和 `dragMoveEvent` 哪个更重要?
> 都一样重要。dragEnter 决定能不能接收,dragMove 决定接收过程中持续确认(比如鼠标移到不接受区域就要 ignore)。

**Q3**:为什么 dropEvent 里要用 `mapToScene` 而不是直接 `event->position()`?
> `event->position()` 是屏幕坐标(像素),画布节点要存在 scene 坐标(世界坐标),中间要考虑滚动条和缩放。`mapToScene()` 一键搞定。

**Q4**:`setDragEnabled(true)` 加在源端,那目标端要加什么?
> `setAcceptDrops(true)`。源端开"拖出"开关,目标端开"接收"开关。

**Q5**:`acceptProposedAction()` 漏掉会怎样?
> Qt 会以为你不想 drop,可能触发默认行为(通常是拒绝)。拖拽看起来"没反应"。

---

## 1️⃣3️⃣ 一句话总结(贴在墙上)

> **拖拽 = 源端 override `mimeData()` 装料 + Qt setDragEnabled/AcceptDrops 双开闸 + 目标端 override 4 个 event 验签卸料 + mapToScene 坐标转换。Qt 事件循环负责把整个流程串起来。**

---

## 📚 相关代码位置(W7 Mini VM)

| 文件 | 关键改动 |
|---|---|
| `W7_VisionMasterLite/mainwindow.h` | `OpTreeWidget` 内部类(override mimeData) |
| `W7_VisionMasterLite/mainwindow.cpp` | `setDragEnabled/DragDropMode/SelectionMode` + 注册算子表 |
| `W7_VisionMasterLite/pipeline_view.h` | `setOperatorRegistry` + 4 个 event override 声明 |
| `W7_VisionMasterLite/pipeline_view.cpp` | `dragEnter/Move/Leave/Drop` 完整实现 + MIME 常量 |

---

## ⏭️ 下一步可做的事

| 任务 | 难度 | 价值 |
|---|---|---|
| **S2: 节点间真实连线** | 中 | 让画布从"散点"变"流程" |
| **S3: 参数面板动态切换** | 中 | 选中节点能看到算子参数 |
| **S5: 项目文件 `.sol.json` 保存/加载** | 中 | 把画布状态持久化 |
| **L1: 算子插件化(DLL)** | 大 | 学习 Qt 动态库加载 |
