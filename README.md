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

- **W6 PCB 缺陷检测** — 命令行工具，识别 7 类 PCB 缺陷（缺件/错件/极性反/焊桥/焊少/偏移/短路），输出 CSV 报告。**3 个无头回归测试 / 477 条断言**
- **W7 Mini VisionMaster Lite** — Qt 流程图编辑器，对标海康 VM EasyBuilder。17 个算子 + pin 类型检查 + 每步结果查看（多帧，带帧选择器）+ 单步执行 + 帧循环 + JSON 持久化 + 撤销重做 + **无头跑整条流水线**（`--run pipeline.json`，四种退出码），26 个无头回归测试共 2371 条断言。**已结项**（2026-10-24），欠账 19 条已收 12 条，剩下 7 条都写清了边界和重开条件 → [结项报告](./notes/notes_W7_Close_2026-10.md)

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
| 没装 SDK 的相机分支也能测 | `image_source_camera` 的 `#ifdef` / `#else` **两半各有测试**（90 / 15 条断言）—— 前半用一套 header-only 的**假 MVS SDK**。第一次真把它编起来就发现：那半边引用了一个**真 SDK 里根本不存在的常量**（`MV_TRIGGER_MODE_SOFT`）——也就是说**装上 SDK 的机器上它编不过**，而没人知道。另有 6 个静默失效（IP 对不上也照样连 / Bayer 一律按 BG 解 / 改了 IP 不重连 / 句柄只开不关 / SDK 返回值全不看 / 缓冲区多大都不看）|
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

> 测试失败时想看是哪条断言挂了，CMakeLists 里已经给 26 个套件都设了
> `QT_FORCE_STDERR_LOGGING=1`。不加的话 Qt 6 在 Windows 上会把 `qInfo`
> 送到调试通道而不是 stderr，`--output-on-failure` 只能看到一个非零退出码。

## 🤖 CI

```powershell
powershell -ExecutionPolicy Bypass -File tools/ci.ps1
```

一条命令跑完：**7 个工程全部编译（Release）→ W6 3 套件 + W7 26 套件
全部跑通 → W5 的 exe 冒烟 → W7 真跑一条完整流水线（headless）
→ 检查源码树没被写脏**。

最后一条是重点：测试产物必须落进 `build/`，源码树必须一个字节都不变。
这条以前靠 `.gitignore` 藏，现在是**断言**——CI 会在产物写进源码树时直接红。

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
> 真正干活的那几步（配置 / 编译 / 测试 / 源码树检查）已在本机
> `tools/ci.ps1` 跑通。**还没在真实 runner 上验过的**只剩两条**纯环境性**的
> （choco 那个安装动作、GitHub 自己的调度与网络），
> 哪些验过、哪些没验过，见
> [notes_CI_2026-10.md](./notes/notes_CI_2026-10.md)。

## 📖 变更日志

详细改动记录见 [notes/](./notes/) 目录（共 26 份 + 67 张 atomic 卡，~893 KB）：

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
- `notes_CI_2026-10.md` (19 KB) — 上 CI：路径不能写死，以及 CI 第一次跑就逮到的两个真 bug
- `notes_PCB_Short_2026-10.md` (19 KB) — 短路判据重做：从「量标记圈」到「量两个端子之间」（附：陈旧数据副本）
- `notes_PCB_Insufficient_2026-10.md` (16 KB) — 焊少判据重做：从「量 ROI 里的金」到「量这个焊盘自己的铜」
- `notes_W7_RunEngine_2026-10.md` (60 KB) — 执行引擎抽离（第十轮）+ headless runner（片 3）+ 假 ONNX Runtime（片 4）+ 假 MVS SDK（片 5）
- `notes_W7_Close_2026-10.md` (43 KB) — **W7 结项报告**：欠账总账（结项 17 条 → 现在 19 条）、每条债的边界与重开条件、明确不覆盖的范围、怎么复现这份结项
- `notes_W7_Verdict_2026-10.md` (18 KB) — 结项之后还的第一笔：把「结论怎么说」收成**一份**规则（GUI 和 CI 共用），顺带抓到 CI 的一个**漏检**
- `notes_W7_BadStep_2026-10.md` (15 KB) — 结项之后还的第二笔：让「跑坏了」真的算进判定（引擎把输出置空 → 旧判据数不到它）
- `notes_W7_RenderPlan_2026-10.md` (16 KB) — 结项之后还的第三笔：结果面板「显示哪张图」收成**一份**规则，顺带抓到本轮自己的两个洞
- `notes_W7_FrameProtocol_2026-10.md` (19 KB) — 结项之后还的第四笔：「源怎么报帧数」收成**一份**（第十份抄副本）+ 纯层瘦身 + **本项目第一条编译期约束**（只链 `Qt6::Core` 的目标往纯层塞 widget 就编不过）
- `notes_W7_ParseLayer_2026-10.md` (20 KB) — 结项之后还的第五笔：**解析层别丢节点**（未知算子留在画布上标「未安装」→ 链不断 → 那一步强制失败）+ 三个「写了没人读」的字段改名并搬进纯层，顺带抓到**两个旧用例正在断言 bug**
- `notes_W7_MainWindow_2026-10.md` (18 KB) — 结项之后还的第六笔：**主窗口第一次进测试**（主程序源码编成静态库 + offscreen 起真窗口），按「错了用户会不会自己发现」划线，顺带抓到**第十一遍抄副本**和一个**状态和画面对不上**的真 bug
- `notes_W7_Knob_Audit_2026-10.md` (121 KB) — **旋钮审计**：欠账的**唯一登记处**（第十节）。19 条账、每条的边界与重开条件、每笔的证据在哪

`notes/atomic/` 里是**单条教训卡片**（67 张）：一条坑一张卡，
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
├── common/cmake/          # 三方依赖路径(本机默认值 + CI 覆盖)
├── tools/ci.ps1           # CI 脚本:构建 + 测试 + 检查源码树没被写脏
├── .github/workflows/     # GitHub Actions
├── notes/                 # 变更日志（26 份 + 67 张 atomic 卡）
├── datasets/              # 共享数据集（空,占位）
├── images/                # 共享图片（空,占位）
├── .gitignore             # Git 忽略规则
└── README.md              # 本文件
```

## 📊 统计

- 7 个 C++ 项目 + 2 个共享核心
- 131 个 C++ 文件 / **41,330 行**（含注释与空行）。口径是**一条能跑的完整命令**，2026-10-25 实测：

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
- 26 份变更日志 + 67 张 atomic 卡 / ~852 KB
- **29 个无头回归测试 / 2848 条断言**（W7 26 个 / 2371，W6 3 个 / 477；
  其中 `pcb_core` 和 `qr_ocr` 各一套源码链进两个项目，两边都跑）
- 1.9 GB 项目体积（其中 .vs/build 占 95%，git 仓库仅 ~5 MB）
