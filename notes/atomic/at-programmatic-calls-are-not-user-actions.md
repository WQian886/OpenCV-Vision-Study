# 程序化调 API，不等于用户操作

## 核心

2026-10-25，W7 把 `mainwindow.cpp` 拉进测试目标时踩到的。

新套件要「选中一个节点」，好让主窗口切参数面板。测试里写的是：

```cpp
node->setSelected(true);
QApplication::processEvents();
```

看起来就是"选中节点"，和用户点一下**应该是一回事**。

## 但它**不是**一回事

用户点一下节点时，`QGraphicsView`（SingleSelection 模式）会做两件事：

1. **取消上一个选中**
2. 选中点到的那个

而 `QGraphicsItem::setSelected(true)` **只做第 2 件事**。
第 1 件事是**视图**在处理鼠标事件时做的 —— 程序化调用**绕过了那一层**。

于是场景里**同时有两个选中**，而主窗口是这么挑节点的：

```cpp
for (auto* item : selected) {
    if (auto* n = dynamic_cast<OperatorNode*>(item)) { node = n; break; }
}
```

**取第一个**。而"第一个是谁"取决于场景内部的顺序 —— **不确定**。

## 症状：测试**时绿时红**

```
run 1 exit=0
run 2 exit=0
run 3 exit=1     ← 红
run 4 exit=0
run 5 exit=1     ← 红
run 6 exit=1     ← 红
```

而且红的时候**报的错和真正的原因对不上**：
断言写的是「选中没装的节点时，面板不该显示上一个节点的表单」，
实际红的原因是「面板显示了**另一个已知节点**的表单」。

> **一个时绿时红的测试，最容易让人怀疑产品不稳定。**
> 而这一次的真相是：**产品是稳的，是测试自己制造了一个产品不会有的状态。**

## 修法：先复现用户动作的**全部**步骤

```cpp
static void selectNode(MainWindow& w, OperatorNode* node) {
    canvas->scene()->clearSelection();   // ← 补上「取消上一个选中」
    QApplication::processEvents();
    node->setSelected(true);
    QApplication::processEvents();
}
```

补上那一步之后，事件顺序就确定了，连跑 12 次全绿。

## 教训

> **「程序化调 API」和「用户操作」差着的那部分，就是测试和产品的差距。**
>
> 凡是**界面层**的 API，先问一句：
> **用户做同一个动作时，界面上还会发生什么？**
> 那些"还会发生的事"往往就是**取消选中**、**取消焦点**、
> **重置滚动位置** —— 它们在 API 里**不存在**，只存在于交互里。

### 推论一：产品代码里可能有同样的洞

这一次是测试踩到的。但同一个洞在**产品**里也在：

```cpp
m_canvas->scene()->clearSelection();   // 用户点空白处
```

这条路径是**真的**会走的。它有没有可能让面板显示出**另一个节点**的表单？
—— 有同款风险的地方，就得查。

### 推论二：「没法测」和「没测」是两回事

`mainwindow.cpp` 一直用「它要 QWidget」当借口不进测试目标。
这一轮证明那个借口是假的：
**起一个 offscreen 的 `QApplication`，照样能把主窗口跑起来。**

> 真正难的不是「能不能测」，是「**测什么**」。
>
> 而「测什么」这个问题有现成的答案（本项目早就定了）：
> **「错了用户会不会自己发现」**——
> 发现得了的（画歪了、按钮没排好）不用测；
> 发现不了的（画面正常但数字对不上）**必须**测。

### 推论三：抖动的测试是**信号**，不是噪音

一个时绿时红的测试，第一嫌疑不是"环境不稳"，是：
1. **测试自己制造了状态**（这一次的真相）
2. **测试依赖了一个不确定的顺序**（"取第一个"）
3. 真的产品有竞态（最罕见，但最贵）

先怀疑前两个 —— 它们是**测试的问题**，改测试就行；
第三个是**产品的问题**，要改产品。

> 而一个**会抖**的测试最坏的地方是：
> 它让人**习惯**了"这个测试偶尔会红"，
> 于是真出问题时也被当成"又抖了"。

## 配套

- `notes/atomic/at-the-state-and-the-screen-must-agree.md` —— 状态和画面对不上
- `notes/atomic/at-cannot-be-tested-is-a-conclusion-not-a-reason.md` —— 「没法测」是结论，不是理由
- `notes/atomic/at-test-leaves-residue-permanently-red.md` —— 测试自己制造的失败

参见 `notes/notes_W7_MainWindow_2026-10.md`。
