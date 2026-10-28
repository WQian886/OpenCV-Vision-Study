# 你以为它在等你抄，其实它已经抄歪了

## 核心
2026-10-21 扫 W7 全部算子，想查"有没有哪个旋钮的默认值被抄了多份"。

本来以为会查出一堆"潜在风险"（三处都写着 0.9，改一处忘一处）。
脚本一跑，结果是**三处已经漂了**：

| 旋钮 | 声明 | `run()` 兜底 | 面板初值 |
|---|---|---|---|
| `blob_count.min_area` | 20 | **50** | 20 |
| `count_check.expected_min` | 1 | **0** | 1 |
| `count_check.expected_max` | 100 | **999** | 100 |

还有一处更糟：`pcb_inspect` 登记的是 `min_area = 80`，
读的是 `min_area_pct` —— **登记的和读的不是同一个 key**。
于是 `min_area` 是死旋钮，而 `min_area_pct` 从来没登记过。

## 为什么这事重要
"抄副本"听起来像是**还没出事**的隐患。
实际上它**已经出事了**，而且出的正是最难查的那种：

- `count_check` 的兜底是 `0..999` —— 上游给什么数都判合格。
  一个**判废算子**在漏检时**判合格**，画布上的点还是绿的。
- `blob_count` 按 50 滤，面板上写 20 —— 用户调的值和算子用的值不是一回事。

这些都**不会报错、不会崩、不会留日志**。

## 教训
> "抄副本"不是假想敌，别用"以后注意"来对待。
> 它是**已经发生**的事实，只是还没被看见。
>
> 而且它发生的速度比想象中快：
> 抄的时候两份一模一样，所以**看起来完全正常** ——
> 没有人会在刚抄完的当天去核对两边。

所以正确的做法不是"小心别抄歪"，是**让第二份不存在**：
默认值只有一份（`operator_defaults.h`），
`defaultParams()` / `run()` 兜底 / 面板初值全部引用它。

## 第六次：2026-10-24，判定口径也抄了两份

W7 结项之后还欠账 17（主窗口零覆盖），
勘查时发现「结论怎么说」这件事有**两份**实现：

- `mainwindow.cpp` 的 `runPipeline`（给人看的那份）
- `headless_runner.cpp` 的 `runPipelineJson`（给 CI 看的那份）

抄的时候一模一样，**而且已经漂了**：

| 规则 | headless | GUI |
|---|---|---|
| 一帧里有步骤报错 → 既不算 PASS 也不算 FAIL | ✅ | ❌ |
| 有步骤报错 → 最终结果强制 ERROR | ✅ | ❌ |
| 整批判定看哪一帧 | 末帧 | 任意一帧 |

后果：**同一条流水线，GUI 说「最终结果: PASS」，headless 说 ERROR。**
而产线上判废看的是 GUI 那份 —— 它是错的那一份。

### 这次为什么还是没人发现

因为**两边都是绿的**：
GUI 跑得好好的，CI 四种退出码全对。
而 `mainwindow.cpp` 不在任何测试目标里，
所以「两份该一致」这件事**从来没被任何东西检查过**。

> **没有比对，就没有发现。**
> 零覆盖的真正代价不是「主程序没测试」，
> 是「主程序和 CI 各有一套判定，而没人比对过」。

### 收法

规则收成**一份**（`frame_verdict.h`），两个调用方都用它，
再配一条**源码守卫**（判据是「**要求某个可测入口**」，
不是「禁止某种写法」—— 后者会被等价改写绕过）：

```
两个调用方都必须走 w7::finalResultOf / w7::classifyBatch / w7::pickFrameResult
不许再出现自己写的 `if (r == "PASS")` 那种计数循环
```

> 顺带：这一轮的教训里还有一条**新的** ——
> 「末帧判定」和「整批判定」是两个概念，混用就是漏检。
> 那条另开了一张卡：`at-the-last-frame-is-not-the-batch.md`。

参见 `notes/notes_W7_Verdict_2026-10.md`（第六次的完整记录）、
`notes/notes_W7_Knob_Audit_2026-10.md`、
`notes/atomic/at-four-copies-of-one-number.md`。

## 第七次：2026-10-25，抽走了「分类」，却把「判据」留在外面

第六次抽 `frame_verdict.h` 的时候，只抽走了**分类**
（`classifyFrame` / `classifyBatch` / `finalResultOf`）。
「**哪一步算坏**」这个**判据**当时还留在外面：

```cpp
// mainwindow.cpp
if (info.output.contains(QStringLiteral("error"))) ++frameErrors;
// headless_runner.cpp
if (info.output.contains(QStringLiteral("error"))) { ++frameErrors; ++res.errorCount; }
```

两份一模一样 —— 而这是**刚建好的架构里的裂缝**：
说「规则只有一份」，而规则还有半份在外面。

而且那句判据**只认一种坏法**：算子返回 `false`（压根没跑成）时，
引擎把输出**置空**，失败原因写在 `info.error` 里 ——
「输出里带 error」**数都数不到**。

> **教训**：「抽成一份」要抽**整条规则**，不是抽它的一半。
> 分类抽走了、判据没抽走，等于把「两份」从「两份完整的规则」
> 变成了「一份完整的规则 + 两份半截的规则」——
> 看起来更整齐，实际更容易漂。

收法：判据和采集**一起**收（`w7::isBadStep` / `w7::FrameErrorCounter`）。

## 第八次：2026-10-25，「这一步显示哪张图」抄了两份

- `step_frame_cache.h` 的 `stepPrimaryImagePath` —— 决定「**图还在不在**」查哪个文件
- `mainwindow.cpp` 的 `renderOutputToResultPanel` —— 决定「**显示哪张**」

### 这一次特别的地方：作者**早就写下了警告**

`step_frame_cache.h` 里那段注释：

> 「优先级必须和 `renderOutputToResultPanel` 的显示顺序对齐 ——
> 两份各写一份的话，加一种新输出类型就只改一处，于是
> "图还在不在"的判据和"显示哪张"会跑偏，
> 而**跑偏的方向恰恰是那个会撒谎的方向**。」

**警告就写在代码里，然后它们真的跑偏了。**

### 跑偏成什么样

| | 查的是 | 显示的是 |
|---|---|---|
| 判据 | `marked_path`（缺陷标注图） | `image_path` \|\| **工具栏那张静态图** |

`pcb_defect_judge` 的输出**根本没有 `image_path`** ——
于是回看「缺陷判定」看到的是**用户手填的那张静态图 + PASS/FAIL 大字**，
而它自己产出的**缺陷标注图一张都没显示过**。

多帧流水线里，那张静态图和第 7 帧根本不是同一张。

### 这一次多出来的一条教训

> **「两份靠人工对齐」这个安排，本身就是 bug。**
>
> 哪怕你**在注释里写下**「这两份必须对齐」——
> 那也只是把「靠纪律」换成了「靠读注释」，
> 而**注释不会红**。

### 收法

规则收成**一份**（`result_render_plan.h`），
**判据和显示都用它** —— 而且**顺序写成一张表**（数据），不是三段 `if`（代码顺序）：

```cpp
static const KeySource kTable[] = {
    { "marked_path", ImageSource::Marked  },
    { "mask_path",   ImageSource::Mask    },
    { "image_path",  ImageSource::Source  },
};
```

> 顺序写成代码,就又变成了「藏在代码顺序里的规则」：
> 读的人得逐行读下去才知道谁优先，
> 而改的人只要动一行就能悄悄改掉顺序。
> 写成表,顺序是**数据**:一眼看得见,也能被一条变异单独打乱。

## 第九次：2026-10-25，**收成一半**也是抄副本

第八次那条规则（候选列表）确实收成了一份。但——

**「从候选里挑哪个」还是两份。**

| | 挑法 |
|---|---|
| 判据侧 `step_frame_cache.h :: imageState()` | 只看**第一**候选（不降级） |
| 显示侧 `mainwindow.cpp` 的候选循环 | 取第一个**存在的**（降级） |

两份靠什么对齐？靠「`Dropped` 那道闸在候选循环**之前**」——
**代码顺序**。

### 这次多出来的一条教训

> **「收成一半」和「没收」是同一件事。**
>
> 候选列表只有一份了，但「取哪张」还是两份 ——
> 那份规则**只是换了个地方长**，没被删掉。
>
> 而且它长的地方更隐蔽：
> 第八次的两份是**同名函数**，一眼能看出是一对；
> 这一次的两份藏在**两个不同的概念**底下
> （一个叫「存在性判据」，一个叫「显示顺序」），
> 读代码的人根本不会把它们放在一起看。

### 而且它还在撒谎

面板那份**会降级**。于是：

> 「最终结果」那次调用**没有判据**（`imgState` 默认 `Available`），
> 判定节点的 `marked_path` 不在时，
> 面板会一路退到**工具栏那张静态图** ——
> **一张和这一帧无关的图，配着这一帧的判定。**

又一次「对应关系是假的」。而这次是**收成一半**之后**新长出来**的。

### 收法

抽一个**两边都调**的函数：

```cpp
inline const ImageCandidate* pickBaseImage(const QVector<ImageCandidate>& cands,
                                           const ExistsFn& exists);
```

于是「判据说图在 ⟹ 面板显示的就是它」
不是靠对齐，是**同一份代码算出来的**。

> **「同一份代码」比「同一份规则」强一个量级。**
> 同一份规则还要人去对齐；同一份代码没有对齐这个环节。

### 顺带一条：谓词也跟着规则搬

`ExistsFn` / `fileExists` 原来住在 `step_frame_cache.h`，
现在「取哪张图」也要用同一个口径，
所以它们**搬到了规则旁边**（`result_render_plan.h`）。

> 规则搬到哪儿，**能证明它的那把尺子**也得在哪儿。
> 尺子留在原地，下一个人就可能给它配一把新的。

### 配套

`notes/atomic/at-the-criterion-and-the-reality-must-be-the-same-thing.md`
（判据和它要检验的现实必须是同一件事）、
`notes/atomic/at-the-guard-can-be-fed-by-a-comment.md`
（源码守卫会被注释喂饱）。

参见 `notes/notes_W7_RenderPlan_2026-10.md`、
`notes/notes_W7_BadStep_2026-10.md`（第七次）、
`notes/notes_W7_Verdict_2026-10.md`（第六次）、
`notes/notes_W7_Knob_Audit_2026-10.md`。

## 第十次：2026-10-25，「源怎么报帧数」抄了两份

规则只有**三条**：

1. 源在**输出**里报一个 `frame_count` 字段；
2. **第一个**带这个字段的输出说了算（按执行序）；
3. 报出来的数要过 `clampFrameCount`。

实现有**两份**：

| | 在哪 |
|---|---|
| GUI | `mainwindow.cpp :: MainWindow::runPipeline` |
| CI | `headless_runner.cpp :: runPipelineJson` |

### 这一次特别的地方：它**还没漂**

前十次里，多数是「抄的时候一模一样，然后真的漂了」。
这一次两份**行为完全一致**。

但收它的理由和前几次**一个字没变**：

> 没有东西保证它们一致；
> 哪天源算子改了 `frame_count` 的语义，两边会各按各的理解算；
> 而**两边都是绿的**。

> **「还没出事」和「不会出事」之间，隔着的正是这条规则。**
> 「这次没漂」只是说明**爆款还没到**，不是说明**收它没必要**。

### 它为什么一点都不显眼

因为它藏在**一个字段名**里。

`frame_count` 在 `run_limits.h`、两个调用方、还有各个源算子里都出现 ——
任何一次 grep 都会搜到它，所以它**看起来像是一份**。

而实际上「哪一帧的输出算第一个」只有**那份循环**说了算 ——
而循环有两条。

> **藏在名字里的副本，比藏在两个函数里的副本更难发现。**
> 前者连「找一份出来」的念头都不会起。

### 顺带：测试代码里的副本

写新套件时发现 `stripComments` 已经有一份（在 `test_result_render_plan.cpp` 里）。
直接复制就是**第十一份**副本 ——
所以抽成了 `tests/source_scan.h`。

> 测试代码里的副本更危险：
> 它让人以为「这工具已经标准化了」，
> 于是下一个人**再抄一遍**。

### 收法

抽一个**两边都调**的函数（`w7::resolveFrameCount`），
规则从「各写一份循环」变成「**一条数据**」：

```cpp
inline int resolveFrameCount(const QVector<QJsonObject>& outputs,
                             int* reportedOut = nullptr,
                             bool* truncated = nullptr);
```

两个调用点各剩一行。

### 配套

`notes/atomic/at-the-compiler-can-guard-what-a-lint-cannot.md`
（这条边界的另一半：分层靠编译期守）、
`notes/atomic/at-the-guard-can-be-fed-by-a-comment.md`
（源码守卫会被注释喂饱）。

参见 `notes/notes_W7_FrameProtocol_2026-10.md`、
`notes/notes_W7_RenderPlan_2026-10.md`（第九次）、
`notes/notes_W7_BadStep_2026-10.md`（第七次）、
`notes/notes_W7_Verdict_2026-10.md`（第六次）、
`notes/notes_W7_Knob_Audit_2026-10.md`。

## 第十一次：2026-10-25，「算子没装」有两份实现

> 这一遍的完整记录在 `notes/notes_W7_MainWindow_2026-10.md` 第 6.2 节 ——
> 主窗口进测试那一轮逮到的：两份实现**行为一致**，只有**消息**不一致。
> 行为一致意味着「两份一起错」也看不出来 ——
> 只有**消息**这个「无害的差别」暴露了它。

## 第十二次：2026-10-28，**跨项目**抄了一份「合成图的事实」

给 W3 补测试时发现的。

W3 的 `main.cpp` 里**内联抄了一份 W2 的金色 HSV 窗口**：

```cpp
// ===== 2. 转 HSV + 金色 mask(复用 W2 思路) =====   ← 注释说复用
cv::inRange(hsv, cv::Scalar(10, 100, 100), cv::Scalar(30, 255, 255), goldMask);
```

同一个文件里还留着**一整份注释掉的** `generatePcbLikeImage()`，
头部写着「同 W2, 8 个金色焊点 — ⏸ 已注释,保留作对照」。

⇒ **注释说复用，代码是抄的。**

### 这一遍和前十一遍**形状不同**

前十一遍里，两份都在**同一个产品**里（GUI 和 CI、判据侧和显示侧）。
这一遍的两份在**两个项目**里（W2 和 W3）。

但病一样，而且**更不容易发现**：

| | 前十一遍 | 这一遍 |
|---|---|---|
| 两份在哪 | 同一个产品里 | **两个项目里** |
| 谁看得见另一份 | 读代码时可能看见 | **跨目录，几乎没人会去看** |
| 漂了会怎样 | 一个产品里两套行为 | 两个项目**各自都是对的**，合起来是错的 |

### 为什么两份测试都是绿的

因为**每一份都只对着自己那份图断言**。
W2 的测试断 W2 那份窗口，W3 的测试断 W3 那份窗口 ——
**谁也没跨过去看一眼**。

> **没有比对，就没有发现**（第六次那条，这次又用上了）。
> 这次的比对不是「把两份放一起跑」，
> 是**让第二份根本不存在**。

### 收法

事实只留一份，放在 `common/pcb_sim/pcb_sim.h`：

- 焊点颜色、板面底色、丝印颜色
- 噪声强度、图的尺寸、焊点大小
- **金色的 HSV 窗口**
- `addNoise(img, seed)` —— **带种子**的噪声生成

W2 和 W3 都从它取。
W2 的公开入口（`padRects` / `padColor` / `backgroundColor` /
`goldHsvLower` / `goldHsvUpper`）**一个都没删** ——
它们现在是**转发**：**名字还是那些名字，实现只剩一份**。

> 顺带治好了另一件事：
> 老的生成器用 `cv::randn` 却**没设种子** ——
> 两次跑出来的是两张不同的图。
> 固定 seed 之后，这是**一张固定的图**，不是「一张像这样的图」。

### 配套

`notes/atomic/at-a-named-constant-can-still-be-a-copy.md`
（取一个名字不叫常量，它就还是副本）、
`notes/atomic/at-four-copies-of-one-number.md`、
`notes/atomic/at-extract-it-or-it-is-just-a-second-copy.md`。

参见 `notes/notes_W3_Tests_2026-10.md` 第 2 节、
`notes/notes_W7_MainWindow_2026-10.md`（第十一次）。

