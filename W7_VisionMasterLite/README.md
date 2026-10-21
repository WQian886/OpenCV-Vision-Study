# W7: Mini VisionMaster Lite(学习版) — 最终版

> 📅 2026-09-29 收官。W7 是最终版,原先规划给 W8 的活(PCB 算法上流水线、
> DL 算子、第 4 种图像源、每步结果查看、pin 类型检查、最近工程)全部并入本阶段。

## 🎯 学习目标
1. 理解**工业视觉平台的 3 层架构**:算子 + 流水线 + GUI
2. 掌握 **Qt6 + QGraphicsView** 实现流程图编辑器(对标海康 VM EasyBuilder)
3. 学会用 **OpenCV C++** 把 W1–W6 算法封装成"算子"
4. 体验 **QDockWidget** 的多面板工作区(对标 VM 可拖拽布局)
5. **实战:跑一个 PCB 缺陷检测流水线**(W6 的算法挂进画布)

## 📁 工程结构
```
W7_VisionMasterLite/
├── CMakeLists.txt              # Qt6 + OpenCV + MSVC,Release-only
├── main.cpp                    # QApplication 入口
├── mainwindow.h / .cpp         # 主窗口(QMainWindow + 4 个 Dock + Run 引擎)
├── pipeline_view.h / .cpp      # 流程图画布(QGraphicsView 子类)
├── operator_node.h / .cpp      # 流程图节点(QGraphicsObject 子类)
├── connection_line.h / .cpp    # 贝塞尔连线
├── connection_payload.h        # 连线拖拽 payload 编解码(uuid|side,纯逻辑)
├── recent_files.h / .cpp       # 最近工程列表(QSettings)
├── step_scope.h                # 单步执行范围(纯逻辑,索引收发不用指针)
├── commands/                   # M6 撤销/重做
│   ├── command.h / .cpp        #   抽象基类
│   ├── node_commands.h / .cpp  #   增/删/移节点
│   └── edge_commands.h / .cpp  #   增/删连线
├── operators/                  # 算子库(14 个)
│   ├── operator_base.h         #   抽象基类
│   ├── pin_types.h / .cpp      #   pin 类型系统 + 连线检查
│   ├── operator_factory.h/.cpp #   名字 → 算子
│   ├── param_form.h / .cpp     #   参数表单基类
│   ├── image_source_dir        #   文件夹批量图像
│   ├── image_source_camera     #   工业相机(MVS SDK 条件编译)
│   ├── image_source_video      #   视频文件
│   ├── color_segment           #   HSV 颜色分割 → mask
│   ├── blob_count              #   连通域计数
│   ├── count_check             #   数量区间判定
│   ├── template_match          #   模板匹配
│   ├── qr_decode               #   QR 解码
│   ├── pcb_inspect             #   PCB 元件定位(W6)
│   ├── pcb_defect_judge        #   PCB 7 类缺陷判定(W6)
│   ├── line_fit                #   直线测量(长度 + 角度)
│   ├── circle_fit              #   圆形测量(圆心/半径/圆度)
│   ├── caliper_dist            #   卡尺距离(点到线)
│   ├── dl_postprocess          #   letterbox / NMS / YOLOv5-v8 解析
│   └── dl_infer                #   ONNX 推理(ONNX Runtime 条件编译)
├── tests/                      # 15 个无头回归测试(1382 断言)
├── (无 images/)                # 测试图**只有一份**,在 W6_PCB_DefectProject/images/
│                               #   (那边才是生成器所在地)。构建时暂存进
│                               #   build/test_work/<套件名>/images/。
│                               #   2026-10-20 之前这儿还摆着一份**副本**,
│                               #   结果它停在旧版图上,测试拿旧图判还判得"对"。
├── recipes/                    # 板型配方 JSON(由 W6 的生成器导出,**与 W6 共用一份**)
├── frame_record.h              # 帧接线:"记一笔" + 序号→帧号换算(可行为测试)
├── ../common/pcb_core/         # ← 判定核心(W6/W7 共用,无 Qt 依赖)
├── README.md
└── NOTES_W7.md
```

## 🛠️ 构建

### VS 里构建
1. VS 2022/2026 → **打开本地文件夹** → `E:\qianwie\00study\OpenCV\W7_VisionMasterLite`
2. CMake 自动配置 → 切到 **x64-Release** → F7

### 命令行构建
```powershell
cd E:\qianwie\00study\OpenCV\W7_VisionMasterLite
# cmake 不在 PATH 上,而且没有 vcvars64 的话 MSVC 的 INCLUDE 环境是空的,
# 每个文件都会报 fatal error C1083: 无法打开包括文件: "type_traits"。
# 所以必须包在 vcvars64.bat 后面。
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\cmake.exe" --build build'
build\w7_vm_lite.exe
```

> ⚠️ Qt 路径硬编码在 `CMakeLists.txt` 的 `CMAKE_PREFIX_PATH`,
> OpenCV 路径硬编码在 `OpenCV_DIR`。换机器要同步改。

### 可选 SDK(装了就用,没装也能编)
CMake 会真实探测并在 configure 时打印结果:

| SDK | 环境变量 | 默认探测路径 | 缺了会怎样 |
|---|---|---|---|
| 海康 MVS | `MVS_DIR` | `D:/MVS` | `image_source_camera` 编译成给出提示的空实现 |
| ONNX Runtime | `ONNXRUNTIME_DIR` | `D:/onnxruntime` | `dl_infer` 编译成给出提示的空实现 |

> 之前 `HAS_MVS_SDK` 从来没人在 CMakeLists 里定义过 ——
> 也就是说哪怕把 MVS SDK 装好、路径全配对,`image_source_camera` 也永远是 no-op。
> 探测逻辑是这次补上的,装了就自动生效,不用改一行代码。

## 🎬 使用方法

启动后界面分 4 个区域:
- **左侧**:算子库(按模块分组,14 个算子)
- **中央**:流程图画布(默认已有 4 个节点 + 连线)
- **右侧**:参数面板(选中节点查看/编辑)
- **底部**:结果图 + **该步输出字段** + 运行日志

### 跑默认流水线
1. 点 **▶ Run**
2. 画布上的节点会**依次闪绿**(执行中),跑完每个节点右上角留下**状态点**
3. 底部日志逐步输出;末尾给出最终 PASS/FAIL,多帧时额外给批量汇总
4. 底部显示标注图

### 🔍 每步结果查看(VM 核心体验)
跑完之后**右键任意节点 → 查看该步输出**,结果区会切到那一步:
- 标题条写清当前看的是哪一步(第几步 · 算子名;多帧时还标出第几帧)
- 图区显示这一步最能说明问题的图(mask / 模板框 / 标注图 / PASS-FAIL 大字)
- 字段框列出这一步**全部输出字段**(`类型 = 值`)

> 只有最后一帧的结果会被缓存:用户右键要回答的是"这条流水线现在为什么
> 这么判",而末帧就是流水线当前的产出。

### 🐢 单步运行到这里(调参回路)
**右键任意节点 → 单步运行到这里**,只跑到它为止(含它),后面的节点本次不跑。
日志会先声明范围:

```
[Run] 单步执行「颜色分割」:本次只跑到它为止,跳过后面的 3 个节点(整条链共 5 个)。
```

范围 = 拓扑序的一个前缀。详见"已完成功能 → 单步执行"一节。

### 🔌 pin 类型检查
每个算子声明自己"吃什么、吐什么",连错时立刻弹窗说明原因:

| pin 类型 | 中文 | 谁在用 |
|---|---|---|
| `Image` | 图像 | 图像源、颜色分割(吐 mask) |
| `Data` | 数据 | Blob 计数(吐 count)、模板匹配(吐坐标)、PCB 元件定位(吐元件清单)、DL 推理(吐检测框) |
| `Text` | 文本 | 数量判定 / QR 解码 / PCB 缺陷判定(吐 PASS/FAIL、文本、缺陷清单) |
| `Any` | 任意 | 新算子没写声明时的默认值 = 不检查 |

拦掉的情况:自环、往图像源上连(图像源没有输入口)、类型不符、
**同一输入口接第二条线**。

> **多前驱冲突为什么必须拦**
> Run 引擎合并多个前驱的输出时是「后写覆盖」,所以同一个下游接两条线时,
> **谁赢取决于画布里连线的遍历顺序,而不是「谁先连的」**。
> 实测(2026-10-15 一次性探针,照抄引擎的合并逻辑):
>
> | 连线遍历顺序 | 下游读到的 |
> |---|---|
> | `[A(红,10000px), B(蓝,5000px)]` | **B 的 blue / 5000** |
> | `[B(蓝,5000px), A(红,10000px)]` | **A 的 red / 10000** |
>
> 注意第一行:`A` 在前却被 `B` 覆盖了 —— 连「先连线的赢」都不成立。
> 而挪动一下节点的上下位置就可能翻转这个顺序。
>
> 最真实的场合就是「抓红 + 抓蓝」两条 `color_segment` 都接 `blob_count`。
> 两个上游状态点都是**绿的**,不报错、不打日志,只有下游那个数悄悄变了 ——
> 到产线上就是漏检或误判。
>
> 这里选择**直接拒绝**而不是做合并策略:合并意味着「下游读到什么取决于
> 用户看不见的某个设置」,而 Run 引擎目前无状态,没地方存这个选择。

> 最有价值的一条是「图像源 → 数量判定」:`count_check` 只读 `inputs["count"]`,
> 接图像源时 count 拿不到 → actual=0 → **静默判 FAIL**,而日志上看不出任何异常。
> 类型检查把它挡在搭流水线的时候,而不是产线上。

**检查只拦"用户主动连线"**(`MainWindow::doConnect`),不拦加载旧工程和撤销重做
(那些走 `addConnection`,属于恢复既有状态而不是做新决策)。

### 🕐 最近工程
工具栏 **最近** 下拉,记最近 5 个 `.sol.json`(存 QSettings,跨会话)。
菜单显示文件名、tooltip 给完整路径;弹出时自动剔掉已被删/挪走的条目。

## 📚 算子清单(14 个)

| 算子 | VM 对标 | OpenCV API | 吃 → 吐 | 用途 |
|---|---|---|---|---|
| `image_source_dir` | 文件夹图像 | `imread` | — → 图像 | 批量加载文件夹,Run 时逐张跑 |
| `image_source_video` | 视频图像 | `VideoCapture` | — → 图像 | 逐帧取图,Run 时按帧循环 |
| `image_source_camera` | 相机采集 | MVS SDK | — → 图像 | 海康 GigE 实时取帧 |
| `color_segment` | 颜色分割 | `inRange` | 图像 → 图像 | HSV 提取,输出二值 mask |
| `blob_count` | Blob 分析 | `connectedComponentsWithStats` | 图像 → 数据 | 连通域计数 + 面积过滤 |
| `count_check` | 数量判定 | 无(纯逻辑) | 数据 → 文本 | 区间判定 PASS/FAIL |
| `template_match` | 模板匹配 | `matchTemplate` | 图像 → 数据 | 找模板位置 + 得分 |
| `qr_decode` | 二维码识别 | `QRCodeDetector` | 图像 → 文本 | 解 QR 内容 + 4 角点（实现来自 `common/qr_ocr/`，W5/W6/W7 共用一份）|
| `pcb_inspect` | — | `findContours`/`moments` | 图像 → 数据 | PCB 元件定位(真值坐标 / 自动轮廓) |
| `pcb_defect_judge` | — | `inRange`/`morphologyEx` | 数据 → 文本 | 7 类缺陷判定,7 个阈值可调 |
| `line_fit` | 直线测量 | 无(纯几何) | 图像 → 数据 | 给两点,量长度 + 角度(0~180°) |
| `circle_fit` | 圆查找 | `findContours`/`minEnclosingCircle` | 图像 → 数据 | 二值图里找圆,量圆心/半径/圆度 |
| `caliper_dist` | 卡尺 | 无(纯几何) | 图像 → 数据 | AB 定基准,量 C 到 AB 的垂直距离 |
| `dl_infer` | — | ONNX Runtime | 图像 → 数据 | YOLOv5/v8 目标检测 |

### VM 四大模块的覆盖情况

| VM 模块 | 本项目算子 | 状态 |
|---|---|---|
| 定位 Positioning | `template_match` / `pcb_inspect` | ✅ |
| **测量 Measurement** | **`line_fit` / `circle_fit` / `caliper_dist`** | ✅ |
| 识别 Identification | `qr_decode` / `dl_infer` | ✅ |
| 检测 Detection | `color_segment` / `blob_count` / `count_check` / `pcb_defect_judge` | ✅ |

> 三个测量算子做了一个刻意的分工:
>
> | 算子 | 要不要图 | 为什么 |
> |---|---|---|
> | `line_fit` | 不要 | 两点定一线,纯几何 |
> | `caliper_dist` | 不要 | 三点算距离,纯几何 |
> | `circle_fit` | **要** | 得先"找到"圆在哪,所以得看图 |
>
> **"没有图也能算"是刻意保留的**(给空 `image_path` 不报错,只不出标注图),
> 好处是这些算子能被纯数学单测,不用准备素材。
>
> `caliper_dist` 做的是卡尺的**几何核心**,不是 VM 那个交互式卡尺工具 ——
> 真卡尺还要处理选边、亚像素拟合、"用户点歪了怎么办"。

### 加一个算子要走哪 5 步

这是本项目最该记住的一条实操经验:

```
1. operators/xxx.h + .cpp    写算子本体(实现 6 个纯虚函数)
2. operators/operator_factory.cpp   加一行 if
3. mainwindow.cpp                   m_operatorInfo.append({名字, 显示名, 分类, 描述})
4. CMakeLists.txt                   加 .cpp;测试还要加 target + add_test
5. tests/test_xxx.cpp               写测试
```

第 2、3、4 步配错**不报错**,只是"算子没出现在左侧列表里"——
卡住时先查这三步,再怀疑代码。

### W6 深度联动
`pcb_inspect` + `pcb_defect_judge` 把 W6 的 PCB 算法整个搬上了流水线:

```
文件夹图像 ──→ PCB 元件定位 ──→ PCB 缺陷判定 ──→ PASS/FAIL
              (吐元件清单)      (吐缺陷清单)
```

7 类缺陷:缺件 / 错件 / 极性反 / 焊桥 / 焊少 / 偏移 / 短路。
判定阈值全部暴露成参数(W6 里是写死的常量,这里可以拖滑块调)。

> 跑 W6 那 8 张样本图必须过:`baseline` 零缺陷,7 张缺陷图各自命中自己那类。
> 这条验收清单已经变成可重复执行的测试(见下面的 `pcb_operators`)。

### DL 算子的分层
推理和前后处理**刻意拆成两个文件**:

- `dl_postprocess` — letterbox / NMS / YOLOv5-v8 输出解析。纯数学,
  **不需要模型也不需要 ONNX Runtime**,可以完整单测。
- `dl_infer` — 需要 ONNX Runtime。没装时编译成给提示的空实现。

> 结论:推理跑不了不算缺口,前后处理没测才算。
> 布局搞反了不报错,只是全检测不出来 —— 静默失效才是重灾区。

## ✅ 已完成功能

### 画布交互
- ✅ 节点拖拽创建、端口拖拽连线、选中改色
- ✅ **pin 类型检查**(见上)
- ✅ 节点右上角**状态点**:绿=成功 / 红=判 FAIL / 橙=算子报错
- ✅ 右键菜单:查看该步输出 / 单步运行到这里 / 删除节点
- ✅ Delete / Backspace 删选中项
- ✅ 节点底部显示端口类型

### 流水线执行
- ✅ **DAG 拓扑排序**(Kahn 算法),按用户画的图执行,不是硬编码顺序
- ✅ **环检测**:检测到环路报错而不是死循环或栈溢出
- ✅ **帧循环**:源算子输出 `frame_count`,引擎按 `inputs["frame_index"]`
  逐轮调用,每轮重跑整条链(文件夹源 / 视频源共用这一个契约)
- ✅ **批量汇总**:多帧时统计 PASS/FAIL 帧数。抽检 30 帧里 1 帧有问题,
  末帧正常的话单看末帧就是漏检 —— 产线判废看的是整体
- ✅ **每步结果查看**(见上)
- ✅ **单步执行**(见下)

### 单步执行 —— 把"改参数 → 看结果"的回路缩短

右键任意节点 → **单步运行到这里**,只跑到它为止(含它),后面的节点本次不跑。

为什么这是本项目最该补的一个功能:调参数时你真正想回答的是
"**这一步**为什么给这个数",不是"整条链跑完为什么给这个数"。
以前只能改一个阈值 → Run → 等整条链跑完 → 翻日志找是哪一步出的问题。
8 步的流水线,这个回路长到会让人放弃调参。

**范围 = 拓扑序的一个前缀**,不需要另写一遍图算法:
拓扑序的定义就保证每个节点的全部上游都在它前面,所以"跑到 target 为止"
天然包含所有上游。

> ⚠️ 这条推演**本身没有代码在守**。所以 `step_scope.h` 里另有一个
> 自检函数 `stepScopeHasAllUpstream`,拿一条**已知正确**的拓扑序进去
> 必须成立,拿一条**顺序被打乱**的进去必须报 false。
> 万一哪天拓扑排序被改(比如为了"让判定算子先跑"而调入队顺序),
> 前缀就不再自动包含全部上游了 —— 而这个错误**不会崩、不报错**,
> 下游拿到空的 nodeOutputs,读到空值或上一帧残留,又是一次静默失效。

| 设计 | 做法 | 为什么 |
|---|---|---|
| 单步/完整 Run | **同一个** `runPipeline(OperatorNode* target)` | 另写一份单步版,必然出现"完整 Run 修了、单步没修"的分裂 |
| 接口用索引 | `stepScopePrefix` 收发 `QList<int>` 而不是 `OperatorNode*` | 引擎特意在帧循环前把裸指针换成 QPointer 防 UAF;纯函数碰不到指针,就绕不过这层防护 |
| 找不到 target | 报错并取消,**不静默跑全链** | 静默跑全链 = 单步这个功能看起来"能用",其实是假的 |
| 超出范围的节点 | 状态点清 `None`,日志明说跳过了几个 | 界面上留着一个上次的绿勾,会被当成"这一步是通过的" |

用户看到的范围声明在日志里,不是静默的:

```
[Run] 单步执行「颜色分割」:本次只跑到它为止,跳过后面的 3 个节点(整条链共 5 个)。
   跳过的节点状态点会清空,它们的结果不是本次的
```

### 持久化
- ✅ Save / Load JSON → `.sol.json`(节点 + 连线 + 参数 + UUID + 图像路径)
- ✅ **节点 UUID**:`.sol.json` 按 UUID 引用边,同名节点也能精确指认
- ✅ **连线两端的端口方向**:`from_side` / `to_side`,竖线存一次再打开还是竖的
- ✅ 旧格式兼容:无 `uuid` 字段的老文件退回"op 名 + FIFO"匹配;
  无 `from_side`/`to_side` 的 v1.0 文件退回"右出 → 左进"(日志里报一句)
- ✅ Load 新工程自动清空撤销栈(旧 UUID 在新画布里已无意义)
- ✅ 最近工程列表

### 撤销/重做 (M6)
- ✅ **Command 模式**:抽象基类 + 6 个子类
  (`AddNodeCmd` / `DeleteNodeCmd` / `MoveNodeCmd` /
   `AddEdgeCmd` / `RemoveEdgeCmd`)
- ✅ 双栈 `QStack<Command*>`,新操作自动清 redo 栈
- ✅ `Ctrl+Z` 撤销 / `Ctrl+Y` 或 `Ctrl+Shift+Z` 重做,工具栏按钮栈空时置灰
- ✅ **全操作可撤销**:拖出节点、连线、拖动节点、右键删、Delete 键删
- ✅ 重入防护:undo/redo 期间产生的事件不落进命令栈

> **设计要点**:Command 全部按 `QUuid` 定位节点而非裸指针。
> 撤销删除会"重建"节点(新地址),存旧指针则后续撤销会操作已释放内存。
> 节点**不能自己 `delete this`** —— `PipelineView::m_nodes` 仍持有该指针,
> 留下悬垂指针后 Run 引擎遍历会段错误。改为发信号,由 MainWindow
> 统一走 `PipelineView::removeNode`(它已实现正确的清理顺序)。

## 🧪 回归测试(无头,1382 断言 / 15 个套件)

```powershell
cd E:\qianwie\00study\OpenCV\W7_VisionMasterLite
# 一次跑完全部 15 个套件
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build -C Release --output-on-failure'
```

> ⚠️ **`-C Release` 不能省。** 生成器是 VS 多配置版，`CMAKE_CONFIGURATION_TYPES`
> 里 Debug 和 Release 都在，而 ctest 不带 `-C` 时**默认按 Debug 找 exe**。
> 只构建过 Release 的话，15 个套件会全部报 `Not Run`——
> 报错长这样：
>
> ```
> 0% tests passed, 15 tests failed out of 15
> 	  1 - undo_redo (Not Run)
> ```
>
> 很容易读成"代码全坏了"，其实一个测试都没跑。
> `add_test(NAME x COMMAND x)` 传的是**目标名**不是路径，
> 由 ctest 按配置去 `$<CONFIG>` 对应的目录里解析。

| 套件 | 断言 | 测什么 | 需要 |
|---|---|---|---|
| `pcb_core` | 279 | **共享判定核心自己的测试**:JSON 解析(拒绝前导零/截断/尾随逗号)、配方 6 类错误分支、**拿不到依据时拒绝判定而不是报 PASS**、端到端对照真值表 | OpenCV(**不依赖 Qt**) |
| `qr_ocr` | 36 | **QR/OCR 共享核心自己的测试**(W6/W7 共用同一份源码):解码失败不崩、模糊图明确报"没解出来"而不是给个空结果 | OpenCV(**不依赖 Qt**) |
| `undo_redo` | 151 | 增删改节点/连线的往返、参数保留、同名节点 UUID 精确指认、节点消失后命令静默 no-op、**连线 payload 编解码**、**批量删除的记账分类**、**撤销栈里不许有空命令**、**竖线存一次再打开还是竖的**、**老文件没有方向字段时退回默认** | Qt(不链 OpenCV) |
| `pcb_operators` | 231 | **W6 那 8 张样本图**:baseline 零缺陷,7 张缺陷图各自命中自己那类、**没有判定依据时拒绝判定而不是报 PASS** | OpenCV |
| `dl_postprocess` | 49 | letterbox 补边/缩放、NMS 阈值、YOLOv5-v8 布局解析、退化输入不越界 | OpenCV |
| `image_sources` | 20 | 帧循环契约(按 frame_index 真的换帧/换张)、坏输入不崩 | OpenCV |
| `step_result` | 37 | 状态点判定边界、每步缓存用 UUID 做 key、**boundingRect 圈住端口圆**、**拖线时节点不跟着跑**、**endConnectionDrag 之后移动锁还锁着**、**release 时把跑偏的节点拉回来** | Qt |
| `pin_and_recent` | 59 | 该拦的拦/不该拦的放行、**同一输入口接第二条线必须拦**(且提示要点名是哪两条 + 说清根因 + 给出路)、路径归一化、已删条目剔除 | QtCore |
| `line_fit` | 24 | 长度/角度算术、**角度 0~180 的约定**、两点重合不报错、没图照样能算 | OpenCV |
| `measurement` | 59 | 圆度阈值真在滤方块、周长 0 不除零、**量的是垂直距离不是直线距离**、A/B 重合退化、**夹角退化为 0 而不是假角度** | OpenCV |
| `run_policy` | 162 | **帧数封顶 / 抽帧限频 / 取消+禁重入**、日志行数封顶、封顶后重绘代价不暴涨、**单步范围 = 拓扑序前缀(含自己)**、**范围找不到 target 要报错不能静默跑全链**、**被打乱的顺序必须被自检揪出来**、**多帧缓存:全帧留数字 / 撞上限要报出来 / 图没了不许拿别的帧顶替**、**帧选择器序号↔真实帧号的映射(丢过帧之后也对得上)**、**帧接线:每帧都记一笔 / 序号换算成真实帧号** | Qt |
| `template_match` | 28 | **三种匹配方法的相似度方向统一**、模板比原图大、纯色模板、非法 method、阈值真的在控 | OpenCV |
| `detection_operators` | 42 | **HSV 数组残缺不越界**、**两个实例的 mask 不互相覆盖**、面积阈值用 `>=`、**阈值填反要说人话而不是静默全判废** | OpenCV |
| `operator_library` | 37 | **没有算子会从算子库里消失**、旧实现确实丢了 `dl_infer`、没见过的分类也现建分组、分组不重不乱序 | QtCore |
| `operator_knobs` | 168 | **旋钮审计**:每个旋钮都得对得上 —— 登记了却没被 `run()` 读(**死旋钮**)、被读了却没登记、默认值抄了多份(`operator_defaults.h` 是唯一一份)、**空参数跑出来的结果必须和声明的默认参数一样**、编译期停用的旋钮必须在**带原因的豁免清单**里登记(且清单过期会报红) | OpenCV + Qt |

### 默认流水线上的三个算子(为什么补这层测试)

前 9 个套件有个共同的盲区:它们测画布、测撤销、测几何、测图像源,
**恰好绕开了启动时你看到的那条默认流水线**
(`image_source_dir → color_segment → blob_count → count_check`)。
这条链上的三个算子一个都没测过,而它们有两个性质让 bug 特别难靠手点发现:

1. **参数从 JSON 来,形状不保证。** 工程文件手可编辑、也可能是旧版本存的。
   原来的 `run()` 直接 `lo[0].toInt()`,拿到短数组就是越界 ——
   实测直接 `0xC0000005` 崩掉。同一个类的表单里却写着 `if (lo.size() >= 3)`,
   说明作者知道数组可能短,只是漏了 `run()` 这一处。
2. **中间结果靠落盘文件名传递。** 文件名写死成
   `images/w7_color_segment_mask.png` 时,一条流水线放两个颜色分割
   (抓红 + 抓蓝,很常见)第二个就覆盖第一个,下游 `blob_count` 读到的是
   **第二个算子的 mask**。

第二条比崩溃严重得多,因为它**每一步都"成功"了**:不崩、不报错、画布上
状态点是绿的,只有数出来的数是错的。产线上这种错要到漏检/误检才发现。

修法是让 Run 引擎把 `inputs["node_uuid"]` 注入下去(见 `MainWindow::onRunClicked`),
文件名变成 `w7_seg_<uuid前8位>_f<帧号>.png`。

### 多帧缓存:数字全留,图不全留(图没了必须明说)

以前每步结果只有一份,而且只写最后一帧:

```cpp
if (frame == totalFrames - 1 || totalFrames == 1) {
    m_stepOutputs[n->uuid()] = out;
}
```

30 帧里第 7 帧判 FAIL,数字只在日志里;右键回看只能看到末帧 ——
**末帧恰好 PASS 时,"回看"这个动作反而掩盖了要查的那一帧。**

中间产物的图上一轮已经按帧分开命名了(`artifact_path.h`),这一层
(`step_frame_cache.h`)解决剩下两件事:数字变成"每节点 × 每帧"一份;
某一帧的图被覆盖时**明说图没了**。

**为什么数字全留、图不全留** —— 两个数都是实测的:

| | 2000 帧 × 8 节点 | 每条 |
|---|---|---|
| 数字(QJsonObject) | 119 MB(put 全程 438 ms) | 7.6 KB / 27 us |
| 图(PNG 落盘) | ≈480 MB | ≈27 KB/张 |

119 MB 换 480 MB,内存比磁盘便宜 —— 所以数字全留。
(这一节我第一版写的是"约 30~60 MB",拍脑袋估的,实测 119 MB,差 2~4 倍。
估的数字写进注释里,下一个照着它做决定的人就被带偏了。)

`kStepCacheMaxFrames` 现在是个**保险丝**:它等于 `kMaxRunFrames`,正常单次
Run 打不到;哪天帧数上限调高,内存不会无声地涨,而是明确丢最早的帧、
并且调用方**被要求把丢弃这件事打进日志**(返回被丢帧数而不是 `void`,
就是为了让调用方没法装作没看见)。

**"图没了"为什么必须判据可测** —— 最危险的不是图没了,是**假装图还在**:

- `renderOutputToResultPanel` 的 fallback 分支拿工具栏那张原图兜底。
  工具栏是用户手填的静态图,和任何一帧都没有关系 —— 拿它配第 7 帧的数字,
  屏幕上是一张"看起来很正常"的图。
- 缓存里存一个 `imageAvailable` 标志。文件在运行期间被删(用户清 `images/`、
  换工程、磁盘满)标志不会变,于是继续显示一张早就没了的图,或者反过来。

所以判据是**每次问的时候去看磁盘**,不是一个存下来的标志:

```cpp
FrameImageState imageState(const QString& nodeId, int frameIndex,
                           const ExistsFn& exists) const;   // 生产传 &w7::fileExists
```

`exists` 是注入进来的,单测传一个假的就能造出"文件已删"的世界,
而判据和事实之间没有缓存层。

#### 这层测试测不到什么(变异跑出来的,不是推测)

跑了 4 次变异,前 3 次被杀,**第 4 次活了** —— 而第 4 次恰好就是这个改动
本身要修的那个 bug:

| # | 改法 | 结果 |
|---|---|---|
| M4 | 把 `put()` 调用重新包回 `if (frame == totalFrames - 1 \|\| totalFrames == 1)`(改回改动前的样子) | **12/12 全绿,活了** |

原因很直白:`step_frame_cache.h` 的断言测的是**这个类的行为**,而
"主窗口每帧都调一次 `put`"是**接线**,它在 `mainwindow.cpp` 里 ——
而 `mainwindow.cpp` 根本不在任何测试目标里(它要真窗口 + 全部算子,
无头构造不了)。

现在的能力边界:

- ✓ 判据对不对、保留策略对不对、文案说不说 —— 测到了
- △ 主窗口有没有真的每帧都喂进来 —— 只由一条**源码级 lint** 兜着
  (`test_mainwindow_records_every_frame`)。它能杀掉 M4,但换个等价写法就
  看不见 —— **它是 lint 不是行为测试,别把它当成"测过了"**
- ✗ 主窗口真跑一遍之后缓存里有什么 —— 没测到,也测不了

写这一段是因为:不写下来,下一个人会以为"147 条断言全绿"就等于
"多帧缓存这层没问题"。它不是。它等于"这个类没问题,接线只被一条 lint 看着"。
这两句话的差别,就是出事故时的差别。

> 顺带记一个被这条 lint 逮到的自坑:它第一版搜 `totalFrames - 1`,
> 结果匹配到了**我自己写的、解释这个 bug 为什么被删掉的注释** ——
> 一条会对着自己的文档叫的守卫,迟早被人删掉。现在搜索前先
> `stripLineComments()`(而且这个剥离器要认字符串字面量,
> 否则 `D://path` 里的 `//` 会被当成注释起点)。

> 还有一条:`add_test(NAME run_policy COMMAND w7_run_test)` 当时**没给
> `WORKING_DIRECTORY`**,ctest 就在 `build/` 下跑,找不到 `mainwindow.cpp`;
> 而守卫写成"打不开就 return",于是它变成一条**恒真的空断言** ——
> 报绿,其实什么都没查。表现是直接跑 exe `pass=147 fail=0`,
> 走 ctest 却是 `fail=1`。两处都改了:CMakeLists 补上
> `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`,守卫改成 `check(false, ...)`
> 并在消息里点名 CMake 那一侧,让配置缺口当场暴露而不是静默。
>
> ⚠️ 2026-10-20 又改了一次:工作目录从工程根挪到了
> `build/test_work/run_policy/`(为了不让测试产物落进源码树),
> 于是"相对路径恰好能打开"这个前提**第二次**不成立了。
> 这次没再补一个 `WORKING_DIRECTORY` —— 守卫改成**自己往上找**
> `mainwindow.cpp`(`findMainWindowCpp()`,最多往上 8 层),
> 找不到就**报红**。
>
> 两次修法不一样,是因为问题变了:
> 第一次是"CWD 放错了",第二次是"**根本不该依赖 CWD**"。
> 一条守卫如果它的成立前提是"某个目录碰巧对",
> 那它迟早会因为目录挪动而失效 —— 而失效的方式是**变成恒真**。
> 现在它只依赖一件事:**源码文件存在**。

### 帧选择器:序号 ≠ 帧号,这是设计出来的

标题条上多了一排 `[◀] [spin] [▶] [跳到末帧]`,右键打开某一步之后就能翻帧。

**为什么放标题条而不是右键菜单**:缓存上限是 2000 帧/节点,菜单塞不下帧列表;
而且帧数只存在于 `StepFrameCache` 里,`OperatorNode` 根本不知道 —— 要做成菜单
子菜单就得在 Run 每帧给每个节点推一次帧数,是白付的耦合。

**为什么用 spin 而不是 slider**:翻帧是**定位**问题,不是"大致浏览"。
用户来找第 7 帧的 FAIL,心里已经有数了,从 1 按到 7 是白折磨。

**最容易写错的地方 —— spin 的序号不是帧号。** 撞过上限之后缓存里的帧号是
不连续的(丢的是最早的几帧),拿帧号直接当 spin 的值,spin 的第 1 个位置会
指向**已经被丢掉**的第 0 帧。所以:

```
spin 第 n 项  →  m_stepCache.framesOf(nodeId).at(n-1)  →  真实帧号
```

标题里显示的仍然是**真实帧号**。两者不一致只可能发生在真的丢过帧的时候,
而那时候缓存至少已经报过一次警。测试里**故意造出丢帧**(3 帧窗口塞 5 帧),
让这条例外路径每天都被走到。

**「图区变了才重画」这道闸,比想象里没用。** 第一版挑了几个字段拼字符串当
判据(imagePath + result + score),漏了 `description` —— 而 description 是画在
PASS/FAIL 大字图上的。更要命的是 `pcb_defect_judge` 的 `image_path` 是
**源算子的输入图**,同一节点 30 帧全是同一个路径,于是"两帧都 PASS、描述不同"
会撞在一起,第二帧就显示**第一帧**印在图上的字。改成比整个 `out` 就没这个坑:
字段再加也只有"漏字段"一种错法,而现在连漏都漏不掉。

而且这道闸**本来也省不到什么** —— 翻帧时每一次的 `out` 本来就不同,
一张 PNG 该读还是要读(毫秒级,30 帧 300 ms)。它拦住的只有真正的重复:
同一帧被重画两次(单步重跑后又点回来看同一帧、已在末帧还按「跳到末帧」)。
留着是因为那几次重绘是真的纯浪费,但**不要**拿它当"翻帧性能已经解决了"
的证据 —— 没解决,也不需要解决。

#### 帧选择器的 4 次变异

| # | 改法(故意的 bug) | 结果 | 说明 |
|---|---|---|---|
| M1 | `showFrameSelector` 写成 `>= 1` | **红 2** | 一帧时也显示选择器 |
| M2 | `evictOldest` 丢**最新**而不是最早 | **红 5** | 其中 3 条是新加的映射断言 |
| M3 | `framesOf` 返回降序 | **红 6** | 末帧取错、spin 指向错帧 |
| M4 | `showStepFrame(id, fs.at(pos-1))` → `showStepFrame(id, pos-1)` | **红 1** | 只被源码 lint 逮到 |

M4 和上一轮那个 M4 是同一类:`mainwindow.cpp` 不在任何测试目标里。
它平时**看不出任何区别**(没撞过上限时 `pos-1` 恰好就是帧号),
只有真的丢过帧之后才暴露成"点 [◀] 翻不动帧"。

### 单步执行的 5 次变异(以及一个把我骗了的假红)

单步执行是纯逻辑,所以适合一条一条地把实现改坏,看测试红不红。
全部 5 个变异都被杀掉,当时的基线是 `run_policy` 的 71 断言
(该套件后来又加了多帧缓存和帧选择器的断言,现在是 147):

| # | 改法(故意的 bug) | 结果 | 说明 |
|---|---|---|---|
| M1 | `stepScopePrefix` 返回整条序(单步静默变成跑全链) | **6 红** | 覆盖全部 4 组。剩下那个节点还在跑,却还报"已跑到目标" |
| M2 | 范围不含 target(停在它前面) | **6 红** | 最阴的一种:界面显示目标节点有结果,其实那是上一个节点的 |
| M3 | `stepScopeHasAllUpstream` 永远返回 true | **3 红** | 正好是 3 个负例,正例一个没挂 —— 说明自检真的在干活 |
| M4 | 范围为空时返回空字符串(不说"找不到节点") | **2 红** | |
| M5 | 跳过的节点数 `total-scope-1`(少报一个) | **2 红** | 见下 |

M5 单独拎出来,是因为它对应我自己笔记里踩过的坑:
> "我只改了 reason 的**文字**,测试照样绿 —— 说明断言只检查了'非空'。"

所以 `describeStepScope` 的断言不是"消息非空",而是
**消息里必须出现节点名、跳过数、总数这三个具体值**。
少报一个节点数就是红的 —— 如果只查非空,M5 会全绿。

> 🧨 **一个把我骗了的假红**(比假绿更值得记)
>
> 5 个变异跑完、代码改回去之后,`ctest` 仍然报 `run_policy` 失败,
> 而且失败信息**精确地就是 M5 的输出**(`跳过后面的 -1 个节点`)。
> 直跑那个 exe 却全绿。
>
> 原因不是代码没改回去,是 **`Copy-Item` 会保留源文件的时间戳**:
> 从备份复制回来的 `step_scope.h` 带着备份那一刻的旧时间,
> 比 `.obj` 还早,MSBuild 判定"不需要重编"直接跳过 ——
> 于是 ctest 跑的是**带 M5 的旧二进制**。
>
> 这是"验证不等于验证"里最隐蔽的一种:你以为在验证新代码,其实在验证上一轮。
> **对策:复制回来之后强制 touch 一下再 build。**
> 而且注意判据 —— 我是靠**失败信息的具体内容**对上了 M5 才定位到的;
> 如果只是看到"跑了 ctest,红了",我会理所当然地以为"哦变异没杀干净",
> 然后去改已经正确的代码。

- 变异验证:去掉数组校验挂 6 条、去 UUID 文件名挂 3 条、
  去掉 min>max 检查挂 2 条
- 第二条的第一版测试只验"两个节点路径不同",而两张图的 mask 内容恰好一样 ——
  覆盖了也测不出来。改成两张图分割出**不同像素数**(全红 10000 / 半蓝 5000),
  并加了一条"让下游 `blob_count` 真的沿 A 的路径读一次":变异后读回 5000 而非
  10000,这才是真实危害发生的那一层。测症状不测后果,等于给自己留后门。

### template_match:三种方法的"方向"不是一回事

下拉框里三种方法,原始值的方向并不统一:

| 方法 | 完美匹配时的值 | 方向 |
|---|---|---|
| `TM_CCOEFF_NORMED` | ≈ 1 | 越大越像 |
| `TM_CCORR_NORMED` | ≈ 1 | 越大越像 |
| `TM_SQDIFF_NORMED` | ≈ 0 | **越小越像** |

代码对第三种取 `-minVal`(完美时正好是 `-0`),再和 `[0,1]` 区间的
threshold(默认 0.9)比 —— `-0 >= 0.9` 恒为 false。于是**选 SQDIFF_NORMED
就整条流水线恒判 NG**,而画布状态点是绿的、日志一切正常。
产线上这就是"模板明明在图里,却一直判 NG"。

现在统一归一成 `1 - minVal`,threshold 对三种方法是同一个含义 ——
这也是"从同一个下拉框里选方法"的用户默认预期。

另外两道闸:

- **模板比原图大**。文档说 `cv::matchTemplate` 会抛 `cv::Exception`,
  但**实测不是**:`CV_Assert` 在 Release 下被 NDEBUG 关掉了,它安静地返回
  `score=0.040665`、位置 `(40,31)` —— 而原图只有 40×40,合法 x 上限是 10。
  它拿着越界坐标照常返回一个结果。所以这道闸防的不是崩溃,
  是"静默的越界结果"。**崩溃反而是好的失败方式,你会立刻知道。**
- **纯色模板 + CCOEFF**。我原本以为会得到 NaN、于是 pass 恒假 ——
  **实测也不是**:本机 OpenCV 4.x 返回的是 `score=1.000000`、`pass=true`,
  也就是**恒判合格**,换任何图都"匹配成功"。这个方向比恒判不合格危险得多:
  判不合格会有人去查参数,判合格没人查,板子再差也能流到下一道工序。
  而且 OpenCV 在退化输入上返回什么依版本而异,所以修法不能是
  "跑完检查一下返回值",必须**调用之前自己算一遍模板方差**。

三条都属于"跑完了、没报错、状态点是绿的、只有结论是错的"。

### 一个方法,分三次才写对

写这个算子的测试时,我自己的第一版边界检查把上界写成了
`TM_SQDIFF_NORMED`,结果 method=3/4/5 全被判非法 —— 11 条里挂了 11 条。

原因是 OpenCV 这六个枚举**按家族顺序**排,不是按相似度排:

```
0 TM_SQDIFF   1 TM_SQDIFF_NORMED   2 TM_CCORR
3 TM_CCORR_NORMED   4 TM_CCOEFF   5 TM_CCOEFF_NORMED
```

`TM_SQDIFF_NORMED` 是 **1**,不是 5。这是先撞红的测试第二次帮我
挡住了一个我以为已经想清楚的地方。
### 跑批卡死的回归测试

`run_policy` 测的是 2026-10-08 修的"点 Run 跑批容易卡死"。先说清楚
**测的不是渲染快不快** —— 实测画布逻辑 3 µs/步、渲染 1 ms/帧 @40 节点,
画布无辜。测的是三条"都不崩"的规则:

- **不崩**是这个项目最难抓的一类缺陷。帧数不封顶照样能跑完,只是要等;
  抽帧不限频照样出结果,只是界面卡。所以"跑一遍看有没有报错"永远
  发现不了,只能把规则拆成纯函数(`run_policy.h`)钉住。
- 六个变异验证(逐个把 bug 放回去)全部被抓:去封顶 3 条红、去限频 4 条、
  去重入防护 2 条、Idle 也接受取消 3 条、去日志封顶 4 条、`endRun` 不复位 2 条。
- `test_log_document_is_bounded` 特意调的是 `configureLogView()` ——
  和主窗口**同一个函数**。第一版测试自己 new 了个控件再设上限,那只证明
  了"封顶机制管用",没证明"主窗口调了它",把 mainwindow 里那行删掉照样全绿。
  测平行宇宙等于没测。

### 画布渲染 bug 的回归测试

残影和"连线接错节点"这两个 bug 都**不会崩、不打日志**,只是屏幕上脏一点、
连线接错一根 —— 纯编译和启动都验不出来,手点一遍还容易看成"渲染正常"。
所以给它们各钉了一条断言:

- `boundingRect` 必须包住 4 个端口圆(含白描边)。Qt 移动 item 时
  **只擦 boundingRect 内的像素**,超出范围的它根本不知道存在,于是每拖一次
  就在原地留一份 —— 这就是残影的成因。以前 `boundingRect` 只写了本体矩形,
  而端口圆画在本体**外面**
- 连线 payload 必须是 `uuid|side`,不能是 `算子名|side`。算子名不唯一,
  画布里放两个 `color_segment` 时两个 payload 完全一样,落点必然接错

### "连线时元件乱跑"——两个 bug 叠在一起

这个现象最费劲:payload 改成 UUID 之后**现象还在**,一度以为没修对。
实际上 payload 那个 bug 是真的、也确实修好了,只是**不是这一个**。

真凶在 `mousePressEvent` 里:端口拖拽走 `QDrag::exec()`,那是个
**嵌套事件循环** —— 它阻塞在那里,但 Qt 仍在往 scene 派发鼠标移动事件。
节点带 `ItemIsMovable` 标志,scene 一看"用户按着某个 item 在动",
就把鼠标位置写进去。于是**整个拖线过程里节点本体一直跟着鼠标跑**,
节点一边跑一边连线,`onNodesMoved()` 在嵌套循环里被反复调用,
画出来还是一条抖动的乱线。

两个 bug 挤在同一段代码、现象又都是"线接得不对",所以修好一个还剩一个。

修法分两层,缺一不可:

- 拖线期间关掉 `ItemIsMovable` —— 治"节点跟着跑"
- 松手时按标志位跳过,不生成 `MoveNodeCmd` —— 治"撤销一次冒出一次假移动"

第二层还有个坑:**标志位不能在 `endConnectionDrag()` 里就清掉**。
`drag->exec()` 返回后 scene 才补发 `mouseReleaseEvent`,那一刻位置
已经变了,正是这个标志在挡误判。提前清掉等于没设 —— 这个是写测试时
先撞红的。

#### 第二轮:还是偶发,窗口期比想象的长

上面那版修完,大多数时候不跑了 —— 但用户反馈"元件**还是有时候**会乱跑"。

"有时候"三个字就是线索:如果 `ItemIsMovable` 根本没生效,那应该是
**每次**都跑。既然只偶发,说明标志位生效了、但**有半扇门开晚了**。

把时序摊开看:

| 时刻 | 发生了什么 |
|---|---|
| t0 | `mousePressEvent` — 节点成为 scene 的 **mouse grabber** |
| t1 | `beginConnectionDrag()` — `ItemIsMovable = false` ← 窗口**打开** |
| t2 | `drag->exec()` 返回 |
| t3 | `endConnectionDrag()` — 旧代码在这里 `ItemIsMovable = true` ← 窗口**还没关** |
| t4 | `mouseReleaseEvent` 真正送达,grabber 被清掉 ← 窗口**才关** |

t3→t4 之间这个节点**同时**是"scene 正按着的那个 item"和"可移动的"。
于是 scene 又能写它的位置了。

为什么平时看不出来:正常松手时物理按钮**已经抬起**了,scene 想写也写不进去,
位置没变;按 **Esc 取消**、或者落点被拒(类型不匹配)时按钮还按着,
scene 就写进去了 —— 节点被拽飞。这跟用户"点错了就发现元件跑了"的
手感完全对得上。

修法:把 `ItemIsMovable` 的恢复**从 `endConnectionDrag()` 挪到 `mouseReleaseEvent()`**,
窗口彻底不存在。再加两层兜底:

- release 时比对拖线前的位置,真的被挪动了就 `setPos()` 拉回来,
  并打一条 `[NODE] !! connection drag moved node` 警告 —— 这条日志是
  "乱跑真的发生过"的证据,不能默默吃掉
- `mousePressEvent` 开头检查标志位,如果卡在关闭状态(说明某条路径
  吞掉了 release)就先修回来,否则节点再也拖不动

> 这几条断言都做过**变异测试**:把 bug 版代码临时放回去,分别挂 11 条、7 条、2 条,
> 确认它们真的抓得住 —— 只会跟着 bug 一起变的测试等于没写。
> 第二轮又加了 2 条,变异验证:
> - 把 `setFlag(ItemIsMovable, true)` **放回** `endConnectionDrag()`(也就是复现上一版的 bug)
>   → 1 条红
> - 把 release 里的位置兜底 `if (...)` 改成 `if (false && ...)` → 1 条红
>
> 另外还反向验证了一次:把 `mouseReleaseEvent` 整个短路,对照组断言
> ("正常拖动节点仍要产生 nodeMoved")立刻变红,确保修复没有过头伤到移动功能。

### 两处 use-after-free（"选中节点按 Delete 会崩"）

和上面那批 bug 完全相反：这两个**会崩**。崩反而是好事——你能立刻知道。

**UAF-1：`deleteSelectedItems()`**

```cpp
for (auto* node : nodesToDelete) doDeleteNode(node);   // ← 节点和它的线在这里就没了
for (auto* line : connsToDelete) {
    if (line->fromNode() == n || line->toNode() == n)  // ← 读已释放的对象
```

`PipelineView::removeNode` 最后两行是 `qDeleteAll(linesToDelete); delete node;`。
第一遍删完节点，第二遍遍历的 `line` 里有一部分**正是刚被释放的那些**。

**UAF-2：`onNodeDeleteRequested()`**

```cpp
doDeleteNode(node);
if (node) m_stepOutputs.remove(node->uuid());   // ← if 只挡 nullptr，挡不住"已释放"
```

修法都是同一个：**需要的信息在删除之前取出来**。UAF-2 取 UUID，UAF-1 把整个"算计划"
抽成纯函数 `planSelectDelete()`，返回 UUID 列表，执行阶段一个裸指针都不碰。

#### 先证明 bug 真的存在，再证明修好了

"这看起来像 use-after-free"不是证据。写了个一次性探针：删完之后用同尺寸对象
把那块堆内存占掉并填毒（模拟"被别人复用"，正是线上偶发崩溃的成因），然后按旧顺序走：

```
删完节点，ab 已释放。开始复用那块内存……
现在读 line->fromNode()……
  fromNode() = 0xdddddddddddddddd   attached = false
  <段错误>            退出码 -1073741819 (0xC0000005)
```

三件事同时得到证实：读到的是已释放内存 → `attached` 判定失效 → 立刻崩。
**探针验完就删**，不进仓库。

> 教训：Release 下堆内存不填毒，所以这个 bug 平时**不崩**——
> 被释放的连线对象里还留着旧指针值，读出来往往还是对的。
> 等那块内存被复用才崩，现场也就没了。"偶尔才崩"和"从来没崩过"在这里是一回事。

#### 顺带挖出来的第二个 bug：撤销栈里混进空命令

原来还会给"已经被连带删除的线"再记一笔 `RemoveEdgeCmd`。那条命令执行时端点
已经没了，是条**空命令**——用户按 Ctrl+Z 会**多按一次、什么都没发生**。

我一开始以为后果是"撤销时线会重建两次"，**猜错了**：多余的 `RemoveEdgeCmd`
在撤销栈里排在 `DeleteNodeCmd` 前面，回滚时端点还不存在，所以它是个 no-op，
不会产生重复连线。真正能被观测到的后果只有命令条数——所以补的是

```cpp
checkEq(h.undoDepth(), 3, "撤销栈恰好 3 条命令(多一条说明有空命令混进来)");
```

> 又一次：**注释和直觉会骗人，断言不会。**
> 如果我当时只写"撤销后连线数 == 2"，这条断言在 bug 版代码上照样是绿的，
> 修复就等于没被验证。

### 算子库那个不存在的 DL 分支

补一行 `else if (dl)` 就能让 `dl_infer` 出现在算子库里。**但那样是错的**——
它只压住这一次的症状，形态没变。

原来的建树代码是硬编码的一串判断：

```cpp
if      (op.category == "acquisition")    parent = acquisition;
else if (op.category == "positioning")    parent = positioning;
...                                        // 一共 5 个分支
if (parent) { 建树项 }
```

注册表里写了 `category = "dl"`。**6 个分类只认 5 个** → `parent` 停在
`nullptr` → `if (parent)` 不成立 → 算子**凭空消失**，左边算子库里根本看不到，
**连日志都不打一句**。

这个形态的特点是：**新增一个分类不会报错、不会崩**，只会让那一类算子无声无息
地不见了。用户看到的是"怎么没有 DL 推理"，不会想到是建树的 if 少了一个分支。

所以改成 [operator_library_groups.h](W7_VisionMasterLite/operator_library_groups.h)
里的做法：给定分类，要么落进已知分组，**要么现建一个分组**。无论传进来什么，
没有算子会不见；现建出来的 key 回报给调用方，日志里留一句。

> 值得测的不是"dl_infer 出现了"（那是自证），而是
> **"没有算子会消失"这个不变式对任何输入都成立** —— 包括传一个分组表里
> 根本没有的分类进去。变异验证：把兜底改回静默丢弃 → **8 条红**。

### 竖线存一次就躺下了(存盘只记了"谁连谁")

画布支持 4 个端口(左 / 右 / 上 / 下),所以你能从上一个节点的**下边**拉出去、
从下一个节点的**上边**接进来,画一条竖线。

问题出在存盘。v1.0 的边只有两个字段:

```json
"edges": [ { "from": "…uuid…", "to": "…uuid…" } ]
```

**只记了谁连谁,没记它是从哪个口拉出来的。** 加载时一律按默认的
`addConnection(from, to)` 重画 —— 而它的默认参数正是 `Right → Left`。

结果:**竖线存一次、再打开,就变成横线了。** 不报错、不打日志,节点还在、
连线关系还在,只有走向变了 —— 你只会觉得"这软件有点怪"。

修法很小:边里多写两个字段,读的时候校验范围。

```json
"edges": [ { "from": "…", "to": "…", "from_side": 3, "to_side": 2 } ]
```

**兼容性天然成立,不用升版本号也不用迁移**:老文件没有这两个字段,
读出来是 `Undefined`,走 fallback 退回 `Right/Left` —— 和 v1.0 行为完全一致。

#### 这里埋着一个很容易写错的坑

```cpp
QJsonValue v = obj.value("from_side");   // 字段不存在 → Undefined
v.toInt();                                 // → 0
```

而 `0` 恰好是 `Left` 的合法编码。**于是所有缺字段的边都会被误判成 Left 线** ——
v1.0 的默认是 `Right`,老文件会被悄悄改成 `Left → Left`,比原来的 bug 更隐蔽。

所以必须先 `isDouble()`:

```cpp
inline OperatorNode::PortSide parseSide(const QJsonValue& v, OperatorNode::PortSide fallback) {
    if (!v.isDouble()) return fallback;      // ← 这一行就是"缺字段"的判据
    ...
}
```

> 变异验证 M14:把 `isDouble()` 那行注释掉 → **5 条红**,其中一条正是
> "老文件的横线仍然是 右出→左进"。`M13`(存盘不写这两个字段)→ **7 条红**。

#### 顺手做的另一件事:把存盘搬出 mainwindow

`pipelineToJson` / `pipelineFromJson` 以前是 `mainwindow.cpp` 里的 `static` 函数,
无头测试编不动(`mainwindow.h` 拖着整个 QtWidgets)。但它们**本来就设计成可测的** ——
`logFn` 和 `factoryFn` 都是传进来的,不碰任何全局状态。搬出来只是让测试够得着。

搬的过程中改了一处签名。原来工厂返回 `OperatorBase*`:

```cpp
std::function<OperatorBase*(const QString&)> factoryFn;
```

读一个文件要先 `new` 一个算子、读两个字符串、立刻 `delete`。而
`operator_base.h` 里 `#include <opencv2/opencv.hpp>` —— 于是 `pipeline_serialize.cpp`
也跟着依赖 OpenCV,`w7_undo_test` 就得链接 OpenCV,**建一次测试目标慢一大截**。

改成返回一个 POD 就干净了:

```cpp
struct OpDefaults {
    bool    known = false;   // 认不认识这个算子名
    QString display;
    QString category;
};
```

存盘模块从此**不认识算子**,只认名字和标签。

> 现有 13 个套件里,`w7_undo_test` 是唯一承诺"零 OpenCV、1 秒跑完"的那个,
> 这个属性值得用一次签名改动保住。

### 三个测量算子的测试为什么长这样

它们的公式都短得不像话,但**最容易错的是约定,不是算术**:

- `line_fit` — 角度 0~180 还是 0~360?(测试钉住)
- `circle_fit` — 圆度 `4π·area/perim²` 漏个 4π 或忘记平方,
  算出来是 0.25,**滤不掉任何东西还"看起来在工作"**。
  所以测试专门验证"阈值真的在滤东西":同一个方块,
  阈值 0.9 被滤掉、0.1 被留下 —— 这才能证明公式没错
- `caliper_dist` — 测的是**垂直距离**不是到 A 的直线距离。
  A=(0,0) B=(10,0) C=(1,1):直线距离 √2,垂直距离 1。搞混这两个最常见

单独跑一个:
```powershell
$env:QT_QPA_PLATFORM="offscreen"; .\build\w7_undo_test.exe
# 期望末行:SUMMARY: pass=151 fail=0   RESULT=PASS(退出码 0)
```

> 🧹 **测试产物落在 `build/test_work/<套件名>/`,不落源码树。**
> `pcb_operators` / `image_sources` / `measurement` / `detection_operators` /
> `template_match` 会在 `images/` 下写标注图和临时素材 ——
> 2026-10-20 之前,它们的 CWD 是工程根,产物**直接写在源码树里**,
> 靠根 `.gitignore` 的 `**/images/w7_*.png` 之类规则**藏起来**。
>
> 现在每个套件一份工作目录(全在 `build/` 下),配方和测试图由构建时暂存进去:
>
> - 产物不再进源码树,`git status` 跑完测试是干净的
> - 暂存**每次构建都重做** —— 改了配方、换了图,下次构建就生效,不会拿旧数据跑
> - 每套件一份目录,`ctest -j` 并行也不会互相踩
>
> 相应地,根 `.gitignore` 里那两条**只为藏测试产物**的规则
> (`**/images/_test_src.avi`、`**/images/_srcframe_*.png`)已经删掉 ——
> 规则是用来藏问题的,不是用来解决问题的。应用运行时产生的
> `**/images/w7_*.png` 规则保留(那是应用行为,不是测试行为)。
> 详见 [../notes/notes_TestWorkDir_2026-10.md](../notes/notes_TestWorkDir_2026-10.md)。

> 🤖 **一条命令跑完"构建 + 全部测试 + 检查源码树没被写脏"**：
> ```powershell
> powershell -ExecutionPolicy Bypass -File ..\tools\ci.ps1
> ```
> 最后那一步（跑完测试源码树必须没变）以前是**没人记得做**的，
> 现在每次跑都查，泄漏会**逐个点名**。
> 详见 [../notes/notes_CI_2026-10.md](../notes/notes_CI_2026-10.md)。

### 测试抓到的真 bug(留着当反面教材)
- **`VideoCapture` 抛异常而不是返回 false**:塞一个非视频文件进去,
  image-sequence 后端走到 `CV_Assert(!filename_pattern.empty())` 直接抛
  `cv::Exception`。我第一次只在 `cap.open()` 外面包了 try,没接住 ——
  异常从别处冒出来。现在加了两道:扩展名预检(给人话)+ open/get/read 全包。
  产线上的算子不能因为用户选错文件就把整条流水线带崩。
- **`image_source_dir` 的注释在撒谎**:注释写着"每张图都让下游算子跑一次",
  代码里却写死 index 0,整个目录只会跑第一张。注释里的功能从没发生过。
  **注释会骗人,断言不会。**
- **日志和屏幕对不上**:结果区以前是"先画 mask,再无条件用大字图盖掉",
  而日志只在没画过时记一行 —— 于是日志写着 mask、屏幕上却是大字图。

## ⚠️ 已知限制

- **MVS SDK / ONNX Runtime 未装**:两个算子在当前机器上编译成空实现,
  点 Run 会给出明确提示。装好 SDK + 设环境变量后重新 configure 即自动启用。
- ~~**PCB 判定沿用 W6 的样本数据**:`short.png` 会同时报出 `short` 和 `wrong`~~
  —— **2026-10-20 已修**:短路图重画成"两脚被铜条连通"之后,铜条是金色不是青色,
  `short.png` 现在只报 `short`(点名 R2),不再串扰 `wrong`。
  根因还是那句老话:生成器画给人看的**标记**被当成了缺陷本身。
  详见 `notes/notes_PCB_Short_2026-10.md`。
- **短路目前只给双端子元件配了**:电阻/电容/二极管/电感。
  三脚以上的封装(TO-220 / QFP64)没有"两脚之间"的概念,配了会判不出。
  代价是这些元件的**两脚短路没有被检查** —— 这是"诚实的不检查",不是"假装检查过了"。
- **复杂子板 / 不同尺寸布局**未适配,PCB 判定用的是固定 ROI。
- **只在 Windows + MSVC + Release 验证过**。Linux/macOS 移植没做:
  `CMakeLists.txt` 里 Qt/OpenCV 路径是硬编码的,测试文件里有
  `windows.h` / `SetConsoleOutputCP` 这类 Windows 专有调用。

### ✅ 2026-10-20 关掉的一条债:mainwindow 的帧接线不再是 lint

以前"每帧都记"和"序号→帧号换算"这两条只由**源码 lint** 守着 ——
因为 `mainwindow.cpp` 不在任何测试目标里。实测代价:把"只记最后一帧"加回去,
**12/12 套件全绿**(当时只有 12 个套件),没有任何一条断言发现。

现在这两件事抽到了 `frame_record.h`(可行为测试),lint 的职责也换了:

| | 以前 | 现在 |
|---|---|---|
| 每帧都记 | lint 查"没有 `totalFrames - 1`" | **行为测试**:驱动 30 帧,断言 30 帧的数字全在 |
| 序号→帧号 | lint 查"没有 `showStepFrame(..., pos - 1)`" | **行为测试**:含"丢过帧之后序号≠帧号"这个关键场景 |
| 接线本身 | — | lint 改成查"**必须走** `recordFrameResult()` / `frameNumberForPosition()`" |

lint 从"禁止某种写法"变成"**要求某个可测入口**" —— 后者不会被等价写法绕过。

**变异验证**:把 `shouldCacheFrame()` 改回 `frame == totalFrames - 1`,
`run_policy` 套件立刻转红(`pass=161 fail=1`)。这条不变量现在**真的在验证**。

### 🔗 判定核心搬到 common/pcb_core

PCB 判定的实现(配方解析 + 定位 + 六条判据 + 短路)搬到了 `common/pcb_core/`,
**W6 命令行和 W7 算子共用同一份**。

理由:能力在 W7、数据在 W6,而 W6 自己用不上;硬把算法抄一份就是本项目
栽过四遍的「抄副本」。现在 W6 也能检那 13 张 complex 板了,两边结论逐字一致。


## ✏️ 课后练习

1. **加新算子**:照着 `color_segment.h/.cpp` 加一个 `circle_find`(用 `cv::HoughCircles`),
   记得声明 `inputPinType` / `outputPinType`,并注册到三处:
   `operator_factory.cpp`、`mainwindow.cpp::registerOperators()`、`CMakeLists.txt`
2. ~~**改 `short.png` 的青色串扰**~~ **已做**（2026-10-20,随短路判据重做一起解决):
   短路图重画成"两脚被铜条连通",铜条是金色不再串扰 `wrong` 判据。
   根因是生成器画给人看的**标记**被当成了缺陷本身,不是 BGR 范围没标定好 ——
   重新标定只是把症状按住,根还在。详见 `notes/notes_PCB_Short_2026-10.md`。
3. **多前驱合并策略**:现在多个前驱的 output 是"后写覆盖"简单合并,
   改成按字段优先级或让用户在连线上标顺序
4. ~~**把帧循环的结果也缓存下来**~~ **已做**(`step_frame_cache.h` + 帧选择器):
   数字全帧留、图只留最近 N 帧、图没了明说不顶替;标题条上的
   `[◀][spin][▶]` 可以在同一节点内翻帧,spin 可直接敲号。
   顺带一个已知的债:因为 `mainwindow.cpp` 没有任何测试目标,
   "主窗口每帧都喂进来"和"spin 的序号怎么换算成帧号"这两条**接线**
   只有源码级 lint 看着。
5. **子图 / 分支复用**:同一个子流水线被两条支路引用(需要拓扑排序支持)

## 🎓 回顾:这个项目真正教我的东西

不是 Qt 的 API 怎么写,而是这几类东西**不写测试就一定会错**:

1. **指针生命周期**。QGraphicsItem 的所有权、Command 跨 undo/redo 的定位、
   `nodeOutputs` 里的悬垂指针 —— 三处都出过真 bug,而且都是段错误级别。
2. **注释不是文档,是意图**。`image_source_dir` 那条"每张图都跑一次"的
   注释骗了好几个版本。断言才是。
3. **静默失效比崩溃危险**。状态点画错颜色、PCB 串扰、图像源取错类型 ——
   这些都不崩,只是结果悄悄错了,要到产线才发现。
4. **优雅降级要有真实的探测**。把 `HAS_MVS_SDK` 写死在"关"等于
   "永远不装也能编",于是永远没人发现它没生效。
