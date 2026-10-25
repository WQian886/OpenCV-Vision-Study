# 能交给编译器守的边界，就别交给 lint

## 核心

2026-10-25，W7 旋钮审计还欠账 17 的后续里遇到的。

W7 有一条分层边界：**纯逻辑不许拖 widget**。
这条边界以前**只写在注释里** ——

- `run_limits.h` 自己写着「纯算术(QtCore 甚至不依赖)」
- `run_policy.h` 自己写着「没有 QApplication、没有画布、没有算子」
- `pipeline_graph.h` 自己写着「**零 OpenCV、零 QtWidgets**」

然后 `run_policy.h` 自己就拖了个 `<QPlainTextEdit>` ——
为一个只有一行的 widget 调用，把整个 QtWidgets 拖进「纯策略层」。

> **没有任何东西拦着。**
> 注释写着「零 QtWidgets」，代码里就有一个 QPlainTextEdit，
> 而两边都是绿的。

## 为什么 lint 守不住这条边界

本项目用源码 lint 守了很多年。它的判据都是「文本里有没有某个东西」：

```cpp
check(!src.contains("opencv2"), "...");
```

而 lint 有三个已知的漏法（本项目全栽过）：

| 漏法 | 现场 |
|---|---|
| **注释喂饱它** | `contains("w7::planResultRender")` 被 mainwindow.cpp 的**注释**满足 —— 变异把真调用删干净了，守卫照样绿 |
| **等价改写绕过** | `out.value("mask_path")` 改成 `out["mask_path"]`，同一个意思，lint 看不见 |
| **搬家不红** | 规则从「查这个字符串」搬到「查那个字符串」，两条 lint 都还绿 |

**编译器没有这三个漏法。**
它查的是**依赖**，不是文本：注释骗不了它，等价改写骗不了它，
搬家会直接让它**编不过**。

## 怎么把边界交给编译器

让一个**只链最窄依赖**的目标 include 那些头文件：

```cmake
target_link_libraries(w7_pure_test PRIVATE Qt6::Core)   # 只有 Core
```

```cpp
// tests/test_pure_layer.cpp
#include "run_limits.h"        // 纯
#include "run_policy.h"         // 纯
#include "frame_verdict.h"      // 纯
#include "run_engine.h"         // 纯
#include "pipeline_graph.h"     // 纯
#include "result_render_plan.h" // 纯
#include "step_frame_cache.h"   // 纯
#include "frame_record.h"       // 纯
#include "headless_runner.h"    // 纯
```

后果：**任何一处纯头文件里多了一个 QWidget / opencv2，那个目标就编不过。**

边界从此不是「文档里的一句承诺」，而是**构建会拒绝的东西**。

## ⚠ 两个坑

### 一、闸本身也会被拆掉

如果有人往那个 target 的 `target_link_libraries` 里加上 `Qt6::Widgets`，
闸就没了 —— 而套件**照样全绿**（它只是又拖进来一个 widget 而已）。

所以还得有一条**守住闸的形状**的守卫：

```cpp
check(!cleaned.contains("Qt6::Widgets"), "那个 target 没有链 Qt6::Widgets(那会把闸拆掉)");
```

> **守卫自己被改掉**，也是「抄副本」的一种 ——
> 而且是最隐蔽的一种,因为它改的是**闸**不是**被闸守的东西**。

### 二、失败发生在**构建**那一步,不是测试那一步

破坏分层时，失败是「编不过」，不是「断言红了」。
第一反应容易是「环境坏了」。

所以要配一条**读得懂**的守卫，把同一件事用中文再说一遍，
让下一个人的第一反应变成「哦，是我把 widget 拖进纯层了」。

## 教训

> **能交给编译器守的边界，就不要交给 lint。**
>
> lint 是「文本里有没有」，编译器是「依赖里有没有」。
> 后者强一个量级，而且**不会被注释骗、不会被等价改写骗**。
>
> 一般式：
> - 判据是**文本**的 → 用 lint（而且要先剥注释）
> - 判据是**依赖**的 → 用**编译期约束**（一个只链最窄依赖的目标）
> - 判据是**行为**的 → 用断言

## 参见

- `notes/atomic/at-the-guard-can-be-fed-by-a-comment.md` —— lint 的第一个漏法
- `notes/atomic/at-the-message-is-not-the-assertion.md` —— 断言声称的对象 vs 实际读的对象
- `notes/atomic/at-a-lint-that-finds-nothing-looks-green.md` —— 扫到 0 个对象，样子和通过一模一样
- `notes/atomic/at-mutation-ruler-broken.md` —— 变异活下来先怀疑尺子
- `notes/atomic/at-the-copies-had-already-drifted.md` —— 抄副本的十次现场

参见 `notes/notes_W7_FrameProtocol_2026-10.md`。
