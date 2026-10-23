# W7 流水线执行引擎 —— 从 mainwindow 里抽出来（第十轮）

> 📅 **日期**：2026-10-23
> 🎯 **目标**：给 headless runner 铺路 —— 先把「跑一条流水线」的内核
> 从 GUI 里搬出来，搬成**一份能直接测**的代码。
> 🔗 **相关**：`notes/notes_W7_Knob_Audit_2026-10.md`（旋钮审计那一条线）、
> `notes/atomic/at-extract-it-or-it-is-just-a-second-copy.md`（本轮的教训卡）

---

## 一、起点：先问「W7 缺什么功能」

盘完仓库之后，缺的功能列了五条（headless runner / 假 ONNX SDK /
子图复用 / 短路连通性 / 审计识别表显式化）。

排在第一的是 **headless runner**：`main.cpp` 现在只有一行
`QApplication app(argc, argv)`，**一行 CLI 解析都没有** ——
W7 只能开窗跑。后果很实在：CI 跑的全是单元测试，
**从来没跑过一条完整流水线**。W6 有 `w6_cli`，W7 缺这一半。

## 二、但第一步不是写 CLI，是抽内核

因为「跑一条流水线」的逻辑**全在 `mainwindow.cpp` 里**：

| 位置 | 内容 |
|---|---|
| `mainwindow.cpp:817` | `topoSort()` —— 拓扑排序 + 环检测 |
| `mainwindow.cpp:1087` | 多前驱输出的合并（后写覆盖） |
| `mainwindow.cpp:1111` | 逐节点执行（建算子 → `run()` → 存输出） |
| `mainwindow.cpp:1128+` | 每步缓存 / 状态点 / 日志（**这些是 UI，留着**） |

而 `mainwindow.cpp` **不在任何测试目标里** ——
它要真窗口 + 整套算子，建一次目标要好几分钟。

于是「拓扑序怎么算、有环能不能检出」「多前驱时谁赢」这两件有真实语义的事，
一直是**只有源码 lint 看着**的。

> 所以顺序必须是：**先抽内核 → 再写 CLI**。
> 反过来做就等于先写第二份执行逻辑 —— 那就是抄副本。

## 三、这两件事以前只有「一次性探针」知道

2026-10-15 有过一次**一次性探针**（手抄 `mainwindow.cpp` 的合并逻辑跑出来的），
实测到：

```
顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue / 5000
顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red  / 10000
```

两个上游状态点**都是绿的、都不报错**，只有下游那个数悄悄变了。
而且**连「先连线的赢」都不成立** —— 决定胜负的是 `connections()` 的遍历顺序。

> 那份探针是**一次性**的：合并逻辑一动它就过期，
> 而且**没人会知道**它过期了。

## 四、抽出来的是什么

新文件 `W7_VisionMasterLite/run_engine.h`（header-only）：

| 东西 | 作用 |
|---|---|
| `struct EngineEdge { int from; int to; }` | 一条边，两端都是**下标** |
| `topoOrder(nodeCount, edges, &hasCycle)` | Kahn 算法（入度 + BFS）+ 环检测 |
| `mergeUpstreamOutputs(upstream)` | 合并多个前驱的输出，**后写覆盖** |

依赖面：**只依赖 QtCore**（`QJsonObject` / `QString` / `QVector` / `functional`）——
**零 OpenCV、零 QtWidgets**。所以依赖它的测试目标建得快，
还顺带证明了「引擎这一层不需要拖整个 GUI 进来」。

### 为什么用下标而不是指针

跑批期间节点**可能被删**（右键删了 / Load 了）。
`mainwindow` 那边用 `QPointer` 兜着 ——
换成裸指针就是往已释放内存上写，实测过，表现是随机的卡死或闪退。

引擎这一层干脆**不持有节点指针**：它只认「第几个节点」。
主程序那边负责把 `OperatorNode*` 翻译成下标。

### 拓扑序为什么是确定的

同一张图可能有多个合法拓扑序。选哪个是实现细节，
但它必须**确定** —— 不确定的话，同一份流水线两次跑可能给出不同的执行顺序，
那是极难查的一类 bug（症状是「同样的图，结果时对时错」）。

实现里按**节点下标顺序**入队，所以边顺序打乱也不影响结果。

## 五、mainwindow 改成**调用**它（不是复制它）

这是本轮最要紧的一步 —— 复制一份的话，就等于没抽。

```cpp
// 以前:11 行合并逻辑,写在 mainwindow 里
QJsonObject inputs;
for (const auto* c : m_canvas->connections()) {
    if (c->toNode() == n && nodeOutputs.contains(c->fromNode())) {
        const QJsonObject& upOut = nodeOutputs[c->fromNode()];
        for (auto it = upOut.begin(); it != upOut.end(); ++it) {
            inputs[it.key()] = it.value();
        }
    }
}

// 现在:3 行
QVector<QJsonObject> upstream;
for (const auto* c : m_canvas->connections()) {
    if (c->toNode() != n) continue;
    if (!nodeOutputs.contains(c->fromNode())) continue;
    upstream.append(nodeOutputs[c->fromNode()]);
}
QJsonObject inputs = w7::mergeUpstreamOutputs(upstream);
```

拓扑排序同理：`topoSort()` 变成一个**适配器**
（把画布的对象指针翻译成下标），算法本身在 `run_engine.h` 里。

> ⚠ **顺序不能重排**：胜负取决于上游列表的顺序，
> 而那个顺序来自 `connections()` 的遍历顺序。所以必须**原样**传进去。

## 六、用例（6 组 / 34 条断言）

新套件 `run_engine`（`tests/test_run_engine.cpp`）：

| # | 核什么 | 关键断言 |
|---|---|---|
| 1 | 拓扑序 | 空图 / 单点 / 链 / **反着给的链** / 菱形；**边顺序打乱 → 序不变** |
| 2 | 环 | 三角环 / 自环 / **环挂在链尾** —— 检出 + **返回空序** |
| 3 | 坏边 | 越界 / 没填 → 跳过，不崩 |
| 4 | 合并 | 空 / 一个 / 不冲突 / **冲突后者赢** / 三个冲突 / **类型也换** / 空上游不干扰 |
| 5 | **顺序敏感性** | `[A,B]` → blue/5000；`[B,A]` → red/10000 |
| 6 | 守卫 | 同一条边方向反过来，拓扑序就变（证明引擎真在干活） |

用例 2 的「返回空序」值得单说：

> **宁可什么都不跑，也不要跑一半。**
> 跑一半的话，下游拿到的是**上一轮**的输出 ——
> 数字看着正常，结论却是错的。

用例 5 看着像「在断言一个 bug」，其实断言的是**当前真实语义**。
哪天把语义改成「先连线的赢」，这两条会红 ——
那时候要一起改的是：引擎 + 主程序的合并顺序 + `pin_types` 的提示文案。

## 七、变异 M49 / M50 / M51：两个杀、一个活

| 编号 | 注入 | 结果 | 红了几条 | 红在哪 |
|---|---|---|---|---|
| M49 | 合并改成「先写赢」（不再是后写覆盖） | **KILLED** | 5 | 用例 4 的三条 + 用例 5 的两条 |
| M50 | 去掉环检测 | **KILLED** | 5 | 用例 2 的五条 |
| M51 | 合并时跳过空上游对象（**合法等价写法**） | **SURVIVED** | 0 | —— |

M51 必须活：一个只会说「红」的规则是噪音发生器，不是规则。
这条证明的是「测试判的是**语义**，不是某一种写法」。

`unexpected results = 0`。

> 变异脚本 `build/mut_round10.ps1`：**纯 ASCII**。
> 注入锚点只取 ASCII 部分 —— 头文件里那行注释是中文，
> 而无 BOM 的 `.ps1` 会按 ANSI/GBK 读，中文注释会把**下一行代码吞进注释**。
> （第七轮 12.6 记过这个坑。）

## 八、这一轮的数字

- 新套件 `run_engine`：**34** 条断言
- W7 15 → **16** 个套件；1661 → **1695** 条断言
- 18 → **19** 个套件；2138 → **2172** 条断言
- 累计变异 49 → **52** 个（M49/M50/M51）
- 门禁：`tools/ci.ps1` → **ALL CHECKS PASSED**（7 项目 / 19 套件 / W5 冒烟 / 源码树零污染）

## 九、下一步：headless runner 还需要什么

内核已经有一半了，还差三块：

| 还缺什么 | 难在哪 |
|---|---|
| **逐节点执行**也进引擎 | 现在还在 `mainwindow` 的循环里。抽出来要处理 UI 回调（状态点/日志/每步缓存）—— 用「每步 hook」解决 |
| **读 JSON → 建图** | `pipelineFromJson()` 现在要一个 `PipelineView*`（QGraphicsScene）—— headless 得能离屏造（offscreen 可以） |
| **CLI 入口** | `main.cpp` 解析 `--run pipeline.json` / `--out result.json`，跑完输出 JSON |

顺序上：**先抽逐节点执行**（它和拓扑/合并是一体的），
再接 JSON，最后才是 CLI。
每一步都能单独验证。

> ⚠ 抽逐节点执行时要特别小心一件事：
> 现在跑批期间节点被删是靠 `QPointer` 跳过的。
> 引擎用下标，看不见「节点被删了」——
> 所以**这一步不能顺手把 QPointer 那套也抽走**，
> 要么留在 `mainwindow` 那层，要么在引擎里显式支持「节点已失效」。

---

# 片 2：逐节点执行也进引擎（2026-10-24）

## 十、这一片抽的是什么

上一片把**拓扑序**和**前驱合并**搬进了 `run_engine.h`，
但「逐节点执行」还留在 `mainwindow.cpp` 的循环里 ——
也就是说最要紧的那段（跑算子、组装 inputs、存输出、每步通知）还是没有测试。

这一片把它搬进同一个引擎：

```cpp
struct EngineFrame runFrame(const QVector<EngineNode>& nodes,
                            const QVector<EngineEdge>& edges,
                            const QJsonObject& fallbackInputs,
                            int frameIndex, int maxSteps,
                            const EngineDefaultsFn& defaults,
                            const EngineRunner& runner,
                            const EngineAliveFn& alive,
                            const EngineStepHook& hook);
```

引擎做的事，按重要性排：

1. 组装 `inputs` —— 上游输出合并 + 兜底（**缺了才补**）+ `frame_index` + `node_uuid`
2. 组装 `params` —— 节点参数优先，**空**则用算子的**声明默认值**
3. 跑算子，把输出存下来
4. 每步的前后通知（`Before` / `After` / `Skipped`），UI 通过 hook 回调
5. **验顺序**：顺序自相矛盾就报错，不闷头跑

依赖面不变：**只依赖 QtCore**，零 OpenCV 零 QtWidgets。

### 三个回调的分工

| 回调 | 回答什么 | 主程序那边怎么实现 |
|---|---|---|
| `defaults` | 「这个算子的**声明默认值**是什么？」 | `createOperatorByName()` → `defaultParams()`，按算子名缓存一份 |
| `runner` | 「拿这组 params / inputs 跑一步，输出是什么？」 | `createOperatorByName()` → `run()` |
| `alive` | 「这个节点**现在还在不在**？」 | `order[idx].isNull()`（`QPointer` 自动置空） |

`alive` 是**引擎看不见的那一半** —— 引擎手里只有快照（值），
「节点被删了」这件事只有画布知道，所以得问。

## 十一、UI 的活儿怎么留给 mainwindow

状态点、日志、每步缓存都要 `QWidget` 和 `QTextEdit`。
引擎碰了就等于把整个 GUI 拖进测试目标 —— 而 `mainwindow.cpp`
本来就不在任何测试目标里，**那正是这几件事一直没有测试的原因**。

所以引擎只留「纯」的那部分，UI 通过 `EngineStepHook` 回调：

```cpp
enum class EnginePhase { Before, After, Skipped };
struct EngineStepInfo {
    int nodeIndex = -1;      // 节点下标
    int step = -1;           // 第几步
    QString opName;
    QString uuid;
    QJsonObject output;      // 只有 After 阶段才有
    bool failed = false;
    bool skipped = false;
    QString error;
};
```

主程序那边从「一个 140 行的 for 循环」变成「一个 hook 回调」，
逻辑归引擎，UI 归 UI。

### 顺带堵上的一个 UAF

`Before` 阶段里有一句 `QApplication::processEvents()` ——
它会把用户挂着的右键删节点**派发进来**。
于是「节点在跑的这一步里被删了」是**会发生的**。

老代码在这一步之后直接用 `OperatorNode* n`（跑之前取的裸指针）：
节点没了，这个指针就是**悬垂的** —— 表现是随机的卡死或闪退。

新代码在 `After` 阶段**重新问一次**：

```cpp
case w7::EnginePhase::After: {
    if (!alive(idx)) return;         // ← 必须重新问
    OperatorNode* n = order.at(idx).data();
```

这不是本轮的目标，是抽离时**顺手堵上的** ——
抽出来的引擎逼着这件事被写下来。

## 十二、mainwindow 改成调用它

`mainwindow.cpp` 的节点循环整段换掉：

```cpp
const w7::EngineFrame ef = w7::runFrame(
    enodes, edges, fallbackInputs, frame, maxSteps,
    defaultsOf, runner, alive, hook);
```

### 单步执行的范围也交给引擎

老代码是在 for 循环里 `break`：

```cpp
if (singleStepTargetIndex >= 0 && step > singleStepTargetIndex) { ...; break; }
```

新代码是 `maxSteps = singleStepTargetIndex + 1`。

> **范围和执行顺序必须是同一个。** 两处各算一次迟早会不一致 ——
> 而不一致的后果不崩，只是「单步跑到的那一步」和用户以为的不是同一步。

### 「⏹ 单步执行到「…」为止」这条日志的位置

老代码在 `break` **之前**打，所以它在「这一帧的节点日志之后」。
新代码引擎跑完 `maxSteps` 个才返回，在 `runFrame` 之后打 —— **同一个位置**。

这类「日志顺序」的东西没人测，只能靠逐行核对。
本轮把老代码和新代码的日志顺序逐条对过一遍（见第十三节）。

## 十三、用例（9 组 / 99 条断言）

套件 `run_engine`（`tests/test_run_engine.cpp`），从 34 条涨到 **99** 条：

| # | 核什么 | 关键断言 |
|---|---|---|
| 1~3 | 拓扑序 / 环 / 坏边 | （片 1 的，没动） |
| 4~5 | 合并 / 顺序敏感性 | （片 1 的，没动） |
| 6 | 守卫 | 同一条边方向反过来，拓扑序就变 |
| **7** | **runFrame 跑一帧** | 空画布 / 链式传递 / 兜底只补缺的 / `frame_index`·`node_uuid` 总是覆盖上游 / 声明默认值只在需要时取 / **算子失败链继续跑** / **节点被删要跳过** / `maxSteps` / **顺序自相矛盾要拒绝** / hook 序列 |
| **8** | **源码守卫** | `mainwindow` 必须调 `w7::runFrame(...)`；不许再出现自己写的 inputs 合并循环；拓扑序也走 `w7::topoOrder(...)` |
| **9** | **hook 拿到的信息** | `nodeIndex` / `step` / `opName` / `uuid` / `skipped` / `failed` / `output` 每个字段都得对得上 |

用例 9 是**补出来的**，原因见第十五节。

### 用例 8 是 lint，不是行为测试

`mainwindow.cpp` 不在任何测试目标里，所以「主程序有没有接上引擎」
没法用行为测。但 lint 也能干这件事 ——
只要把判据从「禁止某种写法」改成「**要求某个可测入口**」：

```cpp
check(src.contains("w7::runFrame("), "mainwindow.cpp 走的是 w7::runFrame(...)");
check(!src.contains("inputs[it.key()] = it.value()"), "mainwindow.cpp 里没有自己写的 inputs 合并循环");
```

第二条是关键：引擎抽出来了、主程序却照旧自己写一遍，
那就等于**两份拷贝** —— 抽了个寂寞，而且两份会漂。

> 源码守卫**必须**配变异验证，否则它可能是一条恒真的空断言。
> M64 / M65 就是给它准备的（见第十四节）。

## 十四、变异 M52~M65：十个杀、四个活

| 编号 | 注入 | 结果 | 红了几条 | 红在哪 |
|---|---|---|---|---|
| M52 | 算子的输出不存下来 | **KILLED** | 6 | 用例 7 的六条 |
| M53 | `frame_index` 不往下传 | **KILLED** | 2 | 用例 7 |
| M54 | `node_uuid` 不往下传 | **KILLED** | 2 | 用例 7 |
| **M55** | **兜底改成「总是覆盖上游的」** | **KILLED** | **1** | **用例 7（补了洞之后才杀的，见第十五节）** |
| M56 | 声明默认值盖掉节点参数 | **KILLED** | 2 | 用例 7 |
| M57 | 「节点还在不在」的判据失效 | **KILLED** | 5 | 用例 7 + 用例 9 |
| M58 | 顺序自相矛盾不再报错 | **KILLED** | 3 | 用例 7 |
| M59 | hook 拿到的 `nodeIndex` 传成 -1 | **KILLED** | 3 | 用例 9 |
| M60 | 跳过的步骤没标 `skipped` | **KILLED** | 1 | 用例 9 |
| M61 | `maxSteps` 改成手写 min（**合法等价写法**） | **SURVIVED** | 0 | —— |
| M62 | 顺序判据换个写法（**合法等价写法**） | **SURVIVED** | 0 | —— |
| M63 | `mainwindow` 去掉 `const`（**合法等价写法**） | **SURVIVED** | 0 | —— |
| M64 | `mainwindow` 不再调 `w7::runFrame` | **KILLED** | 1 | 用例 8 |
| M65 | `mainwindow` 又写了自己的一遍 inputs 合并 | **KILLED** | 1 | 用例 8 |

`unexpected results = 0`。

### 四个 SURVIVE 是必须的

一个只会说「红」的规则是**噪音发生器**，不是规则。
M61/M62/M63 是三个**合法等价改写** —— 它们必须活，
证明测试判的是**语义**，不是某一种写法。

M63 尤其重要：它证明**源码守卫不是见字就红** ——
`mainwindow` 合法地改了一下写法，守卫**没有**叫。
不然那条守卫迟早被人关掉（规则宁窄勿宽）。

### 变异脚本的约定（沿用）

- **纯 ASCII**（无 BOM 的 `.ps1` 按 ANSI/GBK 读，中文注释会把下一行代码吞进注释）
- 注入锚点只取 ASCII 部分；锚点必须**唯一**（注入前先数命中次数）
- 还原后 **SHA-256 逐字节校验**
- 每个变异带 `Expect = KILL | SURVIVE`，结尾打印 `unexpected results = N`
- **BUILD-FAIL ≠ 变异存活**；**没重编 ≠ 变异存活**
- 变异 `mainwindow.cpp` 时**只重编引擎测试**（源码守卫读的是**源码文本**），
  但**收尾必须把 `w7_vm_lite` 也重编一遍** —— 否则磁盘上的二进制和源码对不上
  （老教训 `at-stale-binary-fake-red.md`）

> 脚本 `build/mut_round11.ps1`（gitignored）。
> 这一轮脚本还多了一条：**崩了要单独报**。
> 之前只数 `[FAIL]` 的条数，一个把程序**打崩**的变异会被误判成「存活」。
> 现在多一个 `CRASHED` 判决（没有 `SUMMARY:` 行 = 崩了）。

## 十五、本轮最值钱的东西：M55 活了

第一轮跑变异，**M55 活了**（期望 KILL，实测 96 条全绿）。

M55 注入的是：「兜底只补缺的」改成「兜底总是覆盖」。
按引擎的明文规则，这**必须**是红的。

### 为什么没红

因为那条断言**根本不在测它文案说的那件事**。

```cpp
// 红字:「上游自己的 image_path 没被兜底覆盖」
check(f.outputs.at(0).value("image_path").toString() == "from-upstream.png", ...);
```

它读的对象是 `f.outputs.at(0)` —— **上游自己的输出**。
而**兜底只动下游的 `inputs`，永远碰不到上游的输出**。
所以这条断言对它宣称要防的那个变异是**免疫**的：
它绿不是因为规则没被破，而是因为它**压根没看那个会被破的地方**。

真正会出事的地方在**下游**：

> 上游辛苦算出了 `image_path`，下游拿到的是上游那个，还是兜底那个 `fallback.png`？

而**那条断言当时一条都没有**。

也就是说：这条规则**看起来**有测试守着（红字写得很到位），
实际上**没有任何一条断言真的守着它**。

### 修法

断言要验**那个会被影响的对象**：

```cpp
// 改之后:验的是**下游拿到的 inputs**(兜底真的会动它)
check(rec->gotInputs.at(1).value("image_path").toString() == "from-upstream.png",
      "上游给了 image_path -> 下游拿到的还是**上游那个**");
```

M55 随即变红（红 1 条）。

### 顺带补的：用例 9

写 M59/M60 的变异时发现另一个洞：
`EngineStepInfo` 的 `nodeIndex` / `uuid` / `output` / `skipped`
这几个字段**一条断言都没有** ——
改错了用例 7 一条都不会红，而主程序恰恰全靠这些字段
（`nodeIndex` 去 `order` 取节点、`uuid` 当每步缓存的键、`output` 撑状态点和日志）。

补了用例 9（20 条断言），专门钉这些字段。

> **「字段没人验」就是「字段没有保护」** —— 那和没抽出来没区别。

### 这条教训的一般形状

> **「注释会骗人，断言不会」—— 这话只对了一半。**
> 断言的**红字**也是人写的，它一样会骗人。
>
> 骗人的不是「断言」这个形式，是「断言声称的对象」和「断言实际读的对象」不是一回事。
>
> 所以断言不能靠红字自证，得靠**变异**：
> 断言宣称守住的规则，得有一条变异能把它打红。
> 打不红的红字，是一句愿望。

详见 `notes/atomic/at-the-message-is-not-the-assertion.md`。

## 十六、另一个安静得多的坑：`std::function` 按值存

写用例 7 时，断言「执行器被调了 3 次」直接红（实际 0 次），**但不崩**。

真凶：

```cpp
using EngineRunner = std::function<bool(...)>;   // ← 按值存
```

`runFrame` 的形参虽然是 `const EngineRunner&`，
但 `std::function` **按值存** —— 传进去的 functor 被拷贝了一份。
测试的假执行器把「收到几次」记在**自己的成员**里 → 记到了**副本**上 → 外面看不到。

修法：记录用的状态放 `std::shared_ptr` 里，functor 拷几份都指向同一块。

> 注意 `[&]` 捕获的 lambda **没这个问题**（捕获的是引用）。
> 有问题的是**有成员状态的 functor**。
> 两种在同一个文件里混着用时最容易看错：
> 「刚才那条 lambda 没事，凭什么这条要改？」

详见 `notes/atomic/at-a-callback-copies-what-you-give-it.md`。

## 十七、这一轮的数字（实测，不是推算）

| | 片 1 之后 | 片 2 之后 |
|---|---|---|
| `run_engine` 套件断言 | 34 | **99** |
| W7 套件 / 断言 | 16 / 1695 | 16 / **1760** |
| 全仓套件 / 断言 | 19 / 2172 | 19 / **2237** |
| 累计变异 | 52 | **66**（本轮 +14：10 杀 4 活） |
| atomic 卡 | 46 | **48** |

> 断言数是**实测**的：逐个套件跑一遍、把 `pass=` 加总
> （Qt 的 `qInfo` 在 Windows 上写 **stderr**，不是 stdout ——
> 一开始只重定向 stdout，结果 14 个套件读到 0 字节，
> 差点得出「W7 只有 315 条断言」的结论）。
>
> 全仓总数 = W7 1760 + W6 477 = **2237**。

门禁：`tools/ci.ps1` → **ALL CHECKS PASSED**（7 项目 / 19 套件 / W5 冒烟 / 源码树零污染）

## 十八、下一步：headless runner 还差什么

| 还缺什么 | 难在哪 |
|---|---|
| **读 JSON → 建图** | `pipelineFromJson()` 现在要一个 `PipelineView*`（QGraphicsScene）—— headless 得能离屏造（offscreen 可以） |
| **CLI 入口** | `main.cpp` 解析 `--run pipeline.json` / `--out result.json`，跑完输出 JSON |

执行内核已经**齐了**（拓扑序 + 合并 + 逐节点执行 + 每步 hook）。
剩下的是「怎么把 JSON 变成 `EngineNode` / `EngineEdge`」和「怎么把 `EngineFrame` 变成 JSON」。

> ⚠ 写 headless 的测试时会再遇到 `std::function` 按值存那个坑
> （假算子要记状态）—— 见第十六节。

