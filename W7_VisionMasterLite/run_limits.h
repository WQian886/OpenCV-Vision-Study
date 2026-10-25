#pragma once
// ============================================================
// run_limits.h —— Run 帧循环的"不许 runaway"限额 + 帧协议(纯 QtCore)
// ============================================================
//
// ⚠ 为什么从 run_policy.h 里拆出来:
//   run_policy.h 里还有"重绘限频"和"可取消"两条规则,
//   它们要用 QPlainTextEdit / QApplication —— 于是那个头文件拖了 QtWidgets。
//
//   而这一条(帧数封顶)是**纯算术**:整数进、整数出。
//   headless runner 要用它,但 headless 那一层是**纯 QtCore** ——
//   拖 QtWidgets 进去就等于把整个 GUI 拖回测试目标。
//
//   所以拆成两个头文件:
//     run_limits.h   纯 QtCore —— 帧数封顶 + **帧协议**
//     run_policy.h   要 GUI —— 重绘限频、取消
//
//   **一份定义,两个调用者** —— 限额只有一个数,不能有两处。
//
// ⚠ 2026-10-25:本文件**多了 QJsonObject**。
//   因为"帧协议"(源怎么报帧数)也搬进来了 ——
//   以前 GUI 和 headless 各写一遍"扫描 outputs 找 frame_count",
//   那是本项目栽过**九遍**的「抄副本」的**第十遍**。
//   协议是**数据形状**,不是算术,所以这里现在需要 QtCore(而不是纯 C++)。
//   ⚠ 但它**仍然不碰 QtWidgets、不碰 OpenCV** ——
//     这一条现在有**编译期**的约束守着(见 tests/test_pure_layer.cpp):
//     一个只链 Qt6::Core 的目标 include 了本文件,
//     哪天有人往这儿拖一个 QWidget,那个目标**编不过**。
//
// ============================================================

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace w7 {

// ============================================================
// 规则 1:帧数封顶
// ============================================================
//
// 以前 totalFrames 是直接拿源的 frame_count 当循环上界的,而
// 文件夹源给的就是目录里的文件数(entryList.size())。
// 指向一个大目录 = 几万轮同步跑,没有进度、没有取消、没有上限。
// 循环上界是数据给的,而数据不受控 —— 这是个设计错误,不是参数没调好。
//
// 2000 帧是个刻意选的数:8 节点跑 2000 帧已经要跑很久,再往上
// 用户基本不可能是想一次跑完,更可能是选错了目录。
// 关键是**超了要说清楚**,静默截断会让人以为全跑完了。
// ============================================================
inline constexpr int kMaxRunFrames = 2000;

// 源可能报 0(目录空)、负数(参数填错),一律当 1 帧,
// 否则循环一次都不进,用户看到"Run 没反应"。
// 超过 kMaxRunFrames 就截断,并由 truncated=true 告诉调用方去日志里说明。
inline int clampFrameCount(int reported, bool* truncated = nullptr) {
    if (truncated) *truncated = false;
    if (reported < 1) return 1;              // 0 / 负数 -> 至少跑第 0 帧
    if (reported > kMaxRunFrames) {
        if (truncated) *truncated = true;
        return kMaxRunFrames;
    }
    return reported;
}

// ============================================================
// 帧协议:源怎么报「一共有多少帧」
// ============================================================
//
// ⚠ 这一份以前在**两个地方各写一遍**(第十份抄副本):
//
//   mainwindow.cpp    帧循环首帧之后
//       for (const QJsonObject& out : frameOutputs) {
//           if (out.contains("frame_count")) { ... clampFrameCount ... }
//       }
//
//   headless_runner.cpp  同一件事
//       for (const QJsonObject& o : ef.outputs) { ... 一模一样 ... }
//
//   两边现在**碰巧**一致(都按执行序扫、都取第一个),
//   但没有任何东西保证它们一致 ——
//   哪天源算子改了 frame_count 的语义,两边会各按各的理解算,
//   而**两边都是绿的**。
//
//   这就是本项目记过的那句话:
//   「两份一模一样的东西就是两份会各自漂的东西。」
//
// ⚠ 协议本身只有三条,写下来就完了:
//   1. 源在**输出**里报一个 `frame_count` 字段
//   2. **第一个**带这个字段的输出说了算(按执行序)
//   3. 报出来的数要过 clampFrameCount(小于 1 当 1,超过上限截断)
//
// ⚠ 「第一个」是**执行序**里的第一个,不是文件里的顺序 ——
//   两个调用方的输出数组都是按执行序排的,所以规则一样。
//
// ⚠ reportedOut 给的是**原始报出来的数**,不是截断后的 ——
//   截断提示要说的是"一共有多少帧",不是"我们跑了多少帧"。
// ============================================================
inline int resolveFrameCount(const QVector<QJsonObject>& outputs,
                             int* reportedOut = nullptr,
                             bool* truncated = nullptr) {
    if (reportedOut) *reportedOut = 1;
    if (truncated)   *truncated = false;

    for (const QJsonObject& o : outputs) {
        if (!o.contains(QStringLiteral("frame_count"))) continue;
        const int reported = o.value(QStringLiteral("frame_count")).toInt(1);
        bool t = false;
        const int clamped = clampFrameCount(reported, &t);
        if (reportedOut) *reportedOut = reported;   // 原始值(截断提示要用它)
        if (truncated)   *truncated = t;
        return clamped;
    }
    // 一个输出都没报帧数 -> 当 1 帧(循环至少进一次)
    return 1;
}

} // namespace w7
