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

## 🏆 两个综合项目

- **W6 PCB 缺陷检测** — 命令行工具，识别 7 类 PCB 缺陷（缺件/错件/极性反/焊桥/焊少/偏移/短路），输出 CSV 报告。**3 个无头回归测试 / 379 条断言**
- **W7 Mini VisionMaster Lite** — Qt 流程图编辑器，对标海康 VM EasyBuilder。14 个算子 + pin 类型检查 + 每步结果查看（多帧，带帧选择器）+ 单步执行 + 帧循环 + JSON 持久化 + 撤销重做，14 个无头回归测试共 1101 条断言

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

现在 W6 的 17 张测试图（8 张 demo + 9 张 complex）和 W7 算子走**同一份**判定。

`common/qr_ocr/` 是第二块共享核心：**QR 解码 + tesseract 调用**，
W5 / W6 / W7 三边共用一份（合并前是**三份**拷贝，而且三份从来没被放在一起测过）。
它钉住的是同一类契约：**"没读"和"读了但是空"必须分得开**，
tesseract 没装时必须**明说**原因，不能悄悄返回空串。


### W7 最终版补上的东西

| 能力 | 说明 |
|---|---|
| 4 种图像源 | 本地 / 文件夹 / 视频 / 工业相机（对标 VM 四种） |
| 帧循环 | 源输出 `frame_count`，引擎按 `frame_index` 逐轮跑整条链 + 批量 PASS/FAIL 汇总 |
| W6 深度联动 | `pcb_inspect` + `pcb_defect_judge` 把 PCB 算法挂进画布，7 类缺陷 7 个阈值可调（极性反那条不再是阈值判据 —— 期望侧由配方给）|
| DL 算子 | `dl_postprocess`（letterbox/NMS/YOLOv5-v8 解析）+ `dl_infer`（ONNX Runtime，条件编译） |
| 测量类 | `line_fit`（长度+角度）/ `circle_fit`（圆心+半径+圆度）/ `caliper_dist`（点到线距离），补齐 VM 四大模块最后一块 |
| 每步结果查看 | 节点状态点 + 右键回看任意一步的图和全部字段（数字**全帧留**，图只留最近 N 帧；图没了明说，绝不拿别的帧顶替）+ 标题条上的**帧选择器** `[◀][spin][▶]` |
| pin 类型检查 | 算子声明吃/吐什么，连错时立刻说明原因 |
| 最近工程 | 记最近 5 个 `.sol.json`，自动剔掉已删除的 |

## 🧪 W7 回归测试

```powershell
cd W7_VisionMasterLite
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build -C Release --output-on-failure'
```

14 个套件 / 1101 断言，`QT_QPA_PLATFORM=offscreen` 无需显示器。
其中 `pcb_operators` 直接把 W6 当时的肉眼验收清单（8 张样本图）变成可重复执行的测试，
`pcb_core` 则是共享判定核心自己的测试（**W6 和 W7 链的是同一份源码**）。

> 🧹 **测试产物落在 `build/test_work/<套件名>/`，不落源码树。**
> 每个套件一份工作目录（`ctest -j` 并行也不会互相踩），
> 配方和测试图由构建时暂存进去 —— 改完数据下次构建就生效，不会拿旧数据跑。
> 跑完测试 `git status` 应该是干净的；不干净就说明有产物写进源码树了。
> 详见 [notes_TestWorkDir_2026-10.md](./notes/notes_TestWorkDir_2026-10.md)。

> ⚠️ **`-C Release` 不能省。** 生成器是 VS 多配置版，`CMAKE_CONFIGURATION_TYPES`
> 同时含 Debug 和 Release，而 ctest 不带 `-C` 时默认按 **Debug** 去找 exe。
> 只构建过 Release 的话，13 个测试会全部报 `Not Run`：
>
> ```
> 0% tests passed, 13 tests failed out of 13
> ```
>
> 看着像代码全坏了，其实一个测试都没跑。

> 测试失败时想看是哪条断言挂了，CMakeLists 里已经给 11 个套件都设了
> `QT_FORCE_STDERR_LOGGING=1`。不加的话 Qt 6 在 Windows 上会把 `qInfo`
> 送到调试通道而不是 stderr，`--output-on-failure` 只能看到一个非零退出码。

## 📖 变更日志

详细改动记录见 [notes/](./notes/) 目录（共 14 份 + 21 张 atomic 卡，~230 KB）：

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
- `notes_W7_EngineeringLessons_2026-10.md` (35 KB) — W7 工程经验汇总
- `notes_PCB_Polarity_2026-10.md` (17 KB) — 极性判据重做：从「量错东西」到「量对东西」
- `notes_QR_OCR_SharedCore_2026-10.md` (14 KB) — QR/OCR 合并成共享核心（第 5 次抄副本）
- `notes_TestWorkDir_2026-10.md` (18 KB) — 测试产物落构建目录：`.gitignore` 藏起来的是文件，不是问题

`notes/atomic/` 里是**单条教训卡片**（21 张）：一条坑一张卡，
能单独引用、能单独复习。挑几条：

- `at-losing-a-field-at-a-json-boundary.md` — 「字段存在」≠「字段活过了中间那一跳」
- `at-criterion-must-not-guess-the-baseline.md` — 判据不猜：期望值由知道真相的那一方登记
- `at-test-leaves-residue-permanently-red.md` — 测试自己制造的失败：一次坏掉，永久变红
- `at-gitignore-hides-files-not-problems.md` — `.gitignore` 藏起来的是文件，不是问题
- `at-reported-vs-named.md` — 「报了缺陷」≠「报的是那个缺陷」
- `at-mutation-ruler-broken.md` — 变异尺子本身坏了

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
├── notes/                 # 变更日志（14 份 + 21 张 atomic 卡）
├── datasets/              # 共享数据集（空,占位）
├── images/                # 共享图片（空,占位）
├── .gitignore             # Git 忽略规则
└── README.md              # 本文件
```

## 📊 统计

- 7 个 C++ 项目 + 2 个共享核心
- 96 个 C++ 文件 / ~17700 行代码
- 14 份变更日志 + 21 张 atomic 卡 / ~230 KB
- **17 个无头回归测试 / 1480 条断言**（W7 14 个 / 1101，W6 3 个 / 379；
  其中 `pcb_core` 和 `qr_ocr` 各一套源码链进两个项目，两边都跑）
- 1.9 GB 项目体积（其中 .vs/build 占 95%，git 仓库仅 ~5 MB）
