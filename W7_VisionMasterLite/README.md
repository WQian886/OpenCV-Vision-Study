# W7: Mini VisionMaster Lite(学习版)

## 🎯 学习目标
1. 理解**工业视觉平台的 3 层架构**:算子 + 流水线 + GUI
2. 掌握 **Qt6 + QGraphicsView** 实现流程图编辑器(对标海康 VM EasyBuilder)
3. 学会用 **OpenCV C++** 把 W1–W6 算法封装成"算子"
4. 体验 **QDockWidget** 的多面板工作区(对标 VM 可拖拽布局)
5. **实战:跑一个 PCB 缺陷检测 demo 流水线**

## 📁 工程结构
```
W7_VisionMasterLite/
├── CMakeLists.txt              # Qt6 + OpenCV + MSVC,Release-only
├── main.cpp                    # QApplication 入口
├── mainwindow.h / .cpp         # 主窗口(QMainWindow + 4 个 Dock)
├── pipeline_view.h / .cpp      # 流程图画布(QGraphicsView 子类)
├── operator_node.h / .cpp      # 流程图节点(QGraphicsItem 子类)
├── operators/                  # 算子库
│   ├── operator_base.h         # 抽象基类(统一接口)
│   ├── color_segment.h/.cpp    # HSV 颜色分割 → mask
│   ├── blob_count.h/.cpp       # 连通域计数
│   ├── count_check.h/.cpp      # 数量区间判定
│   ├── template_match.h/.cpp   # 模板匹配
│   └── qr_decode.h/.cpp        # QR 解码
├── images/                     # 测试图(可复用 W6)
├── README.md
└── NOTES_W7.md
```

## 🛠️ 构建(与 W5/W6 同构)
1. VS 2022/2026 → **打开本地文件夹** → `E:\qianwie\00study\OpenCV\W7_VisionMasterLite`
2. CMake 自动配置 → 切到 **x64-Release**
3. F7 编译 → Ctrl+F5 运行

> ⚠️ Qt 路径硬编码 `D:/Qt/6.8.3/msvc2022_64`,改机器请同步改 CMakeLists.txt 第 17 行

## 🎬 使用方法

启动后界面分 4 个区域:
- **左侧**:算子库(4 大模块,5 个算子)
- **中央**:流程图画布(默认已有 4 个节点 + 简单连线)
- **右侧**:参数面板(选中节点查看/编辑)
- **底部**:结果预览 + 运行日志

### 跑默认流水线
1. 工具栏的"图像"输入框已填好 `images/baseline.png`
2. 点 **▶ Run** 按钮
3. 底部日志会逐步输出每个算子的执行结果
4. 最终显示 PASS / FAIL

### 自定义流水线(MVP 限制)
- 节点可拖动(鼠标按住)
- 节点可选中(影响颜色)
- 增删节点 / 真实连线 **本 MVP 暂未实现**,留 TODO 在 MainWindow::onLoadProject

## 📚 5 个算子清单(对标 VM)

| 算子 | VM 对标 | OpenCV API | 用途 |
|---|---|---|---|
| `color_segment` | 颜色分割 | `cv::inRange` | HSV 颜色范围提取 |
| `blob_count` | Blob 分析 | `cv::connectedComponentsWithStats` | 连通域计数 |
| `count_check` | 数量判定 | 无(纯逻辑) | 区间判定 PASS/FAIL |
| `template_match` | 模板匹配 | `cv::matchTemplate` | 找模板位置 |
| `qr_decode` | 二维码识别 | `cv::QRCodeDetector` | 解 QR 内容 |

## ✅ 已完成功能(2026-09-15 更新)

### 算子库 (5 个算子,全部已实现)
- `color_segment`: HSV 颜色范围提取 + 形态学
- `blob_count`: 连通域计数 + 面积过滤
- `count_check`: 区间判定 PASS/FAIL
- `template_match`: 模板匹配 (3 种 method)
- `qr_decode`: QRCodeDetector 解码 + 4 个角点

### 画布交互
- ✅ **节点拖拽**: 从算子库拖到画布创建节点 (`mainwindow.cpp:128-130`)
- ✅ **节点连线**: 节点右侧输出端口拖到另一节点左侧输入端口 (`operator_node.cpp:80-92`)
- ✅ **参数面板动态切换**: 选中节点切换参数表单,支持 5 种算子的参数编辑 (`mainwindow.cpp:260-275`)
- ✅ **删除选中项**: Delete/Backspace 键 (`mainwindow.cpp:515-523`)
- ✅ **右键删除节点**: 节点上右键 → 删除 (`operator_node.cpp:contextMenuEvent`)

### 流水线执行
- ✅ **DAG 拓扑排序**: 按用户画的图执行,不是硬编码顺序 (`mainwindow.cpp:topoSort`)
- ✅ **环检测**: 检测到环路自动报错 (`onRunClicked`)
- ✅ **结果图显示**: 根据最后节点 output 智能显示 (mask / 模板红框 / PASS-FAIL 大字)

### 持久化
- ✅ **Save/Load JSON**: 工具栏 💾 Save / 📂 Load → `.sol.json` 流水线文件 (`mainwindow.cpp:onSaveProject/onLoadProject`)
- ✅ **参数恢复**: 加载后自动恢复每个节点的 params
- ✅ **节点 UUID**: 每个节点自带 `QUuid`,`.sol.json` 按 UUID 引用边 —— 同名节点也能精确指认（2026-09-29）
- ✅ **旧格式兼容**: 加载无 `uuid` 字段的老文件时,自动退回"op 名 + FIFO"匹配

### 撤销/重做 (M6)
- ✅ **Command 模式**: `commands/command.h` 抽象基类 + 6 个子类
  - `AddNodeCmd` / `DeleteNodeCmd` / `MoveNodeCmd`(`commands/node_commands.cpp`)
  - `AddEdgeCmd` / `RemoveEdgeCmd`(`commands/edge_commands.cpp`)
- ✅ **双栈**: `QStack<Command*> m_undoStack / m_redoStack`,新操作自动清 redo 栈
- ✅ **快捷键**: `Ctrl+Z` 撤销 / `Ctrl+Y` 或 `Ctrl+Shift+Z` 重做
- ✅ **工具栏按钮**: `↶ Undo` / `↷ Redo`,栈空时自动置灰
- ✅ **全操作可撤销**: 从算子库拖出节点、端口连线、拖动节点、右键删节点、Delete 键删选中项

> **设计要点**:Command 全部按 `QUuid` 定位节点而非裸指针。
> 撤销删除会"重建"节点(新地址),若存旧指针则后续撤销会操作已释放内存。
> UUID 由节点创建时生成、重建时沿用,命令永远能找到正确的节点。
> Load 新工程后会自动清空两个栈 —— 旧命令的 UUID 在新画布里已无意义。

### 🧪 回归测试(无头)
```powershell
# 构建(约 3 秒,不链接 OpenCV)
cmake --build build --target w7_undo_test
# 运行(headless,不需要显示器)
$env:QT_QPA_PLATFORM="offscreen"; build\w7_undo_test.exe
# 期望输出:SUMMARY: pass=50 fail=0   RESULT=PASS(退出码 0)
```

12 个用例覆盖增删改节点/连线的往返、参数保留、同名节点 UUID 精确指认、
节点消失后命令静默 no-op 等。**改完 Command 逻辑跑一次即可回归,不用手点鼠标。**

> 用例 5 会打印节点重建前后的地址,用来实证「地址确实会变」——
> 这正是 Command 必须存 UUID 而非裸指针的原因。

## ⚠️ 仍待补(下一轮)
- ❌ **input pin 类型检查**: 算子声明需要哪些 key,连线时验证
- ❌ **最近文件列表**: 启动显示最近 5 个 `.sol.json`
- ❌ **复杂子板支持**: 适配不同尺寸/布局的 PCB
- ❌ **真实 PCB 测试**: HSV 范围 + 颜色校准

## 🛠️ 命令行构建(备选)

## 🛠️ 命令行构建(备选)
```powershell
cd E:\qianwie\00study\OpenCV\W7_VisionMasterLite
# 用 vcvars64.bat 激活 MSVC 环境
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build'
build\w7_vm_lite.exe
```

## ✏️ 课后练习

### 基础(MVP 已实现)
1. **跑默认流水线**:点 ▶ Run,看 PASS/FAIL 输出
2. **拖动节点**:鼠标按住画布上的节点移动
3. **缩放画布**:Ctrl + 滚轮
4. **切换算子库选中项**:左侧选不同算子,看日志

### 进阶(下一轮要做)
5. **加新算子**:照着 `color_segment.h/.cpp` 加一个 `circle_find`(用 `cv::HoughCircles`)
6. **真实连线**:让节点拖到画布时自动创建 `QGraphicsLineItem`
7. **JSON 加载**:把画布节点序列化到 `.sol.json`,重启动后恢复
8. **参数面板**:选中节点时,根据 `opName` 动态生成 QFormLayout

## 🎓 下一节预告

W7+:完善 Mini VisionMaster
- 完整流程图编辑器(拖拽 / 连线 / 删除)
- DL 算子集成(Pytorch / ONNX Runtime)
- 与 W6 PCB 检测项目深度联动(把 W6 的算法包成算子挂到 VM 里)
- 跨平台(Linux / macOS)Qt6 移植
