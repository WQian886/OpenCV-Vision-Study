#pragma once
// ============================================================
// frame_verdict.h —— 一帧判定了什么 / 整批结论是什么(纯逻辑)
// ============================================================
//
// ⚠ 为什么要有这个文件:
//
//   "跑完一条流水线,结论到底是什么"这件事,以前有**两份**:
//     · mainwindow.cpp 里的 runPipeline —— 给人看的那份
//     · headless_runner.cpp 里的 runPipelineJson —— 给 CI 看的那份
//
//   两份**不是**同一件事,于是它们**长歪了**。
//   2026-10-24 结项时对账,发现 headless 那份多两条规则:
//
//     规则 1:一帧里有步骤报错 -> 这一帧既不算 PASS 也不算 FAIL
//     规则 2:整条流水线有步骤报错 -> 最终结果强制说 ERROR
//
//   而 mainwindow 那份**两条都没有**。
//   后果很具体:同一条流水线,一个步骤报了错(比如读不到配方文件),
//   判定节点照样说 PASS ——
//     GUI 说「🎉 最终结果: PASS」
//     headless 说「final_result: ERROR」,退出码 3
//
//   **两份都说得很有底气,结论却相反。**
//   产线上判废看的是 GUI 那份,于是它是错的那一份。
//
//   这不是"主程序忘了加两条规则"这么轻 ——
//   这是本项目栽过**五遍**的「抄副本」,第六次,
//   而且**已经漂了**。
//
// ⚠ 为什么抽成纯头文件:
//
//   mainwindow.cpp 不在任何测试目标里(它要真窗口 + 整套算子,
//   建一次目标要好几分钟)。而这件事恰恰**有真实语义**:
//   "结论该怎么说"是产线判废的依据,不能只有源码 lint 看着。
//
//   所以按片 3 / 片 2 的老办法:抽成**一份**纯逻辑,
//   两边都用它,再给一份**行为测试**。
//
// ⚠ 依赖面:
//
//   只依赖 QtCore(QJsonObject / QString / QVector)——
//   **零 QtWidgets、零 OpenCV** ——
//   所以依赖它的测试目标建得快,也能证明
//   "判定逻辑不需要拖整个 GUI 进来"。
//
// ⚠ 这份文件**不许**改判定口径:
//
//   它编码的是 headless 那份**已经有的**口径 ——
//   那份是有测试钉着的(见 tests/test_headless_runner.cpp)。
//   抽出来是为了让 GUI **跟上**,不是为了重新设计。
//   真要改口径,那是另一次改动,得连着改测试和两边。
// ============================================================

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace w7 {

// ============================================================
// 一帧的判定
// ============================================================
//
// ⚠ 为什么坏帧要单列一个 Error,而不是算成 PASS 或 FAIL:
//
//   "算子跑成功了但报了个错"(读不到配方文件、图像读不出来)
//   —— 它**既没通过,也没失败**,它是**跑坏了**。
//
//   算成 PASS 是最贵的那类静默失效:
//   数字看着正常,结论却是错的。
//   抽检 30 帧里第 7 帧跑坏了,末帧正常 ——
//   只看末帧就是漏检,而产线判废看的是整批。
enum class FrameVerdict {
    Unknown,   // 这一帧没有判定(没有 result 字段,也没报错)
    Pass,      // 判定通过
    Fail,      // 判定不通过
    Error,     // 这一帧里有步骤报错 —— 跑坏了,不算通过也不算不通过
};

// ------------------------------------------------------------
// 分类一帧
// ------------------------------------------------------------
//
// result     这一帧的 result 串("PASS" / "FAIL" / 空)
// frameErrors 这一帧里**输出带 error** 的步数
//
// ⚠ 判据的顺序不能换:
//   先看有没有报错,再看 result。
//   反过来的话,一个"报了错但也给了 PASS"的帧会被算成 PASS。
inline FrameVerdict classifyFrame(const QString& result, int frameErrors) {
    if (frameErrors > 0) return FrameVerdict::Error;
    if (result == QStringLiteral("PASS")) return FrameVerdict::Pass;
    if (result == QStringLiteral("FAIL")) return FrameVerdict::Fail;
    return FrameVerdict::Unknown;
}

// ============================================================
// 逐帧累计
// ============================================================
//
// ⚠ lastResult 存的是**末帧的原始 result 串**,不是判定:
//   因为末帧可能压根没有判定(空串),那结论是 UNKNOWN,
//   而不是"回退到上一帧那个 PASS"。
//   回退会让用户以为最后一帧判过了。
struct FrameTally {
    int frames = 0;      // 跑完的帧数
    int pass   = 0;      // 判 PASS 的帧数
    int fail   = 0;      // 判 FAIL 的帧数
    int error  = 0;      // 跑坏了的帧数
    QString lastResult;  // 末帧的原始 result 串

    void addFrame(const QString& result, int frameErrors) {
        ++frames;
        lastResult = result;
        switch (classifyFrame(result, frameErrors)) {
        case FrameVerdict::Pass:  ++pass;  break;
        case FrameVerdict::Fail:  ++fail;  break;
        case FrameVerdict::Error: ++error; break;
        case FrameVerdict::Unknown: break;   // 没判定的帧不计入任何一类
        }
    }
};

// ============================================================
// 挑出这一帧的判定输出
// ============================================================
//
// 规则:**最后一个跑了而且带 result 字段的节点**的输出。
// 不是"第一个",也不是"最后一个跑的"(那个可能压根没有 result)。
//
// ⚠ 为什么要抽出来:
//   mainwindow 和 headless 以前各写了一遍,一模一样。
//   两份一模一样的东西就是两份会各自漂的东西 ——
//   本次就已经漂了(见本文件开头的规则 1 / 规则 2)。
//
// ⚠ finalIndex 是**最后一个跑了的**节点下标(不管它有没有 result)——
//   没有 result 时用它兜底。调用方要的"末帧"就是它。
inline QJsonObject pickFrameResult(const QVector<QJsonObject>& outputs,
                                   const QVector<int>& executed,
                                   int* finalIndex) {
    QJsonObject result;
    int last = -1;
    for (int idx : executed) {
        // 越界的下标直接跳过 —— 以前这里是裸取,越界就是崩。
        // 引擎保证 executed 里的下标有效,但这份逻辑现在**自己**保证。
        if (idx < 0 || idx >= outputs.size()) continue;
        last = idx;
        const QJsonObject& o = outputs.at(idx);
        if (o.contains(QStringLiteral("result"))) result = o;
    }
    // 一个带 result 的都没有 -> 退到最后一个跑了的节点的输出。
    // 这一步不能省:判定节点可能压根没跑(被跳过/未注册),
    // 那时输出是空的,但下游还有别的东西,总得给调用方一个能看的对象。
    if (!result.contains(QStringLiteral("result")) && last >= 0)
        result = outputs.at(last);
    if (finalIndex) *finalIndex = last;
    return result;
}

// ============================================================
// 整批结论
// ============================================================
enum class BatchVerdict {
    Passed,     // 全批判 OK
    Failed,     // 有帧判废
    Errored,    // 有帧跑坏了 —— 结论不完整,**不许**说"全批 OK"
    Cancelled,  // 被手动中止 —— 结论不完整
    Broken,     // 这一批**压根没跑完**(引擎拒绝跑)—— 没有任何结论
};

// ------------------------------------------------------------
// classifyBatch
// ------------------------------------------------------------
//
// t          逐帧累计
// cancelled  用户有没有中途 Stop
//
// ⚠ 优先级不能换:
//
//   Cancelled > Errored > Failed > Passed
//
//   Cancelled 最高:中止的批次**不是整批结论**,
//   哪怕跑完的那几帧全 PASS ——
//   用户 Stop 掉一个 2000 帧的批次,恰好前 3 帧都 PASS,
//   报"全批 3 帧判 OK"看起来像整批合格,而剩下 1997 帧根本没跑。
//
//   Errored 排在 Failed 前面:跑坏的帧比判废的帧更"不成结论" ——
//   判废至少是**跑完了并且判了**,跑坏是**连判都没判成**。
//
// ⚠ 最要紧的一条:Errored 存在时**绝不能**返回 Passed。
//   "全批 N 帧判 OK"这句话的每一个字都必须是真的。
//
// ⚠ 为什么**不看**总帧数:
//   判据是"有没有帧判废 / 有没有帧跑坏",不是"有几帧"。
//   一帧的批次也走同一套 —— 一帧判废了就是 Failed。
//   「要不要显示批量汇总」是**展示**问题(主程序那边管),
//   「这一批是什么结论」是**判定**问题,两者不能混。
inline BatchVerdict classifyBatch(const FrameTally& t, bool cancelled) {
    if (cancelled)          return BatchVerdict::Cancelled;
    if (t.error > 0)        return BatchVerdict::Errored;
    if (t.fail > 0)         return BatchVerdict::Failed;
    return BatchVerdict::Passed;
}

// ------------------------------------------------------------
// 结论的英文名(JSON / 退出码用)
// ------------------------------------------------------------
//
// ⚠ 主程序那边用的是中文文案,不走这个 ——
//   那是展示层,各写各的没关系。
//   这里给的是**机器读**的名字(结果 JSON、CI 退出码)。
inline QString batchVerdictName(BatchVerdict v) {
    switch (v) {
    case BatchVerdict::Passed:    return QStringLiteral("PASS");
    case BatchVerdict::Failed:    return QStringLiteral("FAIL");
    case BatchVerdict::Errored:   return QStringLiteral("ERROR");
    case BatchVerdict::Cancelled: return QStringLiteral("CANCELLED");
    case BatchVerdict::Broken:    return QStringLiteral("BROKEN");
    }
    return QStringLiteral("UNKNOWN");
}

// ============================================================
// 末帧的原始判定
// ============================================================
//
// ⚠ 为什么单独给一个:
//   finalResultOf 里有一条"有步骤报错就强制 ERROR"。
//   但中途失败(引擎拒绝跑)的时候,结果 JSON 照样要写 ——
//   排查的人要靠它才说得清"跑到哪儿了"。
//   那时候**不该**被"有错就 ERROR"盖住末帧的判定:
//   要看的是"末帧判了什么",不是"整批是什么结论"。
//
//   所以分两个:
//     lastResultOf  —— 末帧判了什么(空 -> UNKNOWN)
//     finalResultOf —— 整条流水线的结论(含"有错就 ERROR"这条规则)
inline QString lastResultOf(const FrameTally& t) {
    if (t.lastResult.isEmpty()) return QStringLiteral("UNKNOWN");
    return t.lastResult;
}

// ============================================================
// 最终结果串
// ============================================================
//
// ⚠ 规则 2 在这里:有步骤报错 -> 强制 ERROR。
//
//   "判定节点说 PASS"不代表"整条流水线跑成了" ——
//   上游报过错,那个 PASS 就是**空头**的。
//
//   而"空头的 PASS"是最坏的一种:
//   它不是没结论,它是**一个看起来正常的错结论**。
//   CI 那边就是靠这一条才没把跑坏的流水线当成过的。
//
// ⚠ 末帧没判定(result 空) -> UNKNOWN。
//   UNKNOWN 和 PASS 是两回事:
//   前者是"没判",后者是"判过了"。
//   合成一个码的话,CI 就分不清该修工具还是修流水线,
//   而"分不清"在实践中就等于"没人修"。
inline QString finalResultOf(const FrameTally& t) {
    if (t.error > 0) return QStringLiteral("ERROR");
    return lastResultOf(t);
}

} // namespace w7
