#pragma once
// ============================================================
// headless runner —— 跑一条流水线,不弹窗
// ============================================================
//
// 为什么要有这个文件:
//   W7 以前只能开窗跑。CI 跑的全是单元测试,
//   **从来没跑过一条完整流水线** ——
//   也就是说"一条流水线从头到尾能不能跑通"这件事,一直没有答案。
//
// 这一层做的事:
//   JSON -> 图(解析)-> 引擎(执行)-> 结果 JSON
//
// ============================================================
// ⚠ 依赖面:只依赖 QtCore
// ============================================================
//
// **零 OpenCV、零 QtWidgets、零画布**。
//
// 这是刻意的:
//   · 引擎(run_engine.h)那一层已经证明了"执行逻辑不需要 GUI",
//     这一层继续证明"连**读盘**都不需要 GUI";
//   · headless 路径要是拖着 QGraphicsScene,
//     它的测试就得造离屏窗口、建得慢、还多一份环境依赖;
//   · headless runner 的测试套件因此是**纯 QtCore** ——
//     建得快(几秒)、不需要显示器、CI 里跑得稳。
//
// 代价是:**算子怎么跑、算子认不认识,得由调用方传进来**
// (factoryFn / defaultsFn / runner 三个回调)。
// 那三个回调在真程序里由 operator_factory 提供,需要 OpenCV ——
// 但**这一层本身**不需要。测试里塞假算子就行。
//
// ============================================================
// 三条不变式
// ============================================================
//
// 1. **和主程序跑的是同一份引擎**
//    (w7::runFrame / w7::topoOrder / w7::reindexEdges)——
//    headless 不是"另一套执行逻辑",是同一个引擎换个驱动方式。
//    否则两边迟早漂,而两边都是绿的。
//
// 2. **有环不跑**
//    宁可什么都不跑,也不要跑一半 ——
//    跑一半的话下游拿到的是上一帧的输出,数字看着正常,结论却是错的。
//
// 3. **跳过的必须报出来**
//    未知算子、端点缺失、算子失败 —— 一条都不许悄悄咽下去。
//    静默失效比崩溃更难查。
//
// ============================================================

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "pipeline_graph.h"   // OpDefaults / PipelineGraph / graphFromJson
#include "run_engine.h"       // EngineNode / EngineEdge / topoOrder / runFrame
#include "frame_verdict.h"    // BatchVerdict / FrameTally(判定口径,**和主程序共用**)

namespace w7 {

// ============================================================
// 选项
// ============================================================
struct HeadlessOptions {
    // 图路径。留空 = 用 JSON 里带的那个。
    //
    // ⚠ 它是**兜底**:上游自己产出了 image_path 就不用它
    //   (和主程序的行为一字不差)。
    QString imagePath;

    // 最多跑几帧。<= 0 = 不额外加限(由 kMaxRunFrames 管)。
    //
    // ⚠ 源报多少帧不是"照单全收" —— clampFrameCount 会先压一道
    //   (文件夹源指到大目录就是几万轮)。
    int maxFrames = 0;
};

// ============================================================
// 一步
// ============================================================
struct HeadlessStep {
    int    frame = -1;
    int    step = -1;
    QString opName;
    QString uuid;
    bool   failed = false;
    bool   skipped = false;
    QString error;
    QJsonObject output;

    QJsonObject toJson() const;
};

// ============================================================
// 整条流水线的结果
// ============================================================
struct HeadlessResult {
    bool ok = false;
    QString error;               // 整体性错误(文件没法用 / 有环 / 引擎拒绝跑)

    QVector<HeadlessStep> steps; // 每一步(跨所有帧)
    QJsonObject outputs;         // uuid -> 最后一帧的输出
    QString finalResult;         // PASS / FAIL / UNKNOWN(最后一帧的判定)
    // ⚠ 整批结论 —— 和 finalResult **不是一回事**:
    //   finalResult 是**末帧**判了什么;
    //   batchVerdict 是**这一批**是什么结论
    //   (任一帧判废 -> FAIL;任一帧跑坏 -> ERROR;中止 -> CANCELLED)。
    //
    //   以前 CLI 的退出码是自己拿 finalResult 推的
    //   (`if (res.finalResult == "FAIL") return 1;`)——
    //   那是**末帧**判定:30 帧里第 7 帧判废、末帧正常,会被判成"通过"。
    //   同一批东西,主程序说"整批判 NG",CI 说"过了" —— 而 CI 那个是漏检。
    //
    //   现在这一份由 w7::classifyBatch 算好,CLI 只准读它。
    w7::BatchVerdict batchVerdict = w7::BatchVerdict::Broken;
    int passCount = 0;
    int failCount = 0;
    int frameCount = 0;
    int errorCount = 0;          // 输出里带 error 的步骤数(跨所有帧)
    QStringList logs;            // 过程中的日志(跳过/端点缺失/截断…))

    // === 读盘时的三个异常计数(机器可读) ===
    //
    // ⚠ 2026-10-25(欠账 18 加载期 / 欠账 19):
    //   这三个计数以前**只写不读** —— 生产代码里一处都不读,
    //   只有测试断言它们是 0(那等于「证明了它们一直是 0」,
    //   不是「证明了有人在看」)。
    //
    //   现在它们进结果 JSON,CI 和别的调用方**能查**:
    //     unknown_operators  算子没装的节点数(**节点仍在图上**)
    //     broken_edges       端点真不存在的连线数
    //     defaulted_sides    缺方向字段、按默认重画的连线数
    //
    //   为什么 unknown_operators 要能被机器查:
    //   有算子没装 -> 那一步会失败 -> 整批判 ERROR。
    //   「ERROR」是**结论**,「因为哪个算子没装」才是**原因** ——
    //   CI 报 ERROR 时得能说出**为什么**。
    int unknownNodes = 0;
    int brokenEdges = 0;
    int defaultedSides = 0;

    QJsonObject toJson() const;
};

// ============================================================
// runPipelineJson —— 跑一条流水线
// ============================================================
//
// factoryFn:  算子名 -> 显示名/分类(认不认识它)
// defaultsFn: 算子名 -> 声明默认值
// runner:     跑一个算子
//
// 返回 false = 整条流水线没法跑(文件不是流水线 / 有环 / 引擎拒绝跑)。
// ⚠ 那时 **out 里也有内容** —— steps / logs 已经攒下的别扔,
//   排查的人要靠它们才说得清"卡在哪"。
bool runPipelineJson(const QJsonDocument& doc,
                     const HeadlessOptions& opt,
                     const std::function<OpDefaults(const QString&)>& factoryFn,
                     const w7::EngineDefaultsFn& defaultsFn,
                     const w7::EngineRunner& runner,
                     HeadlessResult* out,
                     QString& errOut);

} // namespace w7
