# 源码守卫会被注释喂饱

## 核心

2026-10-25，W7 旋钮审计还欠账 17 的第二半时遇到的。

那一轮写了一条**源码守卫**，守住「主窗口必须走共享的那份规则」：

```cpp
check(src.contains("w7::planResultRender"),
      "mainwindow.cpp 走的是 w7::planResultRender(...)");
```

看着挺合理：查源码里有这个可测入口。

然后变异验证给出了一条 `Expect = KILL` 的变异：

> **M154**：把主窗口里那行调用换成 `w7::RenderPlan()` ——
> 也就是**面板彻底不用共享规则了**。

实测：**SURVIVED**。62 条断言一条没红。

## 为什么没红

因为 `mainwindow.cpp` 的**注释里**就写着这句话：

```cpp
//   现在两边都用 w7::planResultRender / w7::resultImageCandidates,
//   **必然一致**。详见 result_render_plan.h 开头。
const w7::RenderPlan plan = w7::planResultRender(out, fallbackPath);
```

M154 把**代码**那行换掉了，**注释**那行还在。
守卫查的是 `src.contains(...)` —— 而注释也是 `src` 的一部分。

> **守卫被一句注释喂饱了。**
> 变异把真调用删干净了，守卫照样绿。

## 这个项目的老话只对了一半

> 「注释会骗人，断言不会。」

这次发现：**源码守卫也会被注释骗。**

骗人的不是「守卫」这个形式，是
**「守卫查的东西」和「守卫要量的东西」不是一回事**：

- 它要量的是：**代码里**走的是共享入口
- 它量的是：**文本里**出现过那个词

差一个字，差的就是「一句注释」。

（和 `at-the-message-is-not-the-assertion.md` 那条同源：
断言声称的对象和断言实际读的对象不是一回事。）

## 修法

**查源码之前先剥掉注释。**

```cpp
static QString stripComments(const QString& src) { /* 认 // 和 /* */ */ }
const QString code = stripComments(src);   // ⚠ 只查代码
check(code.contains("w7::planResultRender"), "...");
```

两个坑：

1. **剥注释本身要被测。**
   否则下一个人把 `stripComments` 改成 `return src;`，
   守卫就又变回空转 —— 而且这次是**两处**同时坏。
   所以要有一条**自带样本**的断言：
   喂一段「注释里有那句话、字符串里有 `//`」的样本进去，
   断言剥完之后那句话不见了、而字符串里的 `//` 还在。

2. **字符串里的 `//` 和 `/*` 不是注释。**
   剥错了同样会骗人（只是骗的方向反过来）。

## 推论：守卫的判据该怎么写

> 守卫应该问「**代码里必须出现某个可测入口**」，
> 而不是「文本里出现过某个词」。

更一般的说法：

| 写法 | 会被什么喂饱 |
|---|---|
| `src.contains("w7::Foo")` | **注释**、字符串字面量 |
| `src.contains("out.value(\"mask_path\")")` | 等价改写（`out["mask_path"]`） |
| `stripComments(src).contains(...)` | 注释（这一类堵住了） |

所以本项目的源码守卫一律走「**剥掉注释再查**」，
而且判据尽量落在**可测入口的名字**上（`w7::planResultRender`、`w7::pickBaseImage`），
而不是某个具体写法。

## 检查清单

- [ ] 这条守卫查的是**代码**，还是**文本**？
- [ ] 目标文件里有没有**注释**会满足它？
- [ ] 剥注释这个动作本身**被测了吗**？
- [ ] 判据落在**可测入口**上，还是落在**某种写法**上？

## 配套

- `notes/atomic/at-the-message-is-not-the-assertion.md` —— 断言声称的对象 vs 实际读的对象
- `notes/atomic/at-a-lint-that-finds-nothing-looks-green.md` —— 扫到 0 个对象，样子和通过一模一样
- `notes/atomic/at-zero-violations-is-not-proof-the-rule-works.md` —— 零违规不能证明规则在干活
- `notes/atomic/at-mutation-ruler-broken.md` —— 变异活下来先怀疑尺子

参见 `notes/notes_W7_RenderPlan_2026-10.md`。
