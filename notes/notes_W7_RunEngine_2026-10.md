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
