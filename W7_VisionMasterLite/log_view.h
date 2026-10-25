#pragma once
// ============================================================
// log_view.h —— 日志控件的配置(唯一拖 QtWidgets 的那一个文件)
// ============================================================
//
// ⚠ 2026-10-25:以前这两个东西住在 run_policy.h 里:
//
//     inline constexpr int kLogMaxLines = 5000;
//     inline void configureLogView(QPlainTextEdit* w, int cap = kLogMaxLines);
//
//   于是 **run_policy.h 拖了 <QPlainTextEdit>** ——
//   那个头文件是"Run 帧循环的三条不许 runaway 规则",
//   规则本身是纯的(整数进、整数出),却为了一个 widget 调用
//   把整个 QtWidgets 拖了进来。
//
//   后果:
//     · run_policy.h 的每一个使用者都被迫依赖 QtWidgets
//     · 纯策略层想被一个零 Widgets 的目标 include,就得先妥协
//     · 「层」这条边界只剩注释在说 —— 没有任何东西拦着下一个人
//       再往里塞一个 QWidget
//
//   所以把 widget 的那一小块**单独拎出来**放在这个文件里。
//   现在:
//     run_limits.h   纯 QtCore —— 帧数封顶 + 帧协议
//     run_policy.h   纯 QtCore —— 重绘限频、取消、重入
//     log_view.h     **这里** —— 唯一和 QPlainTextEdit 有关系的地方
//
// ⚠ 边界不是靠注释守的,是靠**编译器**守的:
//   tests/test_pure_layer.cpp 有一个只链 Qt6::Core 的目标,
//   它 include 了 run_limits.h / run_policy.h / frame_verdict.h /
//   run_engine.h / pipeline_graph.h / result_render_plan.h ——
//   哪天有人往那些纯头文件里塞一个 QWidget 或 opencv2,
//   **那个目标编不过**,CI 直接红。
//   ⚠ 这比源码 lint 强:lint 查的是文本,编译器查的是**依赖**。
//     (lint 会被注释骗 —— 见 notes/atomic/at-the-guard-can-be-fed-by-a-comment.md)
//
// ============================================================

#include <QPlainTextEdit>

namespace w7 {

// ============================================================
// 日志控件的配置
// ============================================================
//
// 为什么单独拉出这个函数(而不是在 setupResultPanel 里直接写两行):
//
// 主窗口没法无头构造(要真窗口 + 全部算子),所以第一版的测试是
// "自己 new 一个 QPlainTextEdit,设上限,写几千行,看块数有没有被卡住"。
// 那个测试只证明了**机制**成立(setMaximumBlockCount 确实管用),
// 但完全没证明**主窗口真的调了它** —— 把 mainwindow.cpp 里那行删掉,
// 测试照样全绿。这就是"测了个寂寞"的另一种形态:测的是平行宇宙。
//
// 所以把配置收进这个函数,主窗口和测试调的是同一段代码。
// 剩下的缺口只有"主窗口忘了调这个函数"这一行 ——
// 那条现在由 test_run_policy.cpp 的源码守卫盯着
// (它查的是"主窗口**必须调** w7::configureLogView",不是"某种写法不许出现")。
//
// cap 参数只为测试提速而存在:测"代价曲线形状"时用小上限跑得快,
// 测"真实上限"时用默认值。
// ============================================================
inline constexpr int kLogMaxLines = 5000;

inline void configureLogView(QPlainTextEdit* w, int cap = kLogMaxLines) {
    if (!w) return;
    w->setReadOnly(true);
    // 不封顶的话文档只涨不删,内存和单次重绘代价一起涨。
    // 实测单行重绘 785 us 且随行数上涨,行数不封顶 = 代价不封顶。
    w->document()->setMaximumBlockCount(cap);
}

} // namespace w7
