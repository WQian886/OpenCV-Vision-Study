# W7：让「跑坏了」真的算进判定（欠账 18 的运行期形状）

> 📅 2026-10-25（结项之后还的第二笔）
> 🎯 欠账 18 收一半 + 抽掉本项目**第七份**「抄副本」的萌芽
> 📋 这不是新功能片，是**还账**

---

## 0. 一句话

> 上一轮把「结论怎么说」收成了一份，**但只抽了一半**：
> 「**哪一步算坏**」这个判据，两边还是各写一遍。
> 而那一句判据 ——「输出里带 error 就算坏」——
> **只认一种坏法**，另一种它**根本数不到**。

---

## 1. 起因：上一轮只抽了一半

上一轮（`notes/notes_W7_Verdict_2026-10.md`）把判定口径收成了 `frame_verdict.h`：

- ✅ **分类**收进来了：`classifyFrame` / `classifyBatch` / `finalResultOf` / ...
- ❌ **采集**没收进来：判「这一步算不算坏」还是两边各写一遍

```cpp
// mainwindow.cpp:1176
if (info.output.contains(QStringLiteral("error"))) ++frameErrors;

// headless_runner.cpp:146
if (info.output.contains(QStringLiteral("error"))) {
    ++frameErrors;
    ++res.errorCount;
}
```

**两边一模一样** —— 而**两份一模一样的东西就是两份会各自漂的东西**。
本项目栽过六遍的「抄副本」，上一轮栽的正是这一类。

> ⚠ 这是**刚建好的架构里的裂缝**：
> 上一轮把「分类」搬进了一份，却把「判据」留在外面 ——
> 等于说「规则只有一份」，而规则其实还有半份在外面。

---

## 2. 那个洞：坏步有两种**不一样**的形状

### 2.1 引擎是怎么记失败的

```cpp
// run_engine.h
if (!ok) {
    info.failed = true;
    info.error  = err;                    // ← 失败原因写在这里
    frame.outputs[step] = QJsonObject();   // ← 输出被**置空**
    ...
}
```

**失败原因在 `info.error`，而输出被置空了。**

### 2.2 而判据读的是输出

```cpp
if (info.output.contains(QStringLiteral("error"))) ++frameErrors;
```

于是：

| | 算子怎么表现 | 输出 | 失败原因在哪 | 以前数得到吗 |
|---|---|---|---|---|
| **形状 1** | 返回 `true`，输出里带 `error` 键 | **有内容**，明明白白写着 error | 输出里 | ✅ 数得到 |
| **形状 2** | 返回 `false`（压根没跑成） | **空的** | `info.error` | ❌ **数不到** |

**形状 2 才是危险的那个** —— 形状 1 至少在输出里**写着** error，
翻一下输出就能看见。形状 2 里，失败**只存在于一个没人读的字段**。

> 这就是本项目最贵的那类静默失效的**机制**：
> **信息存在，只是没人去看它。**
>
> 新卡：`notes/atomic/at-the-failure-is-in-another-channel.md`

---

## 3. 后果链条（实测复现）

```
src 算子返回 false
  → 这一步被吞（输出空的，判据数不到）
  → 链继续往下跑
  → judge 拿到**空的上游输出** → 自己的默认值兜底 → 数字看着正常
  → judge 说 PASS
  → 那一帧算 PASS → 整批结论 PASS → 退出码 0
  → CI 说「过了」
```

整条链上**没有一个红点**：

- GUI 那边只有一行 `❌` 日志（`mainwindow.cpp` 的 `if (info.failed)` 分支），
  对判定**毫无影响**；
- headless 那边 `errorCount` 是 **0**。

而用户存的那条流水线**压根没跑成**。

> 上一轮笔记里那句「GUI 明明拿到了 `info.failed`，却只打一行日志，
> 不进判定」—— 这一轮把它补上了。

---

## 4. 为什么以前没人发现

因为**用例只断言了一半**。

`test_headless_runner.cpp` 的用例 5「算子失败:这一步没产出,但链继续往下跑」断言的是：

```cpp
check(sawFailed, "结果里标出了那一步失败");                    // ✅ 有
check(seenErr.contains("炸了"), "失败原因带上了");               // ✅ 有
```

**没有**断言的是：

```cpp
// ❌ 没有:失败**影响了判定**吗?
check(res.errorCount == 1, ...);
check(res.finalResult == "ERROR", ...);
check(res.batchVerdict == w7::BatchVerdict::Errored, ...);
```

> 第一条成立**不等于**第二条成立 —— 而第二条才是产线看的那一条。
>
> 这是「测了不等于测全」的又一个实例：**门在那儿，只有半扇门。**

而且上一轮写的用例 11「算子没崩但报错了:也得算成错误」，
测的恰恰是**形状 1**（`errorOn`）—— 形状 1 有测试，形状 2 没有。

---

## 5. 怎么改

### 5.1 判据收成一份（`frame_verdict.h`）

```cpp
inline bool isBadStep(bool failed, const QJsonObject& output) {
    if (failed) return true;                          // 压根没跑成
    return output.contains(QStringLiteral("error"));   // 跑成了但报了错
}
```

⚠ 判据住在**能同时看见两个通道**的地方。
如果它还住在只看得到 `output` 的那一层，就永远数不到形状 2。

### 5.2 采集也收成一份（同一个头文件）

```cpp
struct FrameErrorCounter {
    int errors = 0;
    bool noteStep(bool failed, const QJsonObject& output) {
        if (!isBadStep(failed, output)) return false;
        ++errors;
        return true;
    }
    int count() const { return errors; }
};
```

两个调用方都改用它：

- `mainwindow.cpp`：`frameErrors.noteStep(info.failed, info.output);`
- `headless_runner.cpp`：`if (frameErrors.noteStep(info.failed, info.output)) ++res.errorCount;`

> ⚠ `noteStep` **返回**是不是坏的 —— 调用方据此累计**全批**总数，
> 自己决定累计在哪儿。采集和累计是**两件事**，别混。

### 5.3 口径变了，所以**判据也变了**

上一轮特意留了一句：「这份文件不许改判定口径」。
这一轮**改了** —— 而且改得有据：

- 上一轮的口径是 headless 那份**已经有的**（有测试钉着的）；
- 这一轮是**补上一个从来没被数到的洞**，不是重新设计。

> 口径扩了，就得连着改测试和两边 —— 这一轮就是这么做的。

---

## 6. 改口径之前先量风险面

改判据会**改变行为**。所以动手前先数了一遍：

```
17 个算子里的 return false 分布:
  board_recipe         x2   (配方解析失败 / 配方文件读不到)
  dl_infer             x1   (没装 ONNX Runtime)
  image_source_camera  x7   (相机连不上 —— 全在真 SDK 分支里)
  其余 14 个             x0
```

**三个算子，全是真失败，没有一处是正常路径。**

所以补上这条判据**不会改变任何正常流水线的行为** ——
它只在**真失败**时改变判定。

> 实测印证：改完之后 `headless_runner` 原有 129 条**一条没红**。
>
> ⚠ 这一步很重要：改口径最怕的是「顺手改好了几个 case，
> 却不知道有没有改坏别的」。先量，再改，改完再看**红的条数**。

另外还核了一遍 CI 那条冒烟流水线（`pipelines/smoke.json`）：
只用了 `pcb_inspect` 和 `pcb_defect_judge`，两个都是 0 处 `return false`
—— 所以 CI 的「exit 0」那条**不受影响**，「exit 3」那条走的是形状 1，也不受影响。

---

## 7. 测试抓到了什么

### 7.1 `frame_verdict` 套件：69 → **90** 条（+21）

新用例 7「`isBadStep`:两种失败形状都得算坏」：

| 测什么 | 为什么 |
|---|---|
| `isBadStep(false, {error})` → true | 形状 1 |
| `isBadStep(true, {})` → **true** | **形状 2 —— 本轮补上的那个洞** |
| `isBadStep(true, {error})` → true | 两种一起来 |
| `isBadStep(false, {})` / `isBadStep(false, {result})` → false | 不能误报 |
| 采集器：`noteStep` 返回值 + `count()` 累计 | 采集器本身 |
| 坏帧接进帧判定：不算 pass、不算 fail、计入 error、最终 ERROR | 闭环 |

### 7.2 `headless_runner` 套件：129 → **140** 条（+11）

新用例 5b「算子压根没跑成:这一帧是坏帧,不是 PASS」：

- 下游拿到的是**空的上游输出**（根因摆出来）
- `errorCount == 1`（这一步被数出来了）
- `passCount == 0` / `failCount == 0`（既不算过也不算废）
- `finalResult == "ERROR"` / `batchVerdict == Errored`
- 结果 JSON 里也说了（不然 CI 看不见）
- 判定节点的原始结论**还在** outputs 里（没被抹掉）
- 没失败的时候**不误报**

### 7.3 源码守卫升级了

守卫的判据从「`++frameErrors` 这个形状」换成
「**必须走共享采集器，而且把 `failed` 传进去了**」：

```cpp
check(src.contains("noteStep(info.failed"), ...);   // 两个调用方都要
check(!src.contains("info.output.contains(QStringLiteral(\"error\"))"), ...);
```

> ⚠ 判据写 `noteStep(info.failed` 而不是 `noteStep(`：
> 只查 `noteStep(` 的话，把 `info.failed` 换成 `false` 也能过 ——
> 而那**正是**本轮修的那个洞。
> 上一轮 M129 栽过一次同样的事（`frameErrors += 0` 也能过 `++frameErrors` 的检查），
> 所以这次直接钉到**参数**上。

---

## 8. 变异第 16 轮：M134~M144（11 条，8 杀 3 活）

`unexpected results = 0`。

| 变异 | 改了什么 | 判定 |
|---|---|---|
| **M134** | **`isBadStep` 不再认 `failed`**（把洞放回去） | KILL（8 条红）|
| M135 | `isBadStep` 不再认输出里的 error | KILL（5 条红）|
| M136 | 采集器不累加（`errors += 0`） | KILL（6 条红）|
| M137 | 采集器永远报 0 | KILL（6 条红）|
| **M138** | **GUI 不再把 `failed` 传进去**（把洞放回去） | KILL（**1 条红**）|
| **M139** | **headless 不再把 `failed` 传进去**（把洞放回去） | KILL（5 条红）|
| M140 | headless 不把坏步数交给帧判定 | KILL（7 条红）|
| M141 | 旧的**内联判据**被写回来（抄副本） | KILL（3 条红）|
| M142~M144 | 三条**合法等价改写** | SURVIVE（必须活）|

### 8.1 M138 只红了 1 条 —— 这本身是个结论

M138 改的是 `mainwindow.cpp`。主程序**不在任何测试目标里**，
所以能抓住它的**只有源码守卫**那一条断言 —— 所以正好红 1 条。

> 这说明两件事：
> 1. 那条守卫**不是摆设**（它确实抓得住）；
> 2. GUI 侧的覆盖**只到「调用存在」这一层** ——
>    「调用会执行到吗」「位置对吗」，lint 答不了。
>
> 这也正是欠账 17 剩下的那部分为什么**难**：
> 源码 lint 验不了「点了 Run 有反应」。

### 8.2 M141 测的是「抄副本」本身

M141 把旧的内联判据**写在共享采集器旁边**（两行都在）。
它被两条不同的断言抓住：

- headless 的行为断言（`errorCount` 变成 2 而不是 1）；
- 源码守卫（内联判据不许再出现）。

> 上一轮说「两份一模一样的东西就是两份会各自漂的东西」——
> 这一轮**当场演示**了：只要两份都在，就多出一条能数错的路。

---

## 9. 欠账 18 的状态：**收一半**

欠账 18 有**两个形状**，这一轮收掉了运行期那个。

| 形状 | 状态 |
|---|---|
| **运行期**：算子返回 false（压根没跑成） | ✅ **已收** |
| **加载期**：存档里写了个不存在的算子名 → `graphFromJson` 跳过 → 链继续跑 | ⏸ **仍欠着** |

### 9.1 加载期那个为什么不一起收

不是因为难，是因为**两边形状不对称**，硬做会引入**假阳性**：

- **headless 侧**：一次性跑。`g.skippedNodes` 在手边，直接用。
- **GUI 侧**：Load 时跳过的节点**根本不在画布上**（画布只会有注册过的算子）。
  要把「Load 时跳过了几个节点」带进判定，就得在 MainWindow 里存这个状态；
  而**用户一旦编辑画布，这个状态就过期了** ——
  画布此时已经是一个完整可跑的流水线了，却还会被判 ERROR。

所以修它得先定义一个概念：**「画布对不对得上用户存的那份」**。
那是另一笔，而且比这一笔大。

- **边界**：加载期这个洞**不是静默的** ——
  `graphFromJson` 会打一条日志（`[Load] 跳过未知算子: xxx`），
  GUI 和 headless **两边都会报**。所以它是「报了但对判定无影响」，
  不是「压根没人知道」。
- **重开条件**：**要定义「画布忠实度」的时候** ——
  也就是要决定「跳过一个节点，该不该让整批作废」的那一刻。
  可能的做法是：把未知算子**留在画布上**（标成「未安装此算子」），
  让它跑的时候**像别的失败一样**被算成坏帧 —— 那样两边天然一致，
  而且用户在画布上**看得见**少了什么。
  这和这一轮修的形状 2 是同一套机制。

---

## 10. 顺带：上一轮没找出来的那 6 条，找到了

这一轮要更新 README 的数字，得逐个套件数一遍 ——
于是上一轮（`notes_W7_Verdict_2026-10.md` §7.2）明写「**查不清就写清楚查不清**」
的那 6 条，**查清了**。

它们**不是一条**账，是**两笔方向相反的账**：

| 套件 | 笔记记的 | 实测 | 差 |
|---|---|---|---|
| `run_engine` | **111** | **99** | **+12**（记多了）|
| `camera` | **90** | **93** | **−3**（记少了）|
| | | **净差** | **+9** ✅ |

`12 − 3 = 9` —— 正好是上一轮那个「差 9」。

> 上一轮只抓到 `camera` 就停手，是因为**另一笔的误差方向正好相反**，
> 两笔**部分抵消**，剩下的净额看起来像「一笔说不清的账」。
>
> **教训**：差值对不上时，**别在「总数」上找原因** ——
> 逐项对一遍，两个方向相反的误差会自己现形。

顺带还抓到**另一处漏**：
**W7 README 的套件表里根本没有 `frame_verdict` 那一行** ——
上一轮加了套件却没加表行（21 行 vs 22 个套件），
而且 `run_engine` 那一行的 **111** 是**旧数**
（欠账 17 的登记处一直写的是 99）。

> ⚠ **「总数对得上」不代表「每一项都对得上」。**
> 这次总数是对的（因为另一个套件的误差抵消了），
> 而表里**缺了一行**、**错了三个数** —— 全靠**逐项数**才看得见。

---

## 11. 这一轮的数字（实测，不是推算）

| | 数 |
|---|---|
| `frame_verdict` 套件 | 69 → **90**（+21）|
| `headless_runner` 套件 | 129 → **140**（+11）|
| W7 套件 / 断言 | 22 / **2152** |
| W6 套件 / 断言 | 3 / 477 |
| 全仓套件 / 断言 | 25 / **2629** |
| 变异 | 134 → **145**（121 杀 / 23 活 / 1 编译期）|
| atomic 卡 | 60 → **61** |
| notes | 21 → **22** 份 |
| C++ 文件 / 行数 | 123 / **37,861** |

---

## 12. 参见

- `notes/notes_W7_Verdict_2026-10.md` —— 上一轮：判定口径收成一份（欠账 17 收一半）
  （它的 §7.2.1 是这一轮补上的结案）
- `notes/notes_W7_Knob_Audit_2026-10.md` 第十节 —— **欠账清单的唯一登记处**
- `notes/notes_W7_Close_2026-10.md` —— 结项报告（总账、怎么继续）
- `notes/atomic/at-the-failure-is-in-another-channel.md` —— **新卡**：失败记在一个通道，判据去另一个通道找
- `notes/atomic/at-one-code-cannot-say-what-broke.md` —— 退出码 3 的定义补上了第二种坏法
- `notes/atomic/at-the-copies-had-already-drifted.md` —— 「抄副本」第六次；这一轮是**第七次**的萌芽
- `notes/atomic/at-extract-it-or-it-is-just-a-second-copy.md` —— 抽出来，别复制一份
- `notes/atomic/at-silent-fail-shape.md` —— 静默失效长得像成功
