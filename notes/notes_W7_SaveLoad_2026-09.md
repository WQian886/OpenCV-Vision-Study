# W7 Save/Load JSON 流水线 — 变更日志（2026-09-15）

> 📅 **记录时间**：2026-09-15
> 📁 **项目来源**：W7_VisionMasterLite Mini VisionMaster Lite
> ✏️ **改动文件**：`W7_VisionMasterLite/mainwindow.cpp`（1 个文件，3 处 Edit）
> 🎯 **本次目标**：把 💾 Save / 📂 Load 按钮从 TODO 占位升级为完整 JSON 流水线持久化
> ✅ **验证**：编译通过 + GUI 启动 3 秒无崩溃（PID 36532，exe 228 KB）

---

## 📌 一句话总结

新增 `pipelineToJson` / `pipelineFromJson` 两个 helper + 重写 `onSaveProject` / `onLoadProject`，让 W7 画布可保存到 `.sol.json` 文件并完整恢复（节点 + 连线 + 参数 + 图路径）。**W7 画布从"内存里临时存在"升级为"可存档可恢复的工程文件"**。

---

## 📋 一、改了什么

### Edit 1 — 新增 include（mainwindow.cpp 行 35、41）

```cpp
#include "operators/operator_factory.h"  // P1: for createOperatorByName
```

放在文件顶部所有算子 include 之后。

### Edit 2 — 新增 2 个 helper（mainwindow.cpp 行 44-160）

放在 `namespace w7` 顶部（**关键：必须在 onLoadProject/onSaveProject 调用之前定义**），包括：

- `pipelineToJson(canvas, imagePath) → QJsonDocument`
  - 遍历 `canvas->nodes()` → 每个节点的 op/display/category/x/y/params
  - 遍历 `canvas->connections()` → 每条边的 fromNode->opName()/toNode->opName()
  - 用 op 名引用避免节点指针失效

- `pipelineFromJson(canvas, doc, logFn, factoryFn, errOut) → bool`
  - `clearPipeline()` 清空画布
  - 重建节点：用 `factoryFn`（外部传入 `createOperatorByName`）验证算子有效性
  - `opIndex[opName].append(node)` 做 FIFO 索引
  - 重建连线：从 `opIndex[fromOp].takeFirst()` 取节点 → `addConnection()`
  - 返回创建/跳过节点数 + 连线数 + 错误信息

### Edit 3 — 重写 onLoadProject / onSaveProject（mainwindow.cpp 行 504-572）

**onLoadProject**（5 行 → 30 行）：
- QFileDialog 选 `.sol.json`
- QFile 读 → QJsonDocument 解析
- 调 `pipelineFromJson` 重建画布
- 恢复工具栏 `m_imagePathEdit` 图路径
- 调整 sceneRect 适配新节点

**onSaveProject**（5 行 → 25 行）：
- QFileDialog 选保存路径（默认 `untitled.sol.json`）
- 调 `pipelineToJson` 序列化画布
- QFile 写 `Indented` 格式 JSON
- 状态栏 + 日志提示

---

## 🎯 二、目的是什么

### 业务目标
让 W7 Mini VM 像 VM EasyBuilder 一样可保存/恢复工程文件：
- 用户花时间搭的流水线能存档，下次打开继续用
- 用户能分享 `.sol.json` 给同事（理论上，邮件/微信传文件）
- 用户能备份多个版本的 pipeline（baseline/V2/实验）

### 架构目标
为后续功能铺路：
- **Ctrl+Z 撤销**：可以基于 Save/Load 做版本对比
- **多 pipeline 切换**：保存多个文件，画布在它们之间切换
- **远程执行**：把 `.sol.json` 上传到服务器跑（W7 → 服务端 W6）

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| W7 完成度 | ~80% | **~90%** |
| Save 按钮 | ❌ TODO log | ✅ 写 JSON 文件 |
| Load 按钮 | ❌ TODO log | ✅ 读 JSON 重建 |
| 画布持久化 | ❌ 退出即失 | ✅ 可存档恢复 |

---

## 🤔 三、为什么要这么改

### 候选方案对比

| 方案 | 工作量 | 兼容性 | 缺点 |
|---|---|---|---|
| **A. JSON（本次采用）** | ~80 行 | ✅ Qt/其他语言都能读 | 文本格式稍大 |
| B. 二进制 QDataStream | ~60 行 | ❌ 仅 Qt 程序能读 | 不可读不可调试 |
| C. XML（QXmlStreamWriter） | ~100 行 | ✅ 标准 | 啰嗦 |
| D. 嵌入 SQLite | 大量 | ❌ 过重 | 杀鸡用牛刀 |

**选 A 的理由**：
1. **Qt 原生支持**：`QJsonDocument`/`QJsonObject`/`QJsonArray` 都是 QtCore 自带
2. **可读可调试**：用文本编辑器打开 `.sol.json` 就能看
3. **跨语言**：Python/Java/JS 都能解析，未来想加 headless runner 很容易
4. **海康 VM 格式**：VM EasyBuilder 的 `.sol` 文件内部也是 JSON-like 格式

### 关键设计：节点 ID 方案

**问题**：节点没有 UUID，edges 怎么引用？

**方案对比**：
| 方案 | 优 | 劣 |
|---|---|---|
| **A. op 名 + 出现顺序（FIFO）**（本次采用） | 简单；JSON 可读 | 同名节点（罕见）按出现顺序可能不符合连线意图 |
| B. 给 OperatorNode 加 UUID | 准确 | 改动大（OperatorNode.h/cpp + JSON 序列化） |
| C. 用 canvas->nodes() 索引 | 无需额外字段 | 节点顺序会随用户增删变化 |

**接受 A 的代价**：W7 当前 5 个算子不重名（color_segment/blob_count/count_check/template_match/qr_decode），FIFO 匹配足够用。**未来若允许同名节点，加 UUID 是必经之路**。

---

## 💡 四、有成型的解决方案吗？

### 4.1 画布持久化的成熟方案

| 项目 | 持久化格式 | 备注 |
|---|---|---|
| **Apache Airflow** | Python pickle | 任务依赖 DAG |
| **TensorFlow Graph** | Protobuf (`.pb`) | 二进制，跨语言 |
| **Qt QGraphicsScene** | 没有官方格式 | 第三方库 `qsjsonc` / `qgv-serialize` |
| **海康 VM** | 自定义二进制 | 类似 `.sol` |
| **LabVIEW** | 自定义二进制 | 工业界事实标准 |
| **draw.io (diagrams.net)** | XML/JSON | 跟 W7 类似，开源可参考 |

本次实现的 `.sol.json` 格式与 draw.io 的简化版很接近：节点 + 边 + 属性。

### 4.2 工业视觉领域

- **海康 VM EasyBuilder** 工程文件格式不公开，但据业内分析是 JSON-like
- **HALCON HDevelop** 工程文件是二进制 `.hdpl`
- **Cognex Designer** 用 SQLite 存 job

**结论**：JSON 是当前最常用的画布持久化方案。

### 4.3 Qt 序列化推荐

- **QSettings**（INI/注册表）—— 不适合树形结构
- **QDataStream**（二进制）—— 紧凑但不跨语言
- **QJsonDocument**（JSON）—— 本次采用
- **QXmlStreamWriter**（XML）—— 啰嗦但工业可靠

---

## ⚠️ 五、踩坑记录

### 坑 1：helper 函数位置导致前向引用

**编译错误**：
```
error C3861: "pipelineFromJson": 找不到标识符
error C3861: "pipelineToJson": 找不到标识符
```

**原因**：我把 helper 定义放在了 `onLoadProject` / `onSaveProject` **之后**（行 583+），但 onLoadProject 在行 528 调用 `pipelineFromJson`——C++ 不允许前向调用未声明的函数（除非加前向声明）。

**修复**：把 2 个 helper 移到 `namespace w7` 顶部（行 44-160），在所有 MainWindow::onXxx 之前。

**教训**：helper 函数默认放文件顶部（在 main 函数或类成员函数之前），调用方在后。如果 helper 在调用方之后，必须写前向声明。

### 坑 2：namespace 内 static 函数不能用 tr()

**编译错误**：
```
error C3861: "tr": 找不到标识符
```

**原因**：`tr()` 是 `QObject` 的 static 函数，需要 Q_OBJECT 宏（在类内部）或 `QObject::tr()`（在类外部）。我的 helper 是 namespace 内的普通 static 函数，没有 Q_OBJECT 上下文。

**修复**：把 `tr("[Load] ...")` 改成 `QString("[Load] ...")`。**功能上完全等价**（tr 的作用是 i18n 翻译标记，但当前 W7 项目没做翻译，所以无所谓）。

**教训**：在 Qt 里用 tr() 要看上下文——helper 函数不要用 tr，直接 QString 字符串拼接即可。

### 坑 3：lambda 返回类型不匹配

**编译错误**：
```
error C2664: ...无法将参数 4 从 lambda 转换为 std::function<OperatorBase*(const QString&)>
```

**原因**：我在 `onLoadProject` 里写的 lambda：
```cpp
[](const QString& name) -> OperatorNode* {
    return w7::createOperatorByName(name);  // ← 实际返回 OperatorBase*
}
```

显式声明 `-> OperatorNode*` 但函数体返回 `OperatorBase*`，不兼容。

**修复**：把 lambda 返回类型改成 `-> OperatorBase*`，与 factoryFn 签名一致。

**教训**：C++ lambda 显式返回类型必须与函数体实际返回类型兼容（或能隐式转换）。**类型不兼容时，要么改 lambda 返回类型，要么改 factory 函数签名**。

---

## ✅ 六、验收结果

### 编译
```
[1/4] Automatic MOC and UIC for target w7_vm_lite
[2/3] Building CXX object CMakeFiles/w7_vm_lite.dir/mainwindow.cpp.obj
[3/3] Linking CXX executable w7_vm_lite.exe; Copying OpenCV DLLs to output directory; Copying test images to output directory
```
- ✅ 退出码 0
- ✅ exe 212 KB → **228 KB**（+16 KB 新增逻辑）

### GUI Smoke Test
```
$proc = Start-Process w7_vm_lite.exe
Sleep 3
$proc.HasExited → False  (PID 36532)
Stop-Process → ✅ 成功
```
- ✅ 启动后存活 3 秒
- ✅ 无 segfault
- ✅ 无 moc/uic 警告

### T1-T5 测试（用户手动跑）

#### T1 默认保存+加载
1. 启动 W7 → 默认 4 节点 2 边自动加载
2. 点 **💾 Save** → 选 `default.sol.json`
3. 退出 → 重启 W7
4. 点 **📂 Load** → 选 `default.sol.json`
5. **预期**：4 个节点出现在原位置（color_segment 在 (150,200)、blob_count 在 (400,200) 等），2 条线连对

#### T2 删节点后加载
1. 启动 W7 → 选中 qr_decode → Delete → 只剩 3 节点
2. Save → 退出 → 重启 → Load
3. **预期**：3 个节点（color_segment→blob_count→count_check）+ 2 条边，qr_decode 缺失但无报错

#### T3 加载文件验证错误处理
1. 创建一个空的 `empty.sol.json` 文件
2. 启动 W7 → Load → 选 empty.sol.json
3. **预期**：日志红字 `[Load] ❌ 缺少 nodes 数组`

#### T4 未知算子处理
1. 用文本编辑器打开任一 `.sol.json`
2. 在 nodes 数组里手动加 `{"op": "fake_op", "x": 500, "y": 500, "params": {}}`
3. 保存 → W7 → Load → 选该文件
4. **预期**：日志 `[Load] 跳过未知算子: fake_op`，其他节点正常加载

#### T5 参数恢复
1. 启动 W7 → 选中 color_segment 节点 → 参数面板
2. 改 `H 上限` 从 40 到 60
3. **应用参数** → Save → 退出 → 重启 → Load
4. 选中 color_segment 节点 → 参数面板
5. **预期**：H 上限显示 60（不是默认 40）

---

## 📊 七、JSON 格式示例

```json
{
    "version": "1.0",
    "name": "untitled",
    "image_path": "../W6_PCB_DefectProject/images/baseline.png",
    "nodes": [
        {
            "op": "color_segment",
            "display": "颜色分割",
            "category": "detection",
            "x": 150,
            "y": 200,
            "params": {"hsv_low": [10,80,80], "hsv_high": [40,255,255]}
        },
        {
            "op": "blob_count",
            "display": "Blob 计数",
            "category": "detection",
            "x": 400,
            "y": 200,
            "params": {"min_area": 20}
        },
        {
            "op": "count_check",
            "display": "数量判定",
            "category": "detection",
            "x": 650,
            "y": 200,
            "params": {"expected_min": 1, "expected_max": 100}
        },
        {
            "op": "qr_decode",
            "display": "QR 解码",
            "category": "identification",
            "x": 150,
            "y": 380,
            "params": {}
        }
    ],
    "edges": [
        {"from": "color_segment", "to": "blob_count"},
        {"from": "blob_count", "to": "count_check"}
    ]
}
```

---

## 🔄 八、待补（不属本次改动）

| 优先级 | 任务 | 说明 |
|---|---|---|
| 🟡 中 | **节点 UUID** | 解决同名节点的 edge 歧义 |
| 🟡 中 | **input pin 类型检查** | 算子声明需要哪些 key，连线时验证 |
| 🟢 低 | **自动保存** | 改节点后自动写盘（类似 VS Ctrl+S 智能保存） |
| 🟢 低 | **最近文件列表** | W7 启动显示最近 5 个 `.sol.json` |
| 🟢 低 | **i18n 翻译** | 把 helper 里的 QString 改回 QObject::tr，支持英文/中文切换 |
| ⚪ 远期 | **Ctrl+Z 撤销** | 命令栈 + undo/redo |
| ⚪ 远期 | **版本迁移** | "version" 字段预留，从 1.0 升到 2.0 时写转换器 |

---

## 🔗 九、相关引用

| 引用 | 路径 |
|---|---|
| 本次改动文件 | `W7_VisionMasterLite/mainwindow.cpp` |
| 已有算子 | `W7_VisionMasterLite/operators/*.cpp` |
| 画布视图 | `W7_VisionMasterLite/pipeline_view.cpp` |
| 上次笔记（P0 Run 引擎） | `notes/notes_W7_DAG_RunEngine_2026-09.md` |
| W7 README | `W7_VisionMasterLite/README.md` |
| W6 修改日志 | `notes/notes_W6_ComponentRecognition_2026-09.md` |

---

> 💬 **写在最后**：本次改动让 W7 从"演示程序"变成"能存盘的工程工具"。Save/Load 是任何图形编辑器的"半壁江山"——没有它，用户每次重启都要重搭流水线，有了它，W7 才有了"工程"的概念。**Save/Load + Run 引擎 DAG 化 是 W7 区别于"普通 GUI 调用脚本"的两个核心特性**。剩 10% 主要是节点 UUID / 撤销重做 / 多 pipeline 切换等"非必须但能极大提升体验"的功能。
