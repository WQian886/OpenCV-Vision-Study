#pragma once
// ============================================================
// run_limits.h —— Run 帧循环的"不许 runaway"限额(纯算术)
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
//     run_limits.h   纯算术(QtCore 甚至不依赖)—— 帧数封顶
//     run_policy.h   要 GUI —— 重绘限频、取消
//
//   **一份定义,两个调用者** —— 限额只有一个数,不能有两处。
//
// ============================================================

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

} // namespace w7
