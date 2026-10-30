# OpenCV 学习路线 (W1-W7)

> 📅 **初始化**：2026-09-15
> 🎯 **目标**：7 周系统学习 OpenCV,从图像处理基础到工业视觉应用
> 🛠️ **技术栈**：C++17 + OpenCV 4.13 + CMake + Qt 6.8.3 + MSVC

## 📚 学习周表

| 周 | 主题 | 进度 | 入口 |
|---|---|---|---|
| **W1** | 图像 I/O + 像素操作 | ✅ 100% | [README](./W1_ImageIO/README.md) |
| **W2** | 色彩空间 + 滤波 + 阈值 | ✅ 100% | [README](./W2_ColorFilter/README.md) |
| **W3** | 边缘 + 轮廓 + 形态学 | ✅ 100% | [README](./W3_EdgeContour/README.md) |
| **W4** | 模板匹配 + ORB 特征 | ✅ 100% | [README](./W4_TemplateFeature/README.md) |
| **W5** | OCR + 二维码 | ✅ 100% | [README](./W5_OCR_QRCode/README.md) |
| **W6** | PCB 缺陷检测综合项目 | ✅ 100% | [README](./W6_PCB_DefectProject/README.md) |
| **W7** | Mini VisionMaster Lite (Qt) | ✅ 最终版 | [README](./W7_VisionMasterLite/README.md) |

## 🧪 回归测试现状

2026-10-26 之前，**W1–W5 一条自动化测试都没有** ——
W7 之前只有 W6 有套件（因为 W6 是第一个「别人真的拿去用」的项目）。

**2026-10-26** 给 **W1 和 W2** 补上了（两个都是纯 OpenCV，不需要 Qt 窗口）：

- **W1 图像 I/O + 像素操作** — **1 个无头回归测试 / 29 条断言**（2026-10-26 之前是 **0 个**）
- **W2 色彩空间 + 滤波 + 阈值** — **1 个无头回归测试 / 54 条断言**（2026-10-26 之前是 **0 个**）

**2026-10-28** 给 **W3** 补上了（同样是纯 OpenCV）：

- **W3 边缘 + 轮廓 + 形态学** — **1 个无头回归测试 / 52 条断言**（2026-10-28 之前是 **0 个**）

**2026-10-29** 给 **W4** 补上了（还是纯 OpenCV）：

- **W4 模板匹配 + ORB 特征点** — **1 个无头回归测试 / 57 条断言**（2026-10-29 之前是 **0 个**）

**2026-10-30** 给 **W5** 补上了 —— **零测试缺口到此为止，W1–W7 全都有了**：

- **W5 二维码 + OCR** — **1 个无头回归测试 / 59 条断言**（2026-10-30 之前是 **0 个**）

补法不是「给 `main.cpp` 加个开关」，而是**把纯逻辑拆出来再测它** ——
`pixel_ops.{h,cpp}` / `color_pipeline.{h,cpp}` / `edge_pipeline.{h,cpp}` /
`feature_pipeline.{h,cpp}` / `present_pipeline.{h,cpp}`，
**demo 和测试链的是同一个源码**（不抄副本）。
详见 [notes_W1W2_Tests_2026-10.md](./notes/notes_W1W2_Tests_2026-10.md)、
[notes_W3_Tests_2026-10.md](./notes/notes_W3_Tests_2026-10.md)、
[notes_W4_Tests_2026-10.md](./notes/notes_W4_Tests_2026-10.md)
和 [notes_W5_Tests_2026-10.md](./notes/notes_W5_Tests_2026-10.md)。

> ⚠ **W5 那一轮最值钱的一条**：**「串号长什么样」这件事，仓库里有两份** ——
> 产品这边一份（`present_pipeline.h` 的 `kSerialPattern`），
> CI 门禁那边一份（`tools/check-ci-steps.ps1` 第 4 步的正则）。
> 它们**不是**「一份定义 + 一份拷贝」，是**两个互相不认识的证人** ——
> 门禁不问程序「你觉得对不对」，它**自己**在输出里重新认这个形状。
> ⇒ 两边必须认的是**同一个形状**，所以字符**必须一样**，
> 而这一点由**一条断言**钉住（逐字比，不是问程序）。
> 详见 [notes_W5_Tests_2026-10.md](./notes/notes_W5_Tests_2026-10.md)。

> ⚠ **W4 那一轮最值钱的一条**：**「像」是什么意思，是 method 定的** ——
> 同一个模板、同一张图，`TM_CCORR_NORMED` 打 **0.985996**（落在焊点上），
> `TM_CCOEFF_NORMED` 只有 **0.030943**（位置跑到**背景**上去了）——
> **而且它不报错**：照样给你一张分数图、一个位置、一个看起来很正常的分数。
> 原因：CCOEFF **去均值**，纯色模板只剩噪声可比 ——
> 「形状一样」这件事，在 CCOEFF 的眼里**根本不存在**。
> ⇒ 「工业首选 CCOEFF_NORMED」不是一条**关于目标**的话，
> 它是一条**关于目标 × method**的话。
> 详见 [notes_W4_Tests_2026-10.md](./notes/notes_W4_Tests_2026-10.md)。

> ⚠ **W3 那一轮最值钱的一条**：它把「找到 6 个焊点」这个数，
> 从**一个门槛的产物**，变成了**一个不依赖门槛的事实** ——
> 断的不是「现在是 6」，是「**门槛在 20 倍范围里动，个数都是 6**」。
> 起因是那个红色的偏移标记**压在了它要标注的焊点上面**，
> 把一块金色切成了三块（1602 / 918 / 876），
> 后两块刚好卡在面积门槛下面。
> ⇒ **标注不该毁掉它要标注的东西。**
> 详见 [notes_W3_Tests_2026-10.md](./notes/notes_W3_Tests_2026-10.md)。

## 🏆 两个综合项目

- **W6 PCB 缺陷检测** — 命令行工具，识别 7 类 PCB 缺陷（缺件/错件/极性反/焊桥/焊少/偏移/短路），输出 CSV 报告。**3 个无头回归测试 / 482 条断言**
- **W7 Mini VisionMaster Lite** — Qt 流程图编辑器，对标海康 VM EasyBuilder。17 个算子 + pin 类型检查 + 每步结果查看（多帧，带帧选择器）+ 单步执行 + 帧循环 + JSON 持久化 + 撤销重做 + **无头跑整条流水线**（`--run pipeline.json`，四种退出码），26 个无头回归测试共 2371 条断言。**已结项**（2026-10-24），欠账 19 条已收 14 条，剩下 5 条都写清了边界和重开条件 → [结项报告](./notes/notes_W7_Close_2026-10.md)

## 🔗 两个共享核心（common/）

W6 和 W7 的 PCB 判定、QR/OCR 读取**共用同一份实现**（`common/`，无 Qt 依赖）：

| 目录 | 作用 |
|---|---|
| `common/pcb_core/` | 板型配方解析 + 元件定位 + 六条判据（missing/wrong/polarity/bridge/insufficient/shifted）+ 短路 |
| `common/qr_ocr/` | QR 解码 + tesseract 文字 OCR（**W5/W6/W7 共用**，2026-10-20 合并） |

`common/pcb_core/` 的文件：

| 文件 | 作用 |
|---|---|
| `pcb_core.h/.cpp` | 板型配方解析 + 元件定位 + 六条判据（missing/wrong/polarity/bridge/insufficient/shifted）+ 短路 |
| `pcb_color_ref.h` | 颜色判据去绝对化（相对板面基色），从 W7 的 operators/ 搬过来的 |
| `mini_json.h/.cpp` | 无依赖 JSON 读取器 —— 配方解析器**只留一个**，两边共用 |

**为什么要共用**：W6 原本是 `if (label == "R2")` 七处硬编码，换块板规则全空转
（实测三块好板全被判废、两块废板输出与好板逐字相同）；W7 早就改成了配方驱动 +
逐条标定的版本。硬把 W7 的算法抄一份到 W6，就是本项目栽过四遍的「抄副本」——
抄出来的那份会随 W7 的标定一起漂，漂了没人知道。

现在 W6 的 21 张测试图（8 张 demo + 13 张 complex）和 W7 算子走**同一份**判定。

`common/qr_ocr/` 是第二块共享核心：**QR 解码 + tesseract 调用**，
W5 / W6 / W7 三边共用一份（合并前是**三份**拷贝，而且三份从来没被放在一起测过）。
它钉住的是同一类契约：**"没读"和"读了但是空"必须分得开**，
tesseract 没装时必须**明说**原因，不能悄悄返回空串。


### W7 最终版补上的东西

| 能力 | 说明 |
|---|---|
| 3 种图像源算子 + 1 条兜底路径 | 文件夹 / 视频 / 工业相机（VM 是四种，W7 少一种）。**「本地单张图像」不是一个算子** —— 喂单张图走的是引擎的兜底输入 `image_path`（headless 的 `--image` 就是它），算子库里没有这一项 |
| 帧循环 | 源输出 `frame_count`，引擎按 `frame_index` 逐轮跑整条链 + 批量 PASS/FAIL 汇总 |
| W6 深度联动 | `pcb_inspect` + `pcb_defect_judge` 把 PCB 算法挂进画布，7 类缺陷 7 个阈值可调（极性反那条不再是阈值判据 —— 期望侧由配方给）|
| DL 算子 | `dl_infer`（ONNX Runtime，条件编译）；配套的 `dl_postprocess`（letterbox/NMS/YOLOv5-v8 解析）是它的**辅助库**，不是独立算子 |
| 没装 SDK 的 DL 分支也能测 | `dl_infer` 的 `#ifdef` / `#else` **两半各有测试**（44 / 10 条断言）—— 前半用一套 header-only 的**假 ONNX Runtime**（只在测试目标里，绝不进发货二进制）。第一次真把它编起来就抓到 3 个 bug（NCHW 张量喂了 HWC 数据 / 名字指针悬空 / 输出维度不检查）|
| 没装 SDK 的相机分支也能测 | `image_source_camera` 的 `#ifdef` / `#else` **两半各有测试**（93 / 15 条断言）—— 前半用一套 header-only 的**假 MVS SDK**。第一次真把它编起来就发现：那半边引用了一个**真 SDK 里根本不存在的常量**（`MV_TRIGGER_MODE_SOFT`）——也就是说**装上 SDK 的机器上它编不过**，而没人知道。另有 6 个静默失效（IP 对不上也照样连 / Bayer 一律按 BG 解 / 改了 IP 不重连 / 句柄只开不关 / SDK 返回值全不看 / 缓冲区多大都不看）|
| 测量类 | `line_fit`（长度+角度）/ `circle_fit`（圆心+半径+圆度）/ `caliper_dist`（点到线距离），补齐 VM 四大模块最后一块 |
| 每步结果查看 | 节点状态点 + 右键回看任意一步的图和全部字段（数字**全帧留**，图只留最近 N 帧；图没了明说，绝不拿别的帧顶替）+ 标题条上的**帧选择器** `[◀][spin][▶]` |
| pin 类型检查 | 算子声明吃/吐什么，连错时立刻说明原因 |
| 最近工程 | 记最近 5 个 `.sol.json`，自动剔掉已删除的 |

## 🧪 W7 回归测试

```powershell
cd W7_VisionMasterLite
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build -C Release --output-on-failure'
```

26 个套件 / 2371 断言，`QT_QPA_PLATFORM=offscreen` 无需显示器。
其中 `pcb_operators` 直接把 W6 当时的肉眼验收清单（8 张样本图）变成可重复执行的测试，
`pcb_core` 则是共享判定核心自己的测试（**W6 和 W7 链的是同一份源码**）。

> 🧹 **测试产物落在 `build/test_work/<套件名>/`，不落源码树。**
> 每个套件一份工作目录（`ctest -j` 并行也不会互相踩），
> 配方和测试图由构建时暂存进去 —— 改完数据下次构建就生效，不会拿旧数据跑。
> 跑完测试 `git status` 应该是干净的；不干净就说明有产物写进源码树了。
> 详见 [notes_TestWorkDir_2026-10.md](./notes/notes_TestWorkDir_2026-10.md)。

> ⚠️ **`-C Release` 不能省。** 生成器是 VS 多配置版，`CMAKE_CONFIGURATION_TYPES`
> 同时含 Debug 和 Release，而 ctest 不带 `-C` 时默认按 **Debug** 去找 exe。
> 只构建过 Release 的话，14 个测试会全部报 `Not Run`：
>
> ```
> 0% tests passed, 14 tests failed out of 14
> ```
>
> 看着像代码全坏了，其实一个测试都没跑。

> 测试失败时想看是哪条断言挂了，CMakeLists 里已经给**全部套件**都设了
> `QT_FORCE_STDERR_LOGGING=1`（名单取自 `${W7_TEST_SUITES}` —— **机器维护**，
> 不再手写；那一段曾经漏了 8 个套件，让计数器默默少算了 567 条断言，
> 详见 [notes_W7_SuiteList_2026-10.md](./notes/notes_W7_SuiteList_2026-10.md)）。
> 不加的话 Qt 6 在 Windows 上会把 `qInfo`
> 送到调试通道而不是 stderr，`--output-on-failure` 只能看到一个非零退出码。

## 🤖 CI

```powershell
powershell -ExecutionPolicy Bypass -File tools/ci.ps1
```

一条命令跑完：**7 个工程全部编译（Release）→ W1 1 + W2 1 + W3 1 + W4 1 + W5 1 + W6 3 + W7 26 共 34 套件
全部跑通 → 数断言并**核对文档里引用的每一个数**
→ **核对文档里每一个相对链接都真的能走到**
→ W5 的 exe 冒烟 → W7 真跑一条完整流水线（headless）
→ 检查源码树没被写脏**。

最后一条是重点：测试产物必须落进 `build/`，源码树必须一个字节都不变。
这条以前靠 `.gitignore` 藏，现在是**断言**——CI 会在产物写进源码树时直接红。

还有一条是新的：**文档里引用的每一个「当前事实」的数，都必须和刚测出来的数一样**
（`tools/check-doc-numbers.ps1`，CI 的 Step 3c）。
期望值**从文档读**，不**抄**一份到 CI 里——抄的那份就是下一个会漂的东西；
而且**逐套件**表逐行查，不只查总数（**总数对得上，分部可以对得上不对**）。
它当场逮到三处真漂移，其中一处是根 README 说 `camera` 是 90、实测 93。

2026-10-26 又加了第四处：检查器原来**只查 W7 的表**，
改成**每个项目都查自己的表**之后，
**W6 的表当场漂了出来**（写着 180/36/163 = 379，
实测 279/36/167 = 482，而**同一份 README 的正文**写着 482）。
**一个没人检查的表，就是一个会漂的表。**

同一轮还发现根 README 那张「每份笔记多大」的表**30 行里错了 8 行**（一个差 29 KB），
**而总数一直是对的**——一行的错可以被另一行的错抵消。
现在那张表也**逐份**查，而且要求它和 `notes/` 目录**一一对应**。

再往后是 **Step 3d（`tools/check-links.ps1`）**：**文档里每一个相对链接都要真的能走到**。
起因是上一轮**删掉了一张重复的卡**——**三张别的卡还指着它**，
而链接的文字还是那么好看、markdown 还是照常渲染、**CI 还是绿的**。
**链接本身一个字都没变，变的是它指的那个文件没了** ⇒ 只能**跟着走一遍**才能发现。
写完检查器，第一次跑就逮到**另一个**坏链接（早就在那儿了：
`W7_VisionMasterLite/README.md` 里的路径多了一层目录）。
详见 [notes_W1W2_Tests_2026-10.md](./notes/notes_W1W2_Tests_2026-10.md) 和
[notes_W7_DocNumbers_2026-10.md](./notes/notes_W7_DocNumbers_2026-10.md)。

再往后是 **Step 3e（`tools/check-sections.ps1`）**：**文档里每一个章节指针都要真的落得到那一章**。
链接是关于**文件**的主张，章节指针是关于**位置**的主张 ——
「详见那份笔记的第十节」在**那一节被重新编号**之后，
**一个字都没变**，而且**读起来完全正常**（只不过指的是另一节）。
⇒ 链接坏了还能靠「文件列表里没有它」想起来；**章节指针坏了连这个都没有**。

量出来的爆炸半径：文档里 **193 个章节指针**，其中 **60 个指着同一份文件**（欠账台账）——
改**那一个标题**，**37 个指针当场失效**（变异实测），而**CI 还是绿的**。
⇒ 所以这一笔还做了一件**看不出来但该做**的事：把链接检查器**收成一份**
（`tools/lib/DocScan.ps1`）——**两份拷贝就是两份会各自漂的东西**。

详见 [notes_SectionPointers_2026-10.md](./notes/notes_SectionPointers_2026-10.md)。

再往后是 **Step 3f（`tools/mutate-census.ps1`）和 Step 3g（`tools/mutate-gates.ps1`）**：
**门禁自己的失败路径**。
上面那三道门（数、链、章节指针）**从来没红过** ——
因为**树一直是绿的**，而失败路径**只有在树红的时候才跑**。
⇒ 「CI 是绿的」只证明了**成功路径**是对的；
一道**从没红过的门禁**，和一道**坏掉的门禁**，在 CI 日志里长得**一模一样**。

这两步每次 CI 都把树**复制一份**（真树一个字都不碰），
往副本里放一个缺陷，要求门禁**必须红**，而且**必须是它该报的那句**；
再把**尺子**（判红的那一格）弄坏，要求它**必须绿**，
而且**摘要和好树一字不差** ⇒ **红是尺子报的**，不是碰巧。
详见 [notes_GateMutations_2026-10.md](./notes/notes_GateMutations_2026-10.md)。

再往后是 **Step 3h（`tools/mutate-ci-steps.ps1`）**：
**CI 自己剩下那三步**（W5 冒烟、W7 无头流水线、源码树干净）的失败路径。
它们原来是**内联**在 `tools/ci.ps1` 里的 ——
内联的检查**只能靠跑整个 CI** 来跑（十分钟，七个工程全编），
所以「把 W5 弄坏，它会不会红」这个问题**根本没法问**。
⇒ 先把它们**搬成** `tools/check-ci-steps.ps1`（**每个输入都是一个命名参数**），
再给它们**各配四条变异** ——
这一回改的是**输入**（工作目录、可执行文件、板子、流水线文件、树的快照），
不是**树**。
详见 [notes_CI_StepMutations_2026-10.md](./notes/notes_CI_StepMutations_2026-10.md)。

`.github/workflows/ci.yml` 在 GitHub Actions 上跑同一套步骤
（Windows runner + `tools/ci-deps.ps1` 装 tesseract / Qt 6.8.3 / 官方预编译 OpenCV 4.13.0）。

> ⚠️ 依赖准备那几步**命令本身已在本机实测过**
> （`tools/ci-deps.ps1 -VerifyOnly`，还对着官方包和本机那套**两种**形状的
> OpenCV 各验了一遍；这一步当场挖出三个会让 CI 第一次跑就挂的真 bug）。
> **整套 CI 也对着官方 OpenCV 4.13.0 world 包跑通了**
> （`ALL CHECKS PASSED`），runner 镜像清单也查过（choco / 7zip / Python / VS 都在）。
> **整份 workflow 也能在本机跑**：`tools/ci-local.ps1` 解析 `ci.yml` 并执行它里面的
> `run:` 块（`GITHUB_ENV` 交接、shell、退出码都按 runner 的规矩来），
> 结果 `ALL WORKFLOW STEPS PASSED` —— 本机跑的和 GitHub 跑的是同一批命令。
>
> ✅ **2026-10-26：整套 CI 在真 runner（`windows-2022`）上真的跑过了。**
> 第一次真跑**挖出四件「只有这台机器成立」的东西** ——
> 空目录不在 git 里（干净克隆上 build 挂）、
> Qt 的 DLL 不在 PATH 上（链接过了但 exe 起不来）、
> tesseract 的 PATH 只写在 YAML 里（本地彩排根本没做这件事）、
> **ACP 把同一份代码判成两回事**（`.string()` 严格窄化：本机 GBK 有中文所以绿，
> runner 的 Latin-1 没有中文就抛 1113）。
> 四件都修掉了；**修完再推一次，绿了** ——
> run [`40315236950`](https://github.com/WQian886/OpenCV-Vision-Study/actions/runs/40315236950)，
> 整条 job 8m0s，`ALL CHECKS PASSED`，
> 而且 runner 报出来的数（**29 套件 / 2853 断言**）**和本机一个字不差**。
> 详见
> [notes_CI_FirstRunnerRun_2026-10.md](./notes/notes_CI_FirstRunnerRun_2026-10.md)；
> CI 这条线的完整记录在
> [notes_CI_2026-10.md](./notes/notes_CI_2026-10.md)。

## 📖 变更日志

详细改动记录见 [notes/](./notes/) 目录（共 36 份 + 91 张 atomic 卡，~1277 KB）：

- `notes_Qt_DragDrop_2026-09.md` (16 KB) — W7 S1 拖拽原理
- `notes_Qt_NodeConnection_BugFix_2026-09.md` (17 KB) — W7 S2 连线 + 3 个 bug
- `notes_W6_ComponentRecognition_2026-09.md` (27 KB) — W6 五步完整改动
- `notes_W7_DAG_RunEngine_2026-09.md` (13 KB) — W7 P0 Run 引擎按图执行
- `notes_W7_SaveLoad_2026-09.md` (13 KB) — W7 P1 Save/Load JSON
- `notes_W7_P2P5_Finalize_2026-09.md` (9 KB) — W7 P2-P5 收尾汇总
- `notes_GitInit_2026-09.md` (10 KB) — git init 完整记录 + 踩坑
- `notes_W7_ImageSourceCamera_2026-09.md` (10 KB) — 工业相机图像源（MVS SDK 条件编译）
- `notes_W7_UndoRedo_2026-09.md` (17 KB) — M6 撤销重做 + 节点 UUID + P0 悬垂指针修复
- `notes_PCB_SharedCore_2026-10.md` (15 KB) — 共享判定核心 + W6 可测层 + 路径编码 bug
- `notes_W7_EngineeringLessons_2026-10.md` (34 KB) — W7 工程经验汇总
- `notes_PCB_Polarity_2026-10.md` (16 KB) — 极性判据重做：从「量错东西」到「量对东西」
- `notes_QR_OCR_SharedCore_2026-10.md` (15 KB) — QR/OCR 合并成共享核心（第 5 次抄副本）
- `notes_TestWorkDir_2026-10.md` (18 KB) — 测试产物落构建目录：`.gitignore` 藏起来的是文件，不是问题
- `notes_CI_2026-10.md` (48 KB) — 上 CI：路径不能写死，以及 CI 第一次跑就逮到的两个真 bug
- `notes_PCB_Short_2026-10.md` (19 KB) — 短路判据重做：从「量标记圈」到「量两个端子之间」（附：陈旧数据副本）
- `notes_PCB_Insufficient_2026-10.md` (16 KB) — 焊少判据重做：从「量 ROI 里的金」到「量这个焊盘自己的铜」
- `notes_W7_RunEngine_2026-10.md` (60 KB) — 执行引擎抽离（第十轮）+ headless runner（片 3）+ 假 ONNX Runtime（片 4）+ 假 MVS SDK（片 5）
- `notes_W7_Close_2026-10.md` (84 KB) — **W7 结项报告**：欠账总账（结项 17 条 → 现在 19 条）、每条债的边界与重开条件、明确不覆盖的范围、怎么复现这份结项
- `notes_W7_Verdict_2026-10.md` (18 KB) — 结项之后还的第一笔：把「结论怎么说」收成**一份**规则（GUI 和 CI 共用），顺带抓到 CI 的一个**漏检**
- `notes_W7_BadStep_2026-10.md` (15 KB) — 结项之后还的第二笔：让「跑坏了」真的算进判定（引擎把输出置空 → 旧判据数不到它）
- `notes_W7_RenderPlan_2026-10.md` (17 KB) — 结项之后还的第三笔：结果面板「显示哪张图」收成**一份**规则，顺带抓到本轮自己的两个洞
- `notes_W7_FrameProtocol_2026-10.md` (20 KB) — 结项之后还的第四笔：「源怎么报帧数」收成**一份**（第十份抄副本）+ 纯层瘦身 + **本项目第一条编译期约束**（只链 `Qt6::Core` 的目标往纯层塞 widget 就编不过）
- `notes_W7_ParseLayer_2026-10.md` (20 KB) — 结项之后还的第五笔：**解析层别丢节点**（未知算子留在画布上标「未安装」→ 链不断 → 那一步强制失败）+ 三个「写了没人读」的字段改名并搬进纯层，顺带抓到**两个旧用例正在断言 bug**
- `notes_W7_MainWindow_2026-10.md` (19 KB) — 结项之后还的第六笔：**主窗口第一次进测试**（主程序源码编成静态库 + offscreen 起真窗口），按「错了用户会不会自己发现」划线，顺带抓到**第十一遍抄副本**和一个**状态和画面对不上**的真 bug
- `notes_W7_Knob_Audit_2026-10.md` (128 KB) — **旋钮审计**：欠账的**唯一登记处**（第十节）。19 条账、每条的边界与重开条件、每笔的证据在哪
- `notes_W7_DocNumbers_2026-10.md` (12 KB) — 结项之后还的第七笔：**把「数字对不对得上」变成机器可查的一步**（欠账 11 的强制那半步）——期望值**读**出来不**抄**一份、逐套件表强查、当场逮到三处真漂移
- `notes_W7_SuiteList_2026-10.md` (13 KB) — 结项之后还的第八笔：**套件名单只有一份**（手抄的那份漂了 8 个名字 → 那 8 个套件的汇总行进了调试通道 → 计数器默默少算了 **567** 条断言）+ **计数的数不许比真值小而不说话**（CI 从「红」到「绿」的一笔）
- `notes_CI_FirstRunnerRun_2026-10.md` (17 KB) — 结项之后还的第九笔：**CI 第一次在真 runner 上跑**（欠账 9 收口）——真跑挖出四件「只有这台机器成立」的东西：空目录不在 git 里 / Qt 的 DLL 不在 PATH 上 / tesseract 的 PATH 只写在 YAML 里 / **ACP 把同一份代码判成两回事**（`.string()` 严格窄化：字符在 ACP 里表示不出来就抛 1113）
- `notes_W1W2_Tests_2026-10.md` (15 KB) — **给 W1 / W2 补测试**（W1–W5 零测试缺口的第一笔）+ 结项之后还的第十笔：**链接断了没人知道**（CI Step 3d）——把纯逻辑从 `main()` 里拆出来、demo 和测试链**同一份源码**、**每个门槛都是量出来的**、**变异验证当场逮到我自己的测试量错了对象**、**删掉一张卡却留下三个悬空链接而 CI 一直是绿的**
- `notes_SectionPointers_2026-10.md` (48 KB) — 结项之后还的第十一笔：**章节指针断了也没人知道**（CI Step 3e）——链接是关于**文件**的主张，章节指针是关于**位置**的主张；**193 个指针里 60 个指着同一份文件**，改**一个标题**就**37 个当场失效**（变异实测），而指针本身**一个字都没变**；收尾时还逮到门禁自己的**三个**毛病：**报红的那一秒它死了**、**被指错了树它还是绿的**、**记录自己怎么修的那张表把那个缺陷又写了一遍**；最后又给它加了**失败路径**（CI Step 3f，脚本住在仓库里，**谁都能重跑**）—— 四条变异跑在**树的副本**上，**第一次上真 runner 就红，而红的不是门禁是脚本**：副本**不是那棵树**（每个文件前面挂着一层 `C,D\`）+ 尺子**只坏了一把**（块断言还活着）；修完把它**自己抓过一次**（故意把复制逻辑改坏，它**拒绝给结论**），顺带发现那张声称「1:1」的表**少了一行** —— 行加起来 75，合计写着 76；**第二次上真 runner 又是脚本红**：副本的相对路径每个都多三个字符（`ine\` = `pristine` 的尾巴）—— runner 的 `$env:TEMP` 是 8.3 **短名**，provider 枚举时给的是**展开后**的名字，**同一个目录两个字符串**，而相对路径是**拿长度切**出来的 ⇒ 修在**共用的根解析**那一处（问 provider 要路径，别自己切字符串），并且**本机造一个 8.3 短名目录就能复现**
- `notes_W3_Tests_2026-10.md` (16 KB) — **给 W3 补测试**（W1–W5 零测试缺口的第二笔）——把纯逻辑抽成 `edge_pipeline.{h,cpp}`、demo 和测试链**同一份源码**、52 条断言**每个门槛都是量出来的**；顺带把「合成 PCB 图的事实」收成**一份**（`common/pcb_sim/`，起因是 W3 抄了一份 W2 的 HSV 窗口而注释写着「复用 W2 思路」）；**最值钱的一条**：那个「6 个焊点」曾经是**面积门槛的产物** —— 红色的偏移标记**压在了它要标注的焊点上面**，把一块金色切成三块（1602 / 918 / 876），后两块刚好卡在门槛下面 ⇒ 断言从「== 6」改成「**门槛在 20 倍范围里动，个数都是 6**」；**标注不该毁掉它要标注的东西**
- `notes_W4_Tests_2026-10.md` (16 KB) — **给 W4 补测试**（W1–W5 零测试缺口的第三笔）——把纯逻辑抽成 `feature_pipeline.{h,cpp}`、demo 和测试链**同一份源码**、57 条断言**每个门槛都是量出来的**；顺带把「合成 PCB 图的事实」收成**一份**（`common/pcb_sim/` 的**第三份**副本被拆掉，注释写着「W3 同款」而代码是抄的）；**最值钱的一条**：**「像」是什么意思，是 method 定的** —— 同一个模板、同一张图，`TM_CCORR_NORMED` 打 **0.985996**（落在焊点上），`TM_CCOEFF_NORMED` 只有 **0.030943**（位置跑到**背景**上去了），**而且不报错**（CCOEFF 去均值，纯色模板只剩噪声可比 ⇒「形状一样」在它眼里根本不存在）；顺带量出两个极端：**1.0 分是白送的**（自己抠自己）、**8 个一模一样的焊点分数散得只有 0.000587**（分不出谁是谁 ⇒ 拿金块当 ORB 模板好匹配 **0 对**）
- `notes_W5_Tests_2026-10.md` (19 KB) — **给 W5 补测试**（W1–W5 零测试缺口的**最后一笔**，七个项目就此都有套件）——把「给人看」那一层抽成 `present_pipeline.{h,cpp}`、demo 和测试链**同一份源码**（QR 解码和 tesseract 那些**共享核心**的测试**在 W7 的 `qr_ocr` 套件里，不重复测**），59 条断言**每个门槛都是量出来的**；**最值钱的一条**：这一处「**有两份**」是对的 —— 「PCB 串号长什么样」在**产品**和 **CI 门禁**各有一份，而它们**不能收成一份**（收成一份 = 让门禁去问产品「你觉得对不对」⇒ **尺子给自己打分**）⇒ 收法不是「合并」，是**加第三条**（一条断言把两边的字面量**逐字**比），而且**光字符一样还不够** —— 两边必须**问同一个问题**（`regex_search` + `icase` 对上 `-match`）；顺带量出两条**恒真断言**：量线的厚度时窗口里混进了文字的最下一行（量成 **10**，其实是 **5**），而「那几个字是绿的」用邻近搜索写 ⇒ **框自己就满足**（换成指定区域数绿像素才量到 **453**）
- `notes_GateMutations_2026-10.md` (11 KB) — **给三道文档门禁配变异**（CI Step 3g）——数、链、章节指针这三道门**从来没红过**，各配四条变异，跑在**树的副本**上；**最要紧的一条**：D（尺子坏了）**必须带着一棵坏树一起走**，而且摘要**和好树一字不差** ⇒ 「绿」和「说得和好树一样」**一起**成立，才算数；顺带把建脚本时栽的**六个跟头**记下来，每一个都差点变成**假绿**
- `notes_CI_StepMutations_2026-10.md` (17 KB) — **给 CI 剩下三步配变异**（CI Step 3h）——W5 冒烟、W7 无头流水线、源码树干净这三步**从来没红过**，而且它们是**内联**在 `ci.ps1` 里的 ⇒ 连「问一句」都不可能；这一轮先把它们**搬成**一个**输入全是参数**的检查器（`tools/check-ci-steps.ps1`），再各配四条变异；**杠杆换了**：文档门禁改**树**，这三道门改**输入**（工作目录、可执行文件、板子、流水线文件、树的快照）⇒ 还第一次验了一条**从没被验证过**的性质：「树本来就脏 ⇒ 必须绿」；顺带逮到**脚本自己**坏了——**逗号比加号优先** ⇒ 八个 token 变成四个 ⇒ 检查器**压根没启动**，而脚本把「exit 1，零条红」当成了「没意见」⇒ 修法不是小心，是**断言命令行的形状**；**推上去之后又改了两处**：④ 那条原来写着「真程序做不出来 ⇒ 站桩」，**量了以后发现是错的**（W5 找图的路径是**写死的相对路径**，换一张图，**真程序**就解出**形状不对**的号，退出码**还是 0**）⇒ 改成**真程序 + 一张错的图**，桩少了一个；⑤ 顺带逮到 **CI 自己红了** —— 树的快照写进 `build\ci`，而那个目录是**构建**建的、构建在**后面**才跑 ⇒ **干净克隆上那一刻它还不存在** ⇒ 写文件**抛异常**，而 `$ErrorActionPreference = 'Continue'` 把异常**吞了** ⇒ **16 分钟之后**最后那一步才炸；本机绿是因为本机**早就 build 过** ⇒ 修法：**先把目录建出来**，写完**当场检查**

`notes/atomic/` 里是**单条教训卡片**（91 张）：一条坑一张卡，
能单独引用、能单独复习。挑几条：

- `at-losing-a-field-at-a-json-boundary.md` — 「字段存在」≠「字段活过了中间那一跳」
- `at-criterion-must-not-guess-the-baseline.md` — 判据不猜：期望值由知道真相的那一方登记
- `at-test-leaves-residue-permanently-red.md` — 测试自己制造的失败：一次坏掉，永久变红
- `at-gitignore-hides-files-not-problems.md` — `.gitignore` 藏起来的是文件，不是问题
- `at-lookalike-dir-fools-the-locator.md` — 靠名字猜出来的位置，会被同名的东西骗到
- `at-reported-vs-named.md` — 「报了缺陷」≠「报的是那个缺陷」
- `at-mutation-ruler-broken.md` — 变异尺子本身坏了
- `at-measure-the-thing-not-the-mark.md` — 判据要量物理事实，不要量「画给人看的标记」
- `at-stale-data-copy-fakes-a-pass.md` — 陈旧的数据副本会让测试「判得对」——判的是旧东西
- `at-unverified-number-in-comment.md` — 注释里的数字没人验证过，它可能是推演出来的
- `at-the-roi-is-not-the-pad.md` — 量对象错位：ROI 是本体框，量到的从来不是焊盘
- `at-four-copies-of-one-number.md` — 同一个数抄四份，改的那一份没被测到
- `at-even-samples-hide-the-median.md` — 只在多脚样本上验「中位数」，等于没验
- `at-a-blind-reading-cannot-watch.md` — 读数对什么不敏感，就别指望它管别的事
- `at-no-runner-is-not-a-reason.md` — 「没在真环境上验过」是结论，不是理由
- `at-a-fixture-must-look-like-the-product.md` — 测试夹具的形状必须和产品写出来的一样
- `at-one-code-cannot-say-what-broke.md` — 一个退出码说不出「哪里坏了」
- `at-a-limit-you-keep-needs-a-boundary.md` — 有意留下的限制，必须写清边界和重开条件
- `at-the-decoder-eats-the-newline.md` — 默认编码的解码器会吃掉换行符，于是你数出来的行数偏少
- `at-the-last-frame-is-not-the-batch.md` — 末帧判定不等于整批判定：拿末帧当整批，就是漏检
- `at-the-dropped-node-hides-the-broken-chain.md` — 丢一个节点会把整条链藏起来
- `at-the-state-and-the-screen-must-agree.md` — 界面看着正常，但**内部状态**和画面对不上
- `at-programmatic-calls-are-not-user-actions.md` — 程序化调 API ≠ 用户操作（差的那部分就是测试和产品的差距）
- `at-ninja-mtime-stale-binary.md` — 过期二进制给出**看着正常的错数**
- `at-the-expected-value-must-be-read-not-copied.md` — 期望值要**读**出来，不要**抄**一份（在检查器里写死 = 抄副本）
- `at-the-total-can-hide-a-wrong-part.md` — 总数对得上，**分部**可以对不上
- `at-a-historical-snapshot-looks-like-a-current-claim.md` — 历史快照长得和当前声明一模一样，得划作用域
- `at-the-counter-must-not-undercount-silently.md` — 数的那一步也会数不全，而且它**不说话**（少算的那部分最容易出事）
- `at-an-empty-dir-is-not-in-git.md` — git 只带文件不带目录 ⇒ 空目录是**这台机器的事实**，不是仓库的事实
- `at-the-runtime-dll-lookup-is-a-machine-fact.md` — 构建期用**路径**、运行期用**名字** ⇒ 链接成功 ≠ exe 跑得起来
- `at-a-rehearsal-can-pass-because-it-was-skipped.md` — 被跳过的那段代码 = 还没写过的代码（彩排的绿灯可能来自「压根没跑」）
- `at-the-detector-must-not-die-on-what-it-detects.md` — 检测器不能被它要检测的东西杀死（否则「没检出」和「检测器死了」长得一样）
- `at-a-number-that-holds-in-one-place-is-not-a-fact.md` — 一个只在一处成立的数，不是事实，是那一处的产物（断「稳定性」，别断「== N」）
- `at-two-witnesses-must-ask-the-same-question.md` — 两份**证人**不能互相抄；而且**光字符一样还不够**，两边得**问同一个问题**

W7 收官期的改动（PCB/DL 算子、视频源、帧循环、每步结果查看、pin 检查、最近工程）
逐条记在 git 提交信息里，每条都写清了"为什么"和"测试抓到了什么"。

## 🔨 编译

每个 W 独立 CMake 构建：

```powershell
# 以 W6 为例
cd W6_PCB_DefectProject
cmake --build out/build/x64-Release --target w6_pcb_defect

# 运行
out/build/x64-Release/w6_pcb_defect.exe report images
```

各 W 的具体命令详见各自 README。

## 📂 项目结构

```
OpenCV/
├── W1-W7/                 # 7 个学习项目
├── common/pcb_core/       # W6/W7 共用的判定核心(无 Qt 依赖)
├── common/qr_ocr/         # W5/W6/W7 共用的 QR/OCR 核心
├── common/pcb_sim/        # W2/W3 共用的「合成 PCB 图的事实」(颜色/噪声/HSV 窗口)
├── common/cmake/          # 三方依赖路径(本机默认值 + CI 覆盖)
├── tools/ci.ps1           # CI 脚本:构建 + 测试 + 核对文档里的数 + 检查源码树没被写脏
├── tools/check-doc-numbers.ps1  # 文档里每个「当前事实」的数 ↔ 实测（期望值从文档读）
├── tools/check-links.ps1  # 文档里每个相对链接 → 真的能走到（链接不会自己漂）
├── tools/check-sections.ps1  # 文档里每个章节指针 → 真的落得到那一章（指针不会自己漂）
├── tools/check-ci-steps.ps1  # CI 第 4/5/6 步搬成的检查器（每个输入都是一个命名参数）
├── tools/mutate-census.ps1  # 那道门的变异验证（量它的脚本得在仓库里，谁都能重跑）
├── tools/mutate-gates.ps1  # 数、链、章节指针三道门自己的变异验证（12 条，每次 CI 都跑）
├── tools/mutate-ci-steps.ps1  # CI 第 4/5/6 步自己的变异验证（12 条，每次 CI 都跑）
├── tools/lib/DocScan.ps1  # 两个文档检查器共用的那一份扫描逻辑（不抄副本）
├── tools/lib/MutHarness.ps1  # 两个变异脚本共用的那一份机械（造副本、跑子进程、比对摘要）
├── tools/fixtures/    # 变异用的**素材**（形状不对的二维码 + 它的生成脚本）
├── .github/workflows/     # GitHub Actions
├── notes/                 # 变更日志（36 份 + 91 张 atomic 卡）
├── datasets/              # 共享数据集（空,占位）
├── images/                # 共享图片（空,占位）
├── .gitignore             # Git 忽略规则
└── README.md              # 本文件
```

## 📊 统计

- 7 个 C++ 项目 + 2 个共享核心
- 149 个 C++ 文件 / **46,113 行**（含注释与空行）。口径是**一条能跑的完整命令**，2026-10-30 实测：

  ```powershell
  $files = git ls-files '*.cpp' '*.h'
  $total = 0
  $files | ForEach-Object { $total += (Get-Content -Encoding UTF8 -LiteralPath $_).Count }
  ```

  > ⚠ **`-Encoding UTF8` 不能省。** 少了它，PowerShell 5.1 会按 GBK 读 UTF-8 源码，
  > **把中文后面的行尾当尾字节吃掉** —— 121 个文件里 102 个对不上，
  > 总共少算 **3,268 行**，而且**少算最多的恰恰是中文注释最多的文件**
  > （这个指标会奖励写得少的人，方向是反的）。
  > 旧数字「33,088 行」就是这么来的，它**复现不出来** ——
  > 连最接近的 ANSI 口径（33,368）也对不上。
  > 详见 [at-the-decoder-eats-the-newline.md](./notes/atomic/at-the-decoder-eats-the-newline.md)。
- 36 份变更日志 + 91 张 atomic 卡 / ~1277 KB
- **34 个无头回归测试 / 3104 条断言**（W7 26 个 / 2371，W6 3 个 / 482，W5 1 个 / 59，W4 1 个 / 57，W3 1 个 / 52，W2 1 个 / 54，W1 1 个 / 29；
  其中 `pcb_core` 和 `qr_ocr` 各一套源码链进两个项目，两边都跑）
- 1.9 GB 项目体积（其中 .vs/build 占 95%，git 仓库仅 ~5 MB）
