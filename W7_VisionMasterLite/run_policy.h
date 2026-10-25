#pragma once
// ============================================================
// run_policy.h — Run 帧循环的三条"不许 runaway"规则
//
// ============================================================
// 为什么单独拉一个头文件:
//
// 这三条规则原来是**内联在 onRunClicked() 里的一堆魔数**,想测都测不到。
// 而它们恰好是这个项目里最贵的三个决定 —— 每一条都直接对应一次
// 线上级卡死(见下面每条的"实测")。所以把它们变成纯函数:
// 没有 QApplication、没有画布、没有算子,拿到整数进、拿整数出,
// 这样才能用无头测试把它们钉死。
//
// 背景:2026-10-08 用户报"还是容易卡死"。实测结论 ——
//   画布逻辑 3 us/步、渲染 1 ms/帧 @40 节点:画布无辜,不是它。
//   真正的大头是日志:Run 的帧循环每跑一个节点 appendLog 一次,
//   帧数 x 节点数 = 行数;而每 append 一行,processEvents() 就把
//   整个(不断变大的)日志文档重排版 + 重画一次。
//   实测 QTextEdit:append 本身 90 us/行,append+重绘 1545 us/行 ——
//   重绘是 append 的 17 倍,而且每行代价随行数上升(前 1/4 到
//   后 1/4 放大 3.7 倍)。文件夹源 2000 张 x 8 节点 = 16000 行,
//   光重绘就 25 秒以上,而且是超线性的 —— 这就是"跑着跑着就卡死",
//   而且全程不弹任何框,用户只看到窗口不动。
//
// 三条规则分别掐住上面链条的不同一环:
//   1) 帧数封顶   -> 掐住"行数"的源头
//   2) 重绘限频   -> 掐住"每行都重绘"
//   3) 可取消     -> 掐住"用户没法自救"
//
// ⚠ 2026-10-25:本文件现在是**纯 QtCore**了。
//   以前它为了一个 widget 调用拖了 <QPlainTextEdit>:
//       inline constexpr int kLogMaxLines = 5000;
//       inline void configureLogView(QPlainTextEdit* w, int cap = ...);
//   那两个搬去了 log_view.h ——
//   「层」这条边界现在由**编译器**守着:
//   tests/test_pure_layer.cpp 有一个只链 Qt6::Core 的目标 include 了这个头,
//   往这儿塞一个 QWidget 会让那个目标**编不过**。
// ============================================================

#include <QtGlobal>

// 帧数封顶 + 帧协议是**纯 QtCore**,拆到纯头文件里去了 ——
// headless runner(纯 QtCore)也要用它们,而这个头文件拖 QtWidgets。
// 定义只有一份,这里只是把它引进来。
#include "run_limits.h"

namespace w7 {

// ============================================================
// 规则 1:帧数封顶
// ============================================================
//
// ⚠ 定义在 run_limits.h 里(纯 QtCore)—— 那边有完整的理由说明。
//   这里只留个指路,免得下一个人把数字抄一份到别处。
//
//   kMaxRunFrames / clampFrameCount —— 见 run_limits.h。
// ============================================================

// ============================================================
// 规则 2:重绘限频
//
// processEvents() 不是"刷新一下",它是**把挂起的重绘真的画出去**。
// 以前每个节点调一次,于是"每写一行日志就重排版+重画整个日志文档",
// 而这个文档还在不断变大。改成按时间限频:再快也最多每秒 10 次。
//
// 注意限频不等于不抽帧 —— 单帧流水线(节点少、跑得快)通常一次都不
// 触发,视觉上就是"跑完直接出结果",这本来也比逐节点闪烁好。
// 慢流水线(每节点几百毫秒)则每 100 ms 刷一次,用户看得到在动。
//
// 顺带把重绘的**触发次数**也砍掉了:限频之后,一次 Run 里的
// processEvents 次数从"节点数"降成"耗时/100ms"。
// ============================================================
inline constexpr int kUiPumpIntervalMs = 100;

// 距上次抽帧的毫秒数够了才抽一次。
// 用时间而不是步数:因为慢的时候用户最需要看到进度,而步数限频
// 在"每个节点都很慢"时反而会让用户等很久才看到第一次更新。
inline bool shouldPumpUi(qint64 msSinceLastPump) {
    return msSinceLastPump >= kUiPumpIntervalMs;
}

// ============================================================
// 规则 3:可取消 + 禁止重入
//
// 光有上限还不够:2000 帧跑完可能要几分钟,用户不能只能干等。
// 帧循环每轮查一次取消标志,顺手也查一次"还在跑吗"。
//
// 重入是另一类卡死:onRunClicked 里调 processEvents() 之后,
// 用户再点一次 Run,第二次点击会被 processEvents 派发进来 ->
// 整个 onRunClicked 重入一层,两个帧循环叠着跑,共享画布和日志。
// 跑的过程中按 Ctrl+Z 更糟:拓扑序里存的是 OperatorNode* 裸指针,
// 节点被 delete 之后循环下一轮还在拿它调 setStepStatus() ——
// 往已释放内存上写,堆被弄坏,表现就是"莫名其妙卡死/必须杀进程"。
//
// 所以标志设成 enum 而不是两个 bool:两个 bool 迟早会写出
// m_running=true 但 m_cancel=true 这种自相矛盾的状态。
// ============================================================
enum class RunState {
    Idle,      // 没在跑
    Running,   // 在跑
    Cancelling // 用户点了 Stop,当前这一轮跑完就收工
};

// 只有 Idle -> Running 这一步是合法的。
// 任何其他转移都拒绝,调用方靠 false 知道"你已经在跑了/已经停了"。
inline bool beginRun(RunState* s) {
    if (!s || *s != RunState::Idle) return false;
    *s = RunState::Running;
    return true;
}

inline bool isRunning(const RunState& s) {
    return s == RunState::Running || s == RunState::Cancelling;
}

// 点了 Stop。
//
// 只接受 Running -> Cancelling 这一步:已经在 Cancelling 时再点一次
// 返回 false,因为状态没有变化。返回值只代表"状态有没有被这次调用改掉",
// 这样调用方才能拿它判断"要不要打这行提示" —— 不然连点三下 Stop
// 就会打出三行"已请求停止"。
//
// 真正要守住的不变量是:**Idle 时点 Stop 绝不能把状态改成 Cancelling**,
// 否则下次 beginRun 会被自己的状态拒掉,表现是"Stop 点过一次之后
// 再也 Run 不动了"。
inline bool requestCancel(RunState* s) {
    if (!s || *s != RunState::Running) return false;
    *s = RunState::Cancelling;
    return true;
}

inline void endRun(RunState* s) {
    if (s) *s = RunState::Idle;
}

// ============================================================
// 日志控件的配置 —— **不在这里了**
// ============================================================
//
// ⚠ 2026-10-25 搬去了 log_view.h。
//   以前 kLogMaxLines 和 configureLogView(QPlainTextEdit*) 住在这儿,
//   于是这个「纯策略」头文件拖了 <QPlainTextEdit>。
//   搬走之后本文件只剩纯逻辑 —— 整数进、整数出。
//
//   见 log_view.h 开头那段「为什么」。
// ============================================================

} // namespace w7
