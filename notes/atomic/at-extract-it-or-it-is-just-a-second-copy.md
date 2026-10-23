# 「抽成一份」和「复制一份去测」不是一回事

## 现象

W7 里跑一条流水线的逻辑（拓扑排序 + 多前驱输出合并）住在 `mainwindow.cpp`。
而 `mainwindow.cpp` **不在任何测试目标里** —— 它要真窗口 + 整套算子，
建一次目标要好几分钟。

于是这两件事的行为，只有**一次性探针**知道过：

> 2026-10-15 有一次探针，**手抄**了 mainwindow.cpp 的合并逻辑跑出来的：
>
> ```
> 顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue / 5000
> 顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red / 10000
> ```
>
> 两个上游状态点都是绿的、都不报错，只有下游那个数悄悄变了。

## 陷阱

发现「这段逻辑没测试」，最顺手的做法是：**复制一份到测试里测**。

那一瞬间看着特别划算：

- 不用动生产代码（风险低）
- 测试立刻就有东西可测（马上见效）

但那是**第二份拷贝**。

合并逻辑动一下，那份拷贝不会跟着动；
而且**没人会知道它过期了** —— 拷贝不会自己报警。
于是测试绿着，测的却是一个已经不存在的版本。

> 本项目栽过五遍「抄副本」。这一次差点栽第六遍。

## 正确做法：抽成一份，两个调用者

```
run_engine.h          ← 唯一的一份定义
   ├─ mainwindow.cpp  调用它（生产）
   └─ test_run_engine.cpp  调用它（测试）
```

抽出来之后：

- 生产代码和测试**测的是同一份东西**，不可能漂；
- 那份一次性探针可以直接扔了（它验证过的现象，现在有常驻断言）；
- 而且 `run_engine.h` 只依赖 QtCore —— **零 OpenCV、零 QtWidgets** ——
  依赖它的测试目标建得快，还顺带证明了「引擎这一层不需要拖整个 GUI 进来」。

改主程序那一步看着吓人（`mainwindow.cpp` 有三处段错误级别的真 bug 历史），
但实际上只是**搬代码 + 换调用**：

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

## 关键差别：顺序

抽成一份之后，那条「顺序决定胜负」的语义**变成了可测的**：

- 以前：只有一次性探针知道，探针一过期就没人知道；
- 现在：`mergeUpstreamOutputs` 的行为有常驻断言钉着，
  改坏了立刻红。

**这就是「抽成一份」的真正收益** —— 不是多了个测试，
而是把「只有一个人知道的事」变成了「所有人都在用同一份、而且它坏不了」。

## 教训

> **发现一段逻辑没测试时，先问「我能把它抽成一份吗」，
> 而不是「我能抄一份来测吗」。**
>
> 前者的收益是永久的，后者的收益是**借来的** —— 借到代码不再变的那一刻。

推论：

> 「一次性探针」是**证据**，不是**测试**。
> 它能让你发现一个洞，但它填不上那个洞 ——
> 填洞的唯一办法是让那段逻辑变成**能直接被测的那一份**。

参见 `notes/atomic/at-dont-replicate-the-guard.md`
（别复刻守卫去回答测量问题）、`notes/atomic/at-measure-the-thing-not-the-mark.md`。
