# W7：帧协议收成一份 + 纯层瘦身（欠账 17 的第三半）

> 2026-10-25 · 旋钮审计结项之后的**第四笔**
> 上一篇：`notes/notes_W7_RenderPlan_2026-10.md`（欠账 17 再收一半）

## 0. 一句话

这一轮做三件事，落在同一个原则上 —— **纯层只放纯的**：

1. 「源怎么报帧数」这份协议，以前在 GUI 和 headless **各写一遍** ——
   本项目栽过**九遍**的「抄副本」的**第十遍**。收成 `w7::resolveFrameCount()`。
2. `run_policy.h`（纯策略层）为了一个 widget 调用拖了 `<QPlainTextEdit>`，
   拖掉之后**没有任何东西拦着下一个人再拖一个**。搬到 `log_view.h`。
3. 于是给它加一道**编译期**的闸：一个**只链 `Qt6::Core`** 的目标 include 全部纯头文件 ——
   往纯层塞一个 QWidget，那个目标**编不过**。
   这是本项目**第一条不用 lint 守的边界**。

第 3 条是这一轮真正的新东西。变异 M169 就是它的工作现场：
**闸的红不是断言红，是编译器拒绝构建。**

## 1. 起因：第十次抄副本

### 1.1 那两份

```cpp
// mainwindow.cpp :: MainWindow::runPipeline
for (const QJsonObject& out : frameOutputs) {
    if (out.contains("frame_count")) {
        const int reported = out.value("frame_count").toInt(1);
        bool truncated = false;
        totalFrames = w7::clampFrameCount(reported, &truncated);
        ...
        break;
    }
}

// headless_runner.cpp :: runPipelineJson
for (const QJsonObject& o : ef.outputs) {
    if (o.contains("frame_count")) continue;
    const int reported = o.value("frame_count").toInt(1);
    bool truncated = false;
    totalFrames = clampFrameCount(reported, &truncated);
    ...
    break;
}
```

规则其实只有**三条**：

1. 源在**输出**里报一个 `frame_count` 字段；
2. **第一个**带这个字段的输出说了算（按执行序）；
3. 报出来的数要过 `clampFrameCount`（小于 1 当 1，超过上限截断并报出来）。

三条规则、两份实现、**零行注释提过这件事**。

### 1.2 这次特别的地方：两份**碰巧**没漂

前十次里，有七次是「抄的时候一模一样，然后真的漂了」。
这一次**还没漂** —— 两边现在行为完全一致。

但「还没漂」不是「不会漂」：

- 两份之间**没有任何东西**保证它们一致；
- 哪天源算子改了 `frame_count` 的语义（比如从「总帧数」改成「剩余帧数」），
  两边会**各按各的理解**算；
- 而**两边都是绿的**。

> **「还没出事」和「不会出事」之间，隔着的正是这条规则。**
> 本项目记过的那句话又成立了一次：
> 「两份一模一样的东西就是两份会各自漂的东西。」

### 1.3 为什么它不显眼

因为它**藏在两个字里**：`frame_count`。

这个字段名在 `run_limits.h`、`mainwindow.cpp`、`headless_runner.cpp`、
还有各个源算子里都出现。任何一次 grep 都会搜到它，
所以它**看起来像是一份**。

而实际上「哪一帧的输出算第一个」这件事，
只有那份**循环**说了算 —— 而循环有两份。

## 2. 收法 A：帧协议收成一份

放在 `run_limits.h`（不是新开一个文件）：

```cpp
inline int resolveFrameCount(const QVector<QJsonObject>& outputs,
                             int* reportedOut = nullptr,
                             bool* truncated = nullptr);
```

### 2.1 为什么不新开文件

这个头文件**已经**是「帧数相关」的家（`kMaxRunFrames` / `clampFrameCount` 在里面），
而且**两个调用方都已经够到它**了。
新开一个 `frame_protocol.h` 只会多一个文件、多一处 include，
却不会让协议更容易找到。

> 收成的目标不是「多一个文件」，是**少一份实现**。

### 2.2 契约从「纯算术」改成「纯 QtCore」

`run_limits.h` 的注释以前写着「纯算术(QtCore 甚至不依赖)」。
协议是**数据形状**不是算术 —— 它要看 `QJsonObject`。

所以契约改成「**纯 QtCore**」，
并明确写清「**仍然不碰 QtWidgets、不碰 OpenCV**」。

这是本项目的老规矩：**类型/依赖不对 → 改契约，不糊弄**。
硬撑「纯 C++」只能靠不检查 `QJsonObject`，
而那等于**把协议写成字符串匹配** —— 那才是真的糊弄。

### 2.3 三条设计决定

| 决定 | 为什么 |
|---|---|
| **第一个**说了算（不是最大的、不是最后一个） | 输出数组按执行序排；「第一个」是**可复现**的定义。变异 M163 专门打这一条（改成最后一个 → 杀） |
| `reportedOut` 给**原始**值，不是截断后的 | 截断提示要说的是「**一共有多少帧**」，不是「我们跑了多少帧」。变异 M166 专门打这一条（→ 杀） |
| 没报帧数 → 当 1 帧 | 循环至少进一次，否则用户看到「Run 没反应」。变异 M165 打「0 不兜底」（→ 杀） |

> 「第一个」这个选择看起来随便，其实不是：
> 「取最大的」要求**所有**输出都可比，
> 「取最后一个」要求执行序**恰好**和数组序一致，
> 只有「取第一个」在**任何**顺序语义下都稳定。

## 3. 收法 B：widget 依赖隔离到一个文件

### 3.1 发现方式

不是「我觉得 run_policy.h 不干净」——
是**给纯层加编译期闸的时候撞上的**：

```
error C1083: 无法打开包括文件: "QPlainTextEdit": No such file or directory
```

这个错误在说一件很具体的事：
`run_policy.h` 拖的 `<QPlainTextEdit>`，**在纯层里根本不该存在**。

### 3.2 那个 widget 调用有多小

```cpp
inline void configureLogView(QPlainTextEdit* w, int cap = kLogMaxLines) {
    if (!w) return;
    w->setReadOnly(true);
    w->document()->setMaximumBlockCount(cap);
}
```

三行。为这三行，整个「纯策略层」拖进了 QtWidgets。

### 3.3 搬到 log_view.h

新建 `W7_VisionMasterLite/log_view.h`：

- `kLogMaxLines = 5000`
- `configureLogView(QPlainTextEdit*)`

于是：

| 文件 | 依赖 |
|---|---|
| `run_limits.h` | 纯 QtCore（帧数封顶 + 帧协议） |
| `run_policy.h` | 纯 QtCore（重绘限频、取消、重入） |
| `log_view.h` | **这里** —— 唯一和 `QPlainTextEdit` 有关系的地方 |

> **搬走不是删掉。**
> 规则（`kLogMaxLines = 5000`）还在，只是不再住在纯层里。
> 而且搬的时候连**理由**一起搬了 —— `log_view.h` 开头那段注释
> 解释了「为什么这两块住在 run_policy.h 是个错误」，
> 而 `run_policy.h` 原来那段解释被换成了「它们搬走了，见 log_view.h」。

### 3.4 顺手搬走一个**测试工具**的副本

写新套件时发现 `stripComments` 已经有一份了 ——
在 `test_result_render_plan.cpp` 里。

那本来是**第十一份**副本。
所以把它抽成 `tests/source_scan.h`（`readSource` / `stripComments` / `includeDirectives`），
两个套件共用。

> 「抄副本」不只发生在产品代码里，**发生在测试代码里**。
> 而测试代码里的副本更危险 ——
> 它让人以为「这个工具已经标准化了」，
> 于是下一个人**再抄一遍**。

## 4. 收法 C：编译期分层约束（本项目第一条不用 lint 守的边界）

### 4.1 边界以前只写在注释里

- `run_limits.h` 自己写着「纯算术」
- `run_policy.h` 自己写着「纯逻辑」
- `pipeline_graph.h` 自己写着「**零 OpenCV、零 QtWidgets**」

然后 `run_policy.h` 自己就拖了个 `<QPlainTextEdit>`。
**两边都是绿的。**

### 4.2 lint 守不住这条边界

本项目用源码 lint 守了很多年，判据全是「文本里有没有某个东西」：

```cpp
check(!src.contains("opencv2"), "...");
```

它有三个已知的漏法，**本项目全栽过**：

| 漏法 | 现场 |
|---|---|
| 注释喂饱它 | `contains("w7::planResultRender")` 被注释满足 —— 变异 M154 把真调用删干净了，守卫照样绿 |
| 等价改写绕过 | `out.value("mask_path")` → `out["mask_path"]`，同一个意思，lint 看不见 |
| 搬家不红 | 规则从查这个字符串搬到查那个字符串，两条 lint 都还绿 |

**编译器没有这三个漏法** —— 它查的是**依赖**，不是文本。

### 4.3 怎么交给编译器

```cmake
qt_add_executable(w7_pure_test tests/test_pure_layer.cpp)
target_link_libraries(w7_pure_test PRIVATE Qt6::Core)   # 只有 Core
```

```cpp
#include "run_limits.h"          // 帧数封顶 + 帧协议
#include "run_policy.h"           // 重绘限频 / 取消 / 重入
#include "frame_verdict.h"        // 判定口径
#include "run_engine.h"           // 拓扑序 + 逐节点执行
#include "pipeline_graph.h"       // 读盘的纯解析层
#include "result_render_plan.h"   // 结果面板「显示哪张图」
#include "step_frame_cache.h"     // 多帧缓存
#include "frame_record.h"         // 帧接线
#include "headless_runner.h"      // 无头跑一条流水线
```

后果：**任何一处纯头文件里多了一个 QWidget / opencv2，那个目标就编不过。**

边界从此不是「文档里的一句承诺」，而是**构建会拒绝的东西**。

### 4.4 实测（M169 的现场）

往 `run_policy.h` 里塞回 `#include <QPlainTextEdit>`，然后编 `w7_pure_test`：

```
run_policy.h(41,10): error C1083: 无法打开包括文件: "QPlainTextEdit": No such file or directory
```

那个目标只链 `Qt6::Core`，**QtWidgets 的头根本找不到**。

> 这就是「闸不是红断言，是编译器拒绝构建」的**字面意思**。

### 4.5 ⚠ 两个坑（都补上了）

**坑一：闸本身也会被拆掉。**

有人往那个 target 的 `target_link_libraries` 里加上 `Qt6::Widgets`，闸就没了 ——
而套件**照样全绿**（它只是又拖进来一个 widget 而已）。

所以用例 5 守着**闸的形状**：

```cpp
check(!cleaned.contains("Qt6::Widgets"), "w7_pure_test **没有**链 Qt6::Widgets(那会把闸拆掉)");
check(!cleaned.contains("OpenCV_LIBS"), "w7_pure_test **没有**链 OpenCV(那会把闸拆掉)");
```

> **守卫自己被改掉**，也是「抄副本」的一种 ——
> 而且是最隐蔽的一种，因为它改的是**闸**不是**被闸守的东西**。

**坑二：失败发生在构建那一步，不是测试那一步。**

破坏分层时，第一反应容易是「环境坏了 / 依赖装坏了」。
所以配了一条**读得懂**的守卫（用例 4）把同一件事用中文再说一遍，
让下一个人第一反应变成「哦，是我把 widget 拖进纯层了」。

用例 4 的判据是**指令行**，不是文本：
`includeDirectives` 只认真正开头的 `#include`，注释里写一行 `#include <QWidget>` 不算数。

而且它**引号形式也查**：

```cpp
const QStringList guiOwningHeaders = {
    "mainwindow.h", "operator_node.h", "connection_line.h",
    "pipeline_view.h", "pipeline_serialize.h", "select_delete_plan.h",
    "param_form.h", "operator_base.h", "operator_defaults.h",
    "dl_postprocess.h", "log_view.h",
};
```

> `#include "mainwindow.h"` 拖的 widget 比 `<QWidget>` 还多 ——
> 它把整个画布、算子库、信号槽都拖进来。

## 5. 变异第 18 轮：M163~M172

**10 条 = 7 杀 + 1 编译期 + 2 活**，`unexpected results = 0`。

| 变异 | 打的是什么 | 预期 | 实测 |
|---|---|---|---|
| M163 | 「第一个」改成「最后一个」 | KILL | **KILLED** |
| M164 | 不再 clamp | KILL | **KILLED** |
| M165 | 0 不再兜底到 1 | KILL | **KILLED** |
| M166 | `reportedOut` 报截断后的数 | KILL | **KILLED** |
| M167 | GUI 改回自己写协议 | KILL | **KILLED**（2 条红） |
| M168 | headless 改回自己写协议 | KILL | **KILLED**（2 条红） |
| M169 | `run_policy.h` 又拖 `<QPlainTextEdit>` | **BUILD-FAIL** | **BUILD FAILED** |
| M170 | 日志文档不再封顶 | KILL | **KILLED**（run_policy 套件，4 条红） |
| M171 | 等价改写：`< 1` → `<= 0` | SURVIVE | **SURVIVED** |
| M172 | 等价改写：`QStringLiteral` → `QLatin1String` | SURVIVE | **SURVIVED** |

### 5.1 M169 是这一轮的**主角**

它是本项目**第一条编译期判定**：

```
=== M169 (pure_layer, expect BUILD-FAIL): gate: QPlainTextEdit is dragged back into the pure policy header
  BUILD FAILED -- [as expected]
```

> **BUILD-FAIL ≠ 变异存活。**
> 这是第三种判定 —— 不是「杀」，也不是「活」，
> 而是「**这条边界根本不该由断言来守**」。
>
> 前 17 轮里，所有边界都是用「一条红断言」守的。
> 这一轮开始，有些边界可以交给**编译器**。

### 5.2 两条 SURVIVE 的意义

规则**宁窄勿宽**，但也不能窄到只认一种写法：

- M171（`< 1` → `<= 0`）：语义等价，**必须活**；
- M172（`QStringLiteral` → `QLatin1String`）：语义等价，**必须活**。

> 只会说红的规则是**噪音发生器** ——
> 它会把「等价改写」也当成违规，于是下一个人开始绕过它。
> 那条 SURVIVE 是**校准尺子**用的。

### 5.3 脚本本身也可靠

这一轮的脚本有个**关键性质**：`Apply-Mutation` 在锚点匹配数 ≠ 1 时会**抛异常**，
记成 `SETUP-ERROR` 并**计入 `unexpected results`**。

所以 `unexpected results = 0` 不只是「10 条都符合预期」，
还意味着**每条变异的锚点都精确命中一次** ——
没有一条是「锚点没匹配上 → 什么都没改 → 假性 SURVIVE」。

> 变异轮次里，**假阳性的 SURVIVE 比假阴性的 KILL 更危险**：
> 它会让你以为规则被覆盖了，于是**放松**守卫。

## 6. 这一轮新增的套件

`W7_VisionMasterLite/tests/test_pure_layer.cpp`（**33 条**，5 个用例）：

| 用例 | 测什么 |
|---|---|
| 1 | `resolveFrameCount` 的行为（空 / 没字段 / 第一个赢 / 0 / 负数 / 非数字 / 超上限 / 原始值 / 刚好等于上限） |
| 2 | GUI 和 headless **都**走共享协议（剥注释后查） |
| 3 | widget 依赖隔离到 `log_view.h`；mainwindow 必须调 `w7::configureLogView` |
| 4 | 纯头文件的 include 指令里不许有 widget / OpenCV / GUI-owning 头 |
| 5 | **守住编译期闸自己的形状**（`w7_pure_test` 只准链 `Qt6::Core`） |

用例 2 的判据是**「要求某个可测入口」**，不是「禁止某种写法」：

```cpp
check(code.contains("w7::resolveFrameCount"), "mainwindow.cpp 走的是 w7::resolveFrameCount(...)");
check(!code.contains("contains(\"frame_count\")"), "mainwindow.cpp 里没有自己写的 \"frame_count\" 判据");
```

而且查的是**剥掉注释之后**的代码（`stripComments`）——
上一轮的 M154 就是被一句注释喂饱的。

## 7. 数字

| 项 | 结项时（`33dd280`） | 本轮之后 |
|---|---|---|
| W7 套件 / 断言 | 23 / **2230** | **24 / 2263** |
| W6 套件 / 断言 | 3 / 477 | 3 / 477 |
| 全仓套件 / 断言 | 26 / **2707** | **27 / 2740** |
| 累计变异 | 163（135 杀 / 27 活 / 1 编译期） | **173（142 杀 / 29 活 / 2 编译期）** |
| atomic 卡 | 63 | **64** |
| notes 份数 | 23（~772 KB） | **24（~808 KB）** |
| C++ 文件 / 行数 | 125 / 38,882 | **128 / 39,453** |

> ⚠ **勘误（2026-10-25，第五笔之后）**：
> 这一表原先写的是 **23 / 2218** 和 **27 / 2728** ——
> **两个断言数都低了 12**。
>
> 根因：**本地 build 树里的 `w7_engine_test.exe` 是旧的** ——
> 前一轮只编了部分目标，没编它，它停在 10-23 的版本（比源码少 12 条断言），
> 而**总数看着完全正常**。CI 树（权威，从干净树重编）实测 2263 证实。
>
> 这一条**不改历史提交**（沿用本项目先例：README 那个行数也是
> 「当场改 + 记下怎么发现的」），只在这里**当场改 + 记下怎么发现的**。
> 详见 `notes/atomic/at-ninja-mtime-stale-binary.md`。
>
> 顺带记一条教训：**本地 build 树不是权威，CI 树才是**。
> 修法是给 CI 加了「数断言」那一步（只打印 + 落文件，不硬编码期望值）——
> 让下一次「漂了」**看得见**。

口径（拿别人的命令能跑出同一个数）：

```powershell
# 套件 / 断言：跑一遍 ctest -V，把每个套件的 pass= 加总
# 变异战绩：每轮脚本结尾的 `unexpected results = N`（必须为 0）
# 行数：
$files = git ls-files '*.cpp' '*.h'
$total = 0
$files | ForEach-Object { $total += (Get-Content -Encoding UTF8 -LiteralPath $_).Count }
```

## 8. 欠账登记

### 欠账 17（主窗口零覆盖）—— 又收了一截

边界改写：

> 剩**三块**：
> ① **引擎语义** —— 拓扑序 + 多前驱合并（`run_engine.h`）✅ 已收
> ② **判定口径** —— PASS/FAIL/ERROR 怎么说（`frame_verdict.h`）✅ 已收
> ③ **显示规则** —— 结果面板显示哪张图（`result_render_plan.h`）✅ 已收
> ④ **帧协议** —— 源怎么报帧数（`run_limits.h :: resolveFrameCount`）✅ **本轮已收**
> ⑤ **UI 接线** —— `mainwindow.cpp` 里真正连 widget 的那部分 ⬜ **还欠着**
>
> ⑤ 为什么还欠着：主窗口**没法无头构造**
> （要真窗口 + 全部算子 + 画布）。
> 现在的做法是「把纯逻辑搬出去测，接线部分用源码守卫盯」——
> 这套办法到 ④ 为止都成立，
> 到 ⑤ 就到头了：**源码守卫看不见「信号连到了哪个槽」**。

**⑤ 的下一步**：把接线部分上 `QTest`（offscreen 平台）。
本机能干，不需要真 GPU、不需要真 SDK。

### 欠账 18（加载期形状）—— 状态不变

运行期那个已收（`w7::isBadStep`）。
剩下**加载期**那个：存档里写了不存在的算子名，`graphFromJson` 直接跳过。

两边**都会打日志**，不是完全静默 ——
但「跳过一个节点」到底该不该让整批作废，
得先定义「**画布忠实度**」。

> 这是 P0-1 要解决的事：
> **解析层别丢节点** —— `pipeline_graph.h` 保留未知算子、标 `known=false`。
> 一次同时解掉欠账 19 和欠账 18 的加载期。

### 欠账 19（`PipelineGraph` 三个死字段）—— 已登记，未修

`pipeline_graph.h:109-111` 三个字段生产代码一处不读，
而且**连锁断链**：写了没人读 → 下一个人不敢删 → 字段越长越不敢动。

P0-1 一起做。

## 9. 这一轮的教训

### 9.1 「还没漂」不等于「不会漂」

第十次抄副本里，前九次至少有七次**当场就漂了**。
这一次**没漂** —— 但收它的理由和前几次**完全一样**：

> 没有东西保证它们一致，
> 而哪天不一致的时候，**两边都是绿的**。

**「这次没出事」不是不收的理由** ——
它只是说明这一次的**爆款还没到**。

### 9.2 lint 的替代品是编译期约束

本项目守边界一直靠源码 lint。
这一轮证明了一条更一般的分工：

| 判据是…… | 用什么守 |
|---|---|
| **文本**的 | lint（而且要先剥注释） |
| **依赖**的 | **编译期约束**（一个只链最窄依赖的目标） |
| **行为**的 | 断言 |

而「层」这条边界，判据恰恰是**依赖** ——
所以它**一开始就不该由 lint 守**。

> lint 查「文本里有没有」，
> 编译器查「依赖里有没有」。
> 后者强一个量级，而且**不会被注释骗、不会被等价改写骗**。

### 9.3 边界也可以是「构建拒绝的东西」

以前本项目的所有规则，红的方式都是**断言红**。
这一轮第一次出现**另一种红**：

> 边界不是文档里的一句承诺，
> 而是**构建会拒绝的东西**。

代价是失败发生在构建那一步，第一反应容易是「环境坏了」——
所以得配一条**读得懂**的守卫把同一件事用中文讲一遍。

### 9.4 「抄副本」包括测试代码里的

`stripComments` 在 `test_result_render_plan.cpp` 里已经有一份。
写新套件时**直接复用**它就是第十一份副本 ——
所以抽成了 `tests/source_scan.h`。

> 测试代码里的副本更危险：
> 它让人以为「这工具已经标准化了」，
> 于是下一个人**再抄一遍**。

## 10. 配套

- `notes/atomic/at-the-copies-had-already-drifted.md` —— 抄副本的**第十次**
- `notes/atomic/at-the-compiler-can-guard-what-a-lint-cannot.md` —— **新卡**
- `notes/atomic/at-the-guard-can-be-fed-by-a-comment.md` —— 补「lint 的替代品」
- `notes/notes_W7_RenderPlan_2026-10.md` —— 上一篇（第九次）
- `notes/notes_W7_BadStep_2026-10.md` —— 第七次
- `notes/notes_W7_Verdict_2026-10.md` —— 第六次
- `notes/notes_W7_Knob_Audit_2026-10.md` —— 欠账总账
- `notes/notes_W7_Close_2026-10.md` —— 结项
