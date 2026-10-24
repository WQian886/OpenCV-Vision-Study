# W7 判定口径：一份规则，两边用

> 📅 2026-10-24（结项之后的第一笔还债）
> 🎯 欠账 17 收一半 + 抽掉本项目**第六份**「抄副本」
> 📋 这不是新功能片，是**还账**

---

## 0. 一句话

> 「跑完一条流水线，结论到底是什么」这件事以前有**两份**实现：
> GUI 那份（给人看）和 headless 那份（给 CI 看）。
> **它们已经漂了** —— 而且漂出来的那个结论是**漏检**。
> 现在规则收成**一份**（`frame_verdict.h`），两边都用它。

---

## 1. 起因：结项对账时顺手对了一件事

W7 结项（见 `notes/notes_W7_Close_2026-10.md`）留下一条欠账 17：

> **主窗口（`mainwindow.cpp`）的行为没有任何测试覆盖。**

结项报告里给的边界是：引擎语义已经搬出去了（`run_engine.h`），
`mainwindow.cpp` 现在只剩**纯 UI 接线**，所以剩下的风险面比想象中小 ——
**但它确实是零覆盖**。

还债的第一步是**勘查**：那 1942 行里，还有哪些是**有真实语义**的纯逻辑？

`runPipeline`（512 行）里，片 2 / 片 3 搬走的是**执行**的部分。
剩下没搬走的那一大块里，有一段是纯逻辑、零 QWidget、而且**有真实语义**：

```cpp
// 每一帧结束后
const QString r = frameResult.value("result").toString();
if (r == "PASS")      ++passCount;
else if (r == "FAIL") ++failCount;
++frameDone;
...
// 最后的结论
QString result = finalResult.value("result").toString();
if (result.isEmpty()) result = "UNKNOWN";
```

**这就是「结论怎么说」。** 产线判废看的就是这个。

而这段逻辑，在 `headless_runner.cpp` 里**还有一份**。

---

## 2. 两份判定，两份规则 —— 而且已经漂了

两份实现长得几乎一模一样（这不是巧合，是抄的）。
但对账的时候，发现它们**已经不是同一件事**了：

| 规则 | headless（CI 看的那份） | GUI（人看的那份） |
|---|---|---|
| 一帧里有步骤报错 → 这一帧**不算 PASS 也不算 FAIL** | ✅ 有 | ❌ **没有** |
| 整条流水线有步骤报错 → 最终结果**强制 ERROR** | ✅ 有 | ❌ **没有** |
| 整批判定看**哪一帧** | 末帧 | **任意一帧** |

第三条最要命。下面单说。

### 2.1 前两条：GUI 少了两条规则

具体后果：

```
一条流水线：pcb_inspect（读不到配方文件，报了个错）→ pcb_defect_judge（判 PASS）
```

| 谁 | 说什么 |
|---|---|
| GUI | 「🎉 最终结果: PASS」 |
| headless | `final_result: "ERROR"`，退出码 **3** |

**两份都说得很有底气，结论却相反。**

而**产线上判废看的是 GUI 那份** —— 所以它是错的那一份。

### 2.2 第三条：CI 那个结论是**漏检**

这条是这一轮最值钱的发现，而且**不是两条规则的漂移，是第三份推导**：

```cpp
// headless_cli.cpp（改之前）
if (res.errorCount > 0) { ...; return 3; }
if (res.finalResult == QStringLiteral("FAIL")) return 1;   // ← 这里
return 0;
```

`res.finalResult` 是**末帧**的判定。于是：

```
30 帧抽检，第 7 帧判废，第 8~30 帧都正常
  → finalResult = "PASS"（末帧说的）
  → 退出码 0
  → CI 说「过了」
而同一批,主程序那边说:
  → 「⚠️ 有 1 帧判废,整批判 NG」
```

**两个结论，CI 那个是漏检。**

而主程序那边的注释其实早就写对了：

> 只看最后一帧的 PASS/FAIL 是不够的：抽检 30 帧里 1 帧有问题，
> 末帧正常的话单看末帧就是漏检。**产线的判废看的是整体，不是抽一张。**

> ⚠ 这正是本项目栽过**五遍**的「抄副本」，第六次 ——
> 而且**已经漂了**。
> 详见 `notes/atomic/at-the-copies-had-already-drifted.md`（本轮的实例搬进了那张卡）。

---

## 3. 为什么以前没人发现

因为**两边都是绿的**。

- GUI 那份：跑得好好的，从不崩，状态点也是绿的；
- headless 那份：CI 全绿，四种退出码都对。

而 `mainwindow.cpp` **不在任何测试目标里**（它要真窗口 + 整套算子），
所以「两份该一致」这件事**从来没有被任何东西检查过**。

> 这是欠账 17 的真实代价：
> **不是「主程序没有测试」，是「主程序和 CI 各有一套判定，而没人比对过」。**
>
> 而**没有比对，就没有发现**。

---

## 4. 收法：抽成一份，两边都用

### 4.1 新文件 `W7_VisionMasterLite/frame_verdict.h`

**纯 QtCore，零 OpenCV、零 QtWidgets**（和 `run_engine.h` 同一个标准），
所以依赖它的测试目标建得快，也顺便证明了
「判定逻辑不需要拖整个 GUI 进来」。

它编码的东西，一共四条规则：

| 规则 | 在哪 |
|---|---|
| 一帧的四种判定：Pass / Fail / **Error**（跑坏了）/ Unknown | `classifyFrame` |
| 逐帧累计 | `FrameTally::addFrame` |
| 整批结论：Cancelled > Errored > Failed > Passed | `classifyBatch` |
| 最后结果那个字符串：末帧判定 vs 整批结论 | `lastResultOf` / `finalResultOf` |

还有一条「取哪个节点的输出当这一帧的判定」——
以前两边也各写了一遍（`pickFrameResult`），现在也收进来了。

### 4.2 两个调用方各自怎么改

**GUI（`mainwindow.cpp`）**
- 多了 `int frameErrors` —— **以前压根没有这个概念**，
  所以坏帧和通过帧在它眼里长得一模一样；
- 多帧汇总走 `classifyBatch`，
  新增了一条 `Errored` 分支：**有坏帧时不再说「全批 N 帧判 OK」**；
- 最终结果走 `finalResultOf` —— 有错时从「PASS」变成「ERROR」。

**headless（`headless_runner.cpp` / `headless_cli.cpp`）**
- 逐帧累计改用 `FrameTally`，判定口径不再自己写；
- `HeadlessResult` 多了一个 `batchVerdict` 字段，**由 `classifyBatch` 算好**；
- CLI 的退出码**只准读** `res.batchVerdict`，不准自己再推一遍；
- 结果 JSON 多了一个 `batch_verdict` 字段
  （`final_result` 是末帧判定，`batch_verdict` 是整批结论 —— **两个不是一回事**）。

> ⚠ 有一条**特意没改**：判「步骤报错」的判据仍是
> 「**输出里带 error**」，不是「引擎报 failed」。
> 抽规则是为了让两边**一致**，不是为了重新设计口径 ——
> 改口径是另一次改动，得连着改测试和两边。
> 详见第 7.1 节。

### 4.3 「要不要显示汇总」和「这一批是什么结论」是**两件事**

写代码时差点混在一起：

- 主程序只在**多帧**时显示「批量汇总」；
- 但**一帧**的批次也要有结论（`Failed` / `Errored` / `Passed`）。

所以 `classifyBatch` **不看总帧数**（判据是「有没有帧判废 / 有没有帧跑坏」），
「多帧才显示」留在主程序那边当**展示**规则。

> 这两个混在一起，就会长出「一帧的批次没有结论」这种洞。

---

## 5. 测到了什么

### 5.1 新套件 `frame_verdict`（69 条）

`tests/test_frame_verdict.cpp`，**纯 QtCore**，建得快、不需要显示器。

| 用例 | 测什么 |
|---|---|
| 1 | `classifyFrame`：四种判定 + **判据的顺序不能换** |
| 2 | `FrameTally`：坏帧不计入 pass/fail；`lastResult` 是**末帧**不是「最后一个有判定的」 |
| 3 | `finalResultOf`：**有错强制 ERROR**（GUI 以前缺的那条） |
| 4 | `classifyBatch`：优先级 Cancelled > Errored > Failed > Passed |
| 5 | `pickFrameResult`：取**最后一个**带 result 的；一个都没有时的兜底；越界下标 |
| 6 | **源码守卫**：两个调用方都必须走这一份 |

### 5.2 headless 补 12 条（117 → 129）

新增用例「7b. 整批结论：末帧正常**不代表**整批正常」：

- 3 帧、第 2 帧判废、末帧 PASS → `batch_verdict = FAIL`；
- 三帧全 PASS → `PASS`；
- 有一步报错 → `ERROR`（不是 FAIL，更不是 PASS）；
- 一帧判废 → 也是 `FAIL`（判据是「有没有帧判废」，不是「有几帧」）。

### 5.3 源码守卫：守住「两边都走这一份」

用例 6 是 lint，但它守的正是这一轮的全部意义：

```
mainwindow.cpp 和 headless_runner.cpp 都必须:
  · 引 frame_verdict.h
  · 走 w7::finalResultOf / w7::classifyBatch / w7::pickFrameResult
  · 不许再出现自己写的 `if (r == "PASS")` 那种计数循环
headless_cli.cpp:
  · 必须读 res.batchVerdict
  · 不许再拿 res.finalResult 当整批结论
```

> 判据是「**要求某个可测入口**」，不是「禁止某种写法」——
> 后者会被等价改写绕过。

---

## 6. 变异第 15 轮：M116~M133（18 条，15 杀 3 活）

`unexpected results = 0`。

| 变异 | 改了什么 | 判定 |
|---|---|---|
| M116 | `classifyFrame` 里两个判据**调换顺序** | KILL（8 条红）|
| M117 | 坏帧又算回 PASS | KILL（7 条红）|
| M118 | 最终结果不再强制 ERROR | KILL（3 条红）|
| M119 | 有坏帧不再算「不成结论」 | KILL |
| M120 | **有帧判废不再算 NG**（就是 CI 那个漏检） | KILL |
| M121 | Errored 不再赢过 Failed | KILL |
| M122 | Cancelled 不再赢过其它 | KILL |
| M123 | 取**第一个**带 result 的而不是最后一个 | KILL |
| M124 | 「没有 result 就退到最后一个」的兜底没了 | KILL |
| M125 | `lastResult` 记成「最后一个**有判定的**」 | KILL |
| M126 | 末帧没判定不再是 UNKNOWN | KILL |
| M127 | 坏批的名字不再是 ERROR | KILL |
| **M128** | **CLI 退回用末帧判定**（把漏检放回去） | KILL |
| **M129** | **GUI 不再数坏帧**（把空头 PASS 放回去） | KILL |
| **M130** | **GUI 不再强制 ERROR**（把空头 PASS 放回去） | KILL |
| M131~M133 | 三条**合法等价改写** | SURVIVE（必须活）|

M128 / M129 / M130 是这一轮最要紧的三条 ——
**它们把本轮找到的三个 bug 原样放回去**，然后证明测试真的抓得住。

### 6.1 这轮自己栽了一次（值得记）

第一遍跑出来 `unexpected results = 2`（M122、M124 存活）。

原因不是规则没被钉住，是**我自己的变异写错了**：
那两条的**改动和描述说的不是一回事** ——

- M122 描述是「Cancelled 不再赢过其它」，
  我实际写成了 `cancelled == true`（**等价改写**，当然活）；
- M124 描述是「兜底没了」，
  我实际写成了「两个操作数对调」（**还是等价改写**）。

> 起因是我先写成了 `cancelled && false` 和 `false && ...`，
> 怕 MSVC 报 C4127（常量条件）——
> 于是换成了等价写法，**却没把期望从 KILL 改成 SURVIVE**。
>
> 这正是「一个变异只能声明一种预期」这条规矩在起作用：
> **声明和实际不符时，尺子会响。**
>
> 改对之后**整轮重跑**，不留半轮的数据。

---

## 7. 顺带查清的两件事

### 7.1 「算子压根没注册」那一步算什么（小缝，已登记成欠账 18）

引擎里一步失败（`info.failed`）时，**输出是空的** ——
所以「输出里带 error」那条判据数不到它。

结果：一个节点压根跑不了（存盘里写了不存在的算子名），
它**不算坏帧**，链继续往下跑，下游照样可能判 PASS。

- **边界**：这不是漂移 —— **两边一致**（都不数）。
  但两边都**漏**了。
  而在生产里它几乎不会发生：17 个算子全注册在工厂里，
  只有手改过的 / 老版本的 `.sol.json` 才可能写出一个不存在的算子名。
- **重开条件**：要改「未知算子」的处理方式时（现在是**跳过并记一条日志**，
  `graphFromJson` 干的事）—— 那时必须同时决定
  「跳过的东西算不算坏帧」，并且**两边一起改**。
- 现在登记为**欠账 18**，边界和重开条件写在
  `notes/notes_W7_Knob_Audit_2026-10.md` 第十节。

### 7.2 上一次记的断言数复现不出来

这一轮要更新 README 的数字，于是把全套件重数了一遍：

| | 笔记里记的 | **实测** |
|---|---|---|
| W7 套件 / 断言 | 21 / 2048 | **22 / 2120** |
| W6 套件 / 断言 | 3 / 477 | 3 / 477 ✓ |
| 全仓 | 24 / 2525 | **25 / 2597** |

本轮新增 81 条（69 + 12），所以**改之前的**实测值是 **2039**，比笔记里的 2048 少 9。

- `camera` 套件：笔记记 **90**，实测 **93**（干净目录和工作目录都一样）——
  这解释了 3 条；
- 剩下 **6 条没找出来**。测试源码从片 5 之后**一个字节没动过**（`git log` 查过），
  所以那 6 条**从片 5 记数字那天起就不对**。

> **查不清就写清楚查不清。**
> 跟上次那个「33,088 行」是同一族：
> 数字本身带着一个精确的整数，看起来完全可信，
> 而**没有一条命令能复现它**。

顺带一个方法上的收获：

> **断言数只能运行时数，源码里的 `check(` 次数不算。**
> 实测：`test_camera.cpp` 源码里 80 个 `check(`，运行时 93 条；
> `test_run_policy.cpp` 160 个，运行时 162；
> `test_headless_runner.cpp` 131 个，运行时 129。
> （有在循环里的，有在 helper 里被调多次的。）
>
> 所以口径是：**逐个 ctest 套件跑一遍，把 `SUMMARY: pass=` 加总**。
> ⚠ 三个套件（`run_policy` / `detection_operators` / `template_match`）
> 的结尾**没有** `SUMMARY:` 前缀，只有 `pass=… fail=…` —— 只按一种格式统计会少算。

### 7.3 顺带一提：这一轮我自己也漏了一次产物

统计断言数时，我直接跑套件 exe，没切工作目录：

```powershell
cmd /c "…\w7_camera_test.exe > out.txt 2>&1"     # ← CWD 是仓库根
```

结果那些套件的产物（`dl_in.png` / `dl_model*.onnx` / `images/_srcframe_*.png` …）
**直接写进了仓库根** —— 也就是它们 2026-10-20 之前待过的地方。

`git add -A` 的时候它们差点被提交进去（我在 `git diff --cached --stat` 里看见了）。
删掉了，**没有加 .gitignore 规则** —— 那个项目 2026-10-20 刚把两条
「只为藏测试产物」的规则删掉，理由是：
**「规则是用来藏问题的，不是用来解决问题的」**。

> ⚠ 顺带发现 CI 第 6 步的一个**盲区**：
> 它比的是 before/after 两个快照 ——
> **只查「这一轮新漏的」，查不出「早就漏在那儿的」。**
>
> 而手动跑套件**恰恰是最容易漏的**（CI 自己会切工作目录，人手动跑经常忘）。
>
> 真正的哨兵是 preflight 那行：
> ```
> working tree before: 0 uncommitted entr(y/ies)
> ```
> **那不是装饰 —— 那是「跑之前源码树是干净的」的断言。**
> 手动跑完套件也该看一眼它。

---

## 8. 这一轮的数字（实测，不是推算）

| | 数 |
|---|---|
| 新套件 | `frame_verdict` **69** 条 |
| headless 套件 | 117 → **129**（+12）|
| W7 套件 / 断言 | **22 / 2120** |
| W6 套件 / 断言 | 3 / 477 |
| 全仓套件 / 断言 | **25 / 2597** |
| 变异 | **134** 条（113 杀 / 20 活 / 1 编译期）|
| C++ 文件 / 行数 | **123 / 37,614** |

---

## 9. 欠账 17 现在的状态：**收一半**

| | |
|---|---|
| ✅ **收掉** | 「结论怎么说」这一整块 —— 判定口径一份、两边共用、有行为测试、有变异钉着 |
| ❌ **还在** | `mainwindow.cpp` 剩下的**纯 UI 接线**：参数面板、结果面板的渲染、拖拽与连线交互、`renderOutputToResultPanel` 那一百多行 |

**重开条件没变**（见欠账 17 的登记处）：
要把 UI 接线也拆出来时，判据仍然是
「**能不能在 QWidget 之外测它**」。

> 诚实的说法是：
> **欠账 17 从「零覆盖」变成「判定那块有覆盖、接线那块没有」。**
> 不是收完了，是**从一个洞变成两个小得多的洞**。

---

## 10. 参见

- `notes/notes_W7_Close_2026-10.md` —— 结项报告（总账、欠账摘要、怎么继续）
- `notes/notes_W7_Knob_Audit_2026-10.md` 第十节 —— **欠账清单的唯一登记处**
- `notes/atomic/at-the-copies-had-already-drifted.md` —— 本轮的实例搬进了那张卡（第六次）
- `notes/atomic/at-the-last-frame-is-not-the-batch.md` —— 新卡：末帧判定 ≠ 整批判定
- `notes/atomic/at-build-failure-looks-like-mutation-survived.md` —— BUILD-FAIL 是第三种判定
- `notes/atomic/at-zero-violations-is-not-proof-the-rule-works.md` —— 零违规 ≠ 规则在干活
