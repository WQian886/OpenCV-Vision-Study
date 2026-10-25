# W7：结果面板「显示哪张图」收成一份（欠账 17 的第二半）

> 2026-10-25 · 旋钮审计结项之后的**第三笔**
> 上一篇：`notes/notes_W7_BadStep_2026-10.md`（欠账 18 收一半）

## 0. 一句话

「这一步该给用户看哪张图」这件事以前有**两份**实现，
作者**自己知道**、**自己写下了警告**，然后它们真的跑偏了 ——
判据查的是缺陷标注图，面板显示的是**工具栏那张静态图**。
这一轮把它收成一份（`result_render_plan.h`），
结果**第一版只收了一半**，被变异验证逮个正着 ——
本轮自己的两个洞（一条规则从没被测过、一条守卫被注释喂饱）
和一个「病搬家了」的第九份副本。

## 1. 起因：第八次抄副本

`step_frame_cache.h` 里的注释原文：

> 「优先级必须和 `renderOutputToResultPanel` 的显示顺序对齐 ——
> 两份各写一份的话，加一种新输出类型就只改一处，于是
> "图还在不在"的判据和"显示哪张"会跑偏，
> 而**跑偏的方向恰恰是那个会撒谎的方向**。」

**警告就写在代码里，然后它们真的跑偏了。**

| | 查/显示的是 |
|---|---|
| `step_frame_cache.h :: stepPrimaryImagePath` | `mask_path` → `marked_path` → `image_path` |
| `mainwindow.cpp :: renderOutputToResultPanel` | `mask` 图 → 模板框 → PASS/FAIL 大字 → 原图 |

面板那份**压根不认 `marked_path`**。

## 2. 它真的撒谎了（实测，不是推演）

`pcb_defect_judge` 的输出形状：

```
marked_path / defects / defect_count / result / metrics / ...
```

**没有 `image_path`。**

于是面板的 PASS/FAIL 大字那条分支（**无条件执行**）
取基底图时取不到 `image_path`，退到 `fallbackPath`
—— **工具栏那张用户手填的静态图**。

而「图还在不在」那个判据查的是 `marked_path`
（逐帧的缺陷标注图）。

**两个说的不是同一张图。**

后果：

- **缺陷标注图明明在，却一张都没显示过**；
- 多帧流水线里，工具栏那张静态图和第 7 帧**根本不是同一张**；
- 判据说「图在」，面板显示的却是另一张 —— **对应关系是假的**。

而且 `mainwindow.cpp` 里那道「不撒谎」的最后一道闸，
**自己写下了这个坑**：

> 「分支 3/4 的 fallback 是工具栏那张原图 —— 用户手填的静态图，
> 和任何一帧都没有关系。拿它配这一帧的数字，屏幕上是一张正常图，
> 数字也正常，唯独**对应关系是假的**。」

它点名了分支 3，但代码里那道闸只挡了 `imgState == Dropped`，
**挡不住「拿 fallback 冒充这一帧」**。

## 3. 怎么收：`result_render_plan.h`

新建 `W7_VisionMasterLite/result_render_plan.h`（**纯 QtCore**，266 行）：
纯函数、头文件内联、无状态、无副作用 —— 单测可以**不带 MainWindow、不带 QWidget** 直接调。

| 东西 | 是什么 |
|---|---|
| `ImageSource{Marked,Mask,Source,Fallback}` | 基底图的来源 |
| `ImageCandidate{source,path,isFallback}` | 一个候选 |
| `RenderOverlay{None,Verdict,TemplateBox}` | 叠加 |
| `RenderPlan` | 整个显示计划 |
| `resultImageCandidates(out, fallbackPath)` | **候选列表**（表驱动） |
| `decideOverlay(out, …)` | 叠加判定 |
| `planResultRender(out, fallbackPath)` | 整个计划 |
| `pickBaseImage(cands, exists)` | **取哪张**（两边共用） |
| `ExistsFn` / `fileExists` | 存在性谓词（从 `step_frame_cache.h` 搬来） |

### 顺序写成**表**，不是三段 `if`

```cpp
static const KeySource kTable[] = {
    { "marked_path", ImageSource::Marked  },
    { "mask_path",   ImageSource::Mask    },
    { "image_path",  ImageSource::Source  },
};
```

> 顺序写成代码，就又变成了「藏在代码顺序里的规则」：
> 读的人得逐行读下去才知道谁优先，
> 而改的人只要动一行就能悄悄改掉顺序。
> 写成表，顺序是**数据**：一眼看得见，也能被一条变异单独打乱。

### 三处改动

- `step_frame_cache.h`：**删掉 `stepPrimaryImagePath`**（副本本体）、
  删掉 `StepFrameResult::imagePath`（缓存一份就等于又埋一份会过期的副本）、
  `imageState()` 改用共享候选列表 + 共享取图函数。
- `mainwindow.cpp`：`renderOutputToResultPanel` 改用 `w7::planResultRender` +
  `w7::pickBaseImage`，叠加从「代码顺序」变成**显式规则**。
- 新套件 `tests/test_result_render_plan.cpp`（**62 条**，6 个用例）。

## 4. 变异第 17 轮**第一遍**：抓到了我自己两个洞

第一遍 14 条变异（M145~M158）跑完：**12 杀 2 活**，
`unexpected results = 2`。

那两条「活」的不是变异写错，是**我本轮自己留的洞**。

### 洞一（M148）：规则写在**用例名字**里，不等于它被测过

那条用例的标题一直写着「**2. 叠加：大字优先于框**」，
规则注释也写着「大字**优先于**框」。

但翻遍整个用例，**没有一条断言**让 `result` 和 `position_x`
**同时**出现：

- 测「有 PASS → 大字」的输出里**只有** `result`
- 测「有位置 → 框」的输出里**只有** `position_x`

于是「优先」这两个字**从来没被验证过**。

M148（把 `decideOverlay` 改成「框赢过大字」）→ **SURVIVED**。

> 这比「零违规不能证明规则在干活」还糟一格：
> 零违规至少承认「我没找到违规」，
> 而这一条**看起来**像「我验证过了」——
> 用例名字、断言红字、规则注释，**三处都在说这条规则存在**。

补上「两者都在」的输入之后，M148 **红了 5 条**。

配套卡：`notes/atomic/at-zero-violations-is-not-proof-the-rule-works.md`。

### 洞二（M154）：源码守卫会被**注释**喂饱

那条守卫本来是：

```cpp
check(src.contains("w7::planResultRender"), "...");
```

而 `mainwindow.cpp` 的**注释里**就写着这句话：

```cpp
//   现在两边都用 w7::planResultRender / w7::resultImageCandidates,
//   **必然一致**。详见 result_render_plan.h 开头。
const w7::RenderPlan plan = w7::planResultRender(out, fallbackPath);
```

M154 把**代码**那行换成 `w7::RenderPlan()`（面板彻底不用共享规则了），
**注释**那行还在 —— 守卫查的是 `src.contains(...)`，
而**注释也是 `src` 的一部分**。

> **守卫被一句注释喂饱了。**
> 变异把真调用删干净了，守卫照样绿。

修法：**查源码之前先剥掉注释**。
而且「剥注释」这个动作本身**要被测** ——
喂一段「注释里有那句话、字符串里有 `//`」的样本进去，
断言剥完之后那句话不见了、而字符串里的 `//` 还在。

> 这个项目有一句老话：「注释会骗人，断言不会。」
> 这次发现：**源码守卫也会被注释骗。**
> 骗人的不是「守卫」这个形式，是
> **「守卫查的东西」和「守卫要量的东西」不是一回事**。

配套卡：`notes/atomic/at-the-guard-can-be-fed-by-a-comment.md`。

## 5. 洞三：只收一半 = **病搬家了**（第九份副本）

第一版只把**候选列表**收成了一份。
「**从候选里挑哪个**」还是两份：

| | 挑法 |
|---|---|
| 判据侧 `imageState()` | 只看**第一**候选（不降级） |
| 显示侧 `mainwindow.cpp` 的候选循环 | 取第一个**存在的**（降级） |

两份靠什么对齐？靠「`Dropped` 那道闸在候选循环**之前**」——
**代码顺序**。

> **代码顺序也是规则，而且不会红。**
> 把规则藏进代码顺序，和把规则藏进注释，是同一类错误。

而且面板那份**会降级**，于是它**还会撒谎**：

> 「最终结果」那次调用**没有判据**（`imgState` 默认 `Available`），
> 判定节点的 `marked_path` 不在时，面板会一路退到
> **工具栏那张静态图** —— 一张和这一帧无关的图，配着这一帧的判定。

收法：抽一个**两边都调**的函数

```cpp
inline const ImageCandidate* pickBaseImage(const QVector<ImageCandidate>& cands,
                                           const ExistsFn& exists);
```

于是「判据说图在 ⟹ 面板显示的就是它」
不是靠人工对齐，是**同一份代码算出来的**。

> **「同一份代码」比「同一份规则」强一个量级。**
> 同一份规则还要人去对齐；同一份代码没有对齐这个环节。

顺带：`ExistsFn` / `fileExists` 从 `step_frame_cache.h` **搬到**规则旁边 ——
规则搬到哪儿，**能证明它的那把尺子**也得在哪儿。

配套卡：`notes/atomic/at-the-copies-had-already-drifted.md`（第九次）、
`notes/atomic/at-the-criterion-and-the-reality-must-be-the-same-thing.md`。

## 6. 有意改掉的行为（不是 bugfix，是修正）

| 场景 | 改之前 | 改之后 |
|---|---|---|
| 回看 `pcb_inspect` / `pcb_defect_judge` / `dl_infer` | 原图，或**工具栏那张静态图** | **它们自己产出的标注图** |
| 「最终结果」，判定图已不在 | **工具栏那张静态图** + PASS/FAIL 大字 | **「图没了」**（并说清原因） |
| 这一步压根不产图 | 「(该步不产生图像,见下方输出字段)」 | 同（现在**和「图没了」分开说**） |

第一条的理由：那才是「这一步干了什么」的证据。
缺陷标注图上有缺陷位置，原图上没有 ——
以前用户回看「PCB 缺陷判定」，**看不到缺陷在哪**。

第二条的理由：**宁可空着，并把原因写出来。**
用户看到「图没了」会去看日志；
看到一张别的帧的图，他只会得出一个错的结论。

第三条：这两件事**必须分开说** —— 混成一句就是在撒谎。

## 7. 欠账的状态

### 欠账 17：仍然**收一半**，但边界缩小了

原来欠账 17 的边界写的是「还没搬出去的：参数面板、
**结果面板的渲染（`renderOutputToResultPanel` 那一百多行）**、拖拽与连线交互」。

这一轮把结果面板的**决策部分**搬出去了：

| | 搬出去了吗 | 在哪 |
|---|---|---|
| 选哪张图（候选列表 + 取图规则） | ✅ | `result_render_plan.h` |
| 上面画什么（大字 / 框 / 无） | ✅ | `result_render_plan.h` |
| 出错时怎么办 | ✅ | `result_render_plan.h` |
| **画**（QPainter / QLabel） | ❌ | 还在 `mainwindow.cpp` |
| **接线**（点 Run 有没有反应、参数面板、拖拽连线） | ❌ | 还在 `mainwindow.cpp` |

判据跟片 3 拆 `run_engine.h` 一模一样：
**能不能在 QWidget 之外测它？**
「选哪张图」能 → 搬出去了；
「画」和「接线**接不到** QWidget 之外**（`QPainter` 那部分其实可以，
但它对用户没有价值 —— 画错了用户自己看得见，而**选错了图用户看不见**）。

> 这条边界本轮想清楚了，所以写下来：
> **搬出去的标准不是「能不能」，是「错了用户会不会自己发现」。**
> 画错了会自己发现（画面明显不对），
> 选错了**不会**（画面很正常，只是和数字对不上）——
> 后者才是必须搬出去、必须测的那一类。

### 欠账 19（新登记）：`PipelineGraph` 三个字段「写了没人读」

这一轮勘查时顺带发现的，**还没登记**，现在登记：

`PipelineGraph` 有三个字段：

- `skippedNodes`（加载时跳过了哪些未知算子）
- `skippedEdges`（因为端点缺失而跳过了哪些边）
- `defaultedSides`（哪些 pin 用了默认方向）

**三个全是「写了没人读」。**
`defaultedSides` 连**测试都没碰过**。

更要命的是它**连锁**：

> 未知算子被跳过 → 指向它的**边端点缺失** → **边也被跳过** →
> **链断成两截** → 下游拿**空输出** → 默认值兜底 →
> 数字正常 → 判 PASS。

这条比已登记的**欠账 18（加载期形状）**更严重：
欠账 18 说的是「跳过一个节点」，这条说的是「**跳过一个节点会让整条链断成两截**」。

边界：这条债**只关于**「跳过之后的链该怎么继续」。
它和欠账 18 的加载期形状**是同一个根因**（未知算子被跳过），
但**后果不同**（一个节点没了 vs 一条链断了），所以分开登记。

重开条件：和欠账 18 一起 —— 要定义「**画布忠实度**」的时候。
在那之前，`skippedEdges` 这个字段**至少要能被读出来**（先做到「不写没人读」）。

## 8. 这一轮的数字（实测，不是推算）

| | 结项那天（2026-10-24） | **现在（2026-10-25）** |
|---|---|---|
| W7 套件 / 断言 | 22 / 2152 | **23 / 2218** |
| 全仓套件 / 断言 | 25 / 2629 | **26 / 2695** |
| 累计变异 | 145 条（121 杀 / 23 活 / 1 编译期） | **163 条（135 杀 / 27 活 / 1 编译期）** |
| atomic 卡 | 61 | **63** |
| notes | 22 份 | **23 份**（+ atomic 共 ~772 KB） |
| C++ 文件 / 行数 | 123 / 37,861 | **125 / 38,882** |
| 欠账 | 17 条（已收 9） | **19 条（已收 9 / 未收 10）** |

**这一轮新增的变异**：M145~M162，**18 条，14 杀 4 活**，
`unexpected results = 0`。

那 4 条 SURVIVE **全是合法等价改写**：

| | 改了什么 | 为什么必须活 |
|---|---|---|
| M156 | `isEmpty()` → `size() == 0` | 同一个意思 |
| M157 | `QStringLiteral("PASS")` → `"PASS"` | 同一个意思 |
| M158 | 三元表达式多套一对括号 | 同一个意思 |
| M162 | `pickBaseImage` 改写成 early-return | 同一个意思 |

> **一条只会说「红」的规则是噪音发生器** ——
> 「不该红的时候不红」和「该红的时候红」，两个都要验。

**断言数怎么数的**（口径要能被别人复现）：

```powershell
& 'D:\Program Files\...\ctest.exe' --test-dir '<W7 build>' -C Release -V
# 每套件一行 "<n>: SUMMARY: pass=N fail=M"
# run_policy / detection_operators / template_match 三套件没有 SUMMARY 前缀,
#   它们的结尾是 "<n>: pass=N fail=M" —— 别漏掉这三套,否则会少算 236 条
```

## 9. 怎么复现这一轮

```powershell
# 编 + 跑新套件
& 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build 'E:\qianwie\00study\OpenCV\W7_VisionMasterLite\build' --config Release `
  --target w7_result_render_plan_test w7_run_test w7_vm_lite w7_test_data

# 全量 W7
& 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir 'E:\qianwie\00study\OpenCV\W7_VisionMasterLite\build' -C Release -V

# 变异第 17 轮
cmd /c "powershell -ExecutionPolicy Bypass -File E:\qianwie\00study\OpenCV\build\mut_round17.ps1 > E:\qianwie\00study\OpenCV\build\mut17_log.txt 2>&1"
```

第一遍的日志留在 `build/mut17_log_pass1.txt`（**那两条意外存活是证据，别删**）。

## 10. 这一轮改了什么

| 文件 | 改了什么 |
|---|---|
| `W7_VisionMasterLite/result_render_plan.h` | **新建**（266 行 → 收完取图函数后更长） |
| `W7_VisionMasterLite/tests/test_result_render_plan.cpp` | **新建**（6 个用例 / 62 条断言） |
| `W7_VisionMasterLite/step_frame_cache.h` | 删 `stepPrimaryImagePath`、删 `imagePath` 字段、谓词搬走、判据改用共享取图 |
| `W7_VisionMasterLite/mainwindow.cpp` | `renderOutputToResultPanel` 改用计划 + 共享取图 |
| `W7_VisionMasterLite/tests/test_run_policy.cpp` | `test_primary_image_path_priority` 重写 |
| `W7_VisionMasterLite/CMakeLists.txt` | 新套件目标 + `w7_add_test` |
| `notes/atomic/at-the-copies-had-already-drifted.md` | 追加第九次 |
| `notes/atomic/at-zero-violations-is-not-proof-the-rule-works.md` | 追加「规则写在用例名字里」 |
| `notes/atomic/at-the-criterion-and-the-reality-must-be-the-same-thing.md` | **新建** |
| `notes/atomic/at-the-guard-can-be-fed-by-a-comment.md` | **新建** |
| `notes/notes_W7_Knob_Audit_2026-10.md` | 欠账 17 边界改写 + **新登记 19** |

## 11. 参见

- `notes/notes_W7_BadStep_2026-10.md` —— 结项后还的第二笔（欠账 18 收一半）
- `notes/notes_W7_Verdict_2026-10.md` —— 结项后还的第一笔（欠账 17 收一半）
- `notes/notes_W7_Close_2026-10.md` —— 结项报告（欠账总账）
- `notes/notes_W7_Knob_Audit_2026-10.md` —— **欠账的完整定义只有这一处**（第十节）
- `notes/atomic/at-the-copies-had-already-drifted.md` —— 抄副本的九次现场
- `notes/atomic/at-the-criterion-and-the-reality-must-be-the-same-thing.md`
- `notes/atomic/at-the-guard-can-be-fed-by-a-comment.md`
- `notes/atomic/at-zero-violations-is-not-proof-the-rule-works.md`
- `notes/atomic/at-the-message-is-not-the-assertion.md` —— 断言声称的对象 vs 实际读的对象
