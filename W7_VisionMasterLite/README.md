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
├── tests/                      # 8 个无头回归测试(335 断言)
├── images/                     # 测试图(复用 W6)
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

### 🔌 pin 类型检查
每个算子声明自己"吃什么、吐什么",连错时立刻弹窗说明原因:

| pin 类型 | 中文 | 谁在用 |
|---|---|---|
| `Image` | 图像 | 图像源、颜色分割(吐 mask) |
| `Data` | 数据 | Blob 计数(吐 count)、模板匹配(吐坐标)、PCB 元件定位(吐元件清单)、DL 推理(吐检测框) |
| `Text` | 文本 | 数量判定 / QR 解码 / PCB 缺陷判定(吐 PASS/FAIL、文本、缺陷清单) |
| `Any` | 任意 | 新算子没写声明时的默认值 = 不检查 |

拦掉的情况:自环、往图像源上连(图像源没有输入口)、类型不符。

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
| `qr_decode` | 二维码识别 | `QRCodeDetector` | 图像 → 文本 | 解 QR 内容 + 4 角点 |
| `pcb_inspect` | — | `findContours`/`moments` | 图像 → 数据 | PCB 元件定位(真值坐标 / 自动轮廓) |
| `pcb_defect_judge` | — | `inRange`/`morphologyEx` | 数据 → 文本 | 7 类缺陷判定,9 个阈值可调 |
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
- ✅ 右键菜单:查看该步输出 / 删除节点
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

### 持久化
- ✅ Save / Load JSON → `.sol.json`(节点 + 连线 + 参数 + UUID + 图像路径)
- ✅ **节点 UUID**:`.sol.json` 按 UUID 引用边,同名节点也能精确指认
- ✅ 旧格式兼容:无 `uuid` 字段的老文件退回"op 名 + FIFO"匹配
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

## 🧪 回归测试(无头,335 断言 / 8 个套件)

```powershell
cd E:\qianwie\00study\OpenCV\W7_VisionMasterLite
# 一次跑完全部 8 个套件
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build --output-on-failure'
```

| 套件 | 断言 | 测什么 | 需要 |
|---|---|---|---|
| `undo_redo` | 72 | 增删改节点/连线的往返、参数保留、同名节点 UUID 精确指认、节点消失后命令静默 no-op、**连线 payload 编解码** | Qt(不链 OpenCV) |
| `pcb_operators` | 32 | **W6 那 8 张样本图**:baseline 零缺陷,7 张缺陷图各自命中自己那类 | OpenCV |
| `dl_postprocess` | 49 | letterbox 补边/缩放、NMS 阈值、YOLOv5-v8 布局解析、退化输入不越界 | OpenCV |
| `image_sources` | 20 | 帧循环契约(按 frame_index 真的换帧/换张)、坏输入不崩 | OpenCV |
| `step_result` | 35 | 状态点判定边界、每步缓存用 UUID 做 key、**boundingRect 圈住端口圆** | Qt |
| `pin_and_recent` | 44 | 该拦的拦/不该拦的放行、路径归一化、已删条目剔除 | QtCore |
| `line_fit` | 24 | 长度/角度算术、**角度 0~180 的约定**、两点重合不报错、没图照样能算 | OpenCV |
| `measurement` | 59 | 圆度阈值真在滤方块、周长 0 不除零、**量的是垂直距离不是直线距离**、A/B 重合退化、**夹角退化为 0 而不是假角度** | OpenCV |

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

> 这几条断言都做过**变异测试**:把 bug 版代码临时放回去,分别挂 11 条、7 条、2 条,
> 确认它们真的抓得住 —— 只会跟着 bug 一起变的测试等于没写。
> 另外还反向验证了一次:把 `mouseReleaseEvent` 整个短路,对照组断言
> ("正常拖动节点仍要产生 nodeMoved")立刻变红,确保修复没有过头伤到移动功能。

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
# 期望末行:SUMMARY: pass=72 fail=0   RESULT=PASS(退出码 0)
```

> `pcb_operators` / `image_sources` 会在 `images/` 下写标注图和临时素材,
> 工作目录必须是工程根(CTEST 里已配 `WORKING_DIRECTORY`)。
> 临时素材的 gitignore 规则在根 `.gitignore`,**必须是 `**/images/…`** ——
> gitignore 里带 `/` 的模式是锚定到 `.gitignore` 所在目录的,
> 写成 `images/w7_*.png` 管不到 `W7_VisionMasterLite/images/`。

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
- **PCB 判定沿用 W6 的样本数据**:`short.png` 会同时报出 `short` 和 `wrong`
  两种缺陷。这不是搬算子时搬出来的 bug,W6 当时就是这样,原因在样本:
  生成脚本里 `GOLD = (30,180,220)` BGR 也就是**青色**,而 `judgeWrong` 的
  BGR 范围 `(150,150,0)-(255,255,80)` 青色也满足。已用特征测试钉住,
  免得它无声漂移。换真实产线数据时要重新标定。
- **复杂子板 / 不同尺寸布局**未适配,PCB 判定用的是固定 ROI。
- **只在 Windows + MSVC + Release 验证过**。Linux/macOS 移植没做:
  `CMakeLists.txt` 里 Qt/OpenCV 路径是硬编码的,测试文件里有
  `windows.h` / `SetConsoleOutputCP` 这类 Windows 专有调用。

## ✏️ 课后练习

1. **加新算子**:照着 `color_segment.h/.cpp` 加一个 `circle_find`(用 `cv::HoughCircles`),
   记得声明 `inputPinType` / `outputPinType`,并注册到三处:
   `operator_factory.cpp`、`mainwindow.cpp::registerOperators()`、`CMakeLists.txt`
2. **改 `short.png` 的青色串扰**:重新标定 `judgeWrong` 的 BGR 范围,
   然后更新 `test_pcb_operators.cpp` 里那条特征测试
3. **多前驱合并策略**:现在多个前驱的 output 是"后写覆盖"简单合并,
   改成按字段优先级或让用户在连线上标顺序
4. **把帧循环的结果也缓存下来**:现在只留最后一帧,想看第 7 帧得重跑
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
