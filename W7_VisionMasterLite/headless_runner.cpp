#include "headless_runner.h"
#include "run_limits.h"         // clampFrameCount / kMaxRunFrames(纯算术,零 QtWidgets)
#include "frame_verdict.h"      // 帧判定与整批结论(和主程序**共用**这一份)

#include <QJsonArray>

namespace w7 {

// ============================================================
// 结果 -> JSON
// ============================================================
QJsonObject HeadlessStep::toJson() const {
    QJsonObject o;
    o["frame"]   = frame;
    o["step"]    = step;
    o["op"]      = opName;
    o["uuid"]    = uuid;
    o["failed"]  = failed;
    o["skipped"] = skipped;
    if (!error.isEmpty()) o["error"] = error;
    o["output"]  = output;
    return o;
}

QJsonObject HeadlessResult::toJson() const {
    QJsonObject o;
    o["ok"]           = ok;
    o["error"]        = error;
    o["final_result"] = finalResult;
    // ⚠ 整批结论,和 final_result(末帧判定)不是一回事。
    //   CI 要判"这一批过没过",看的是这个字段。
    o["batch_verdict"] = w7::batchVerdictName(batchVerdict);
    o["frames"]       = frameCount;
    o["pass"]         = passCount;
    o["fail"]         = failCount;
    o["errors"]       = errorCount;
    o["outputs"]      = outputs;

    QJsonArray stepArr;
    for (const HeadlessStep& s : steps) stepArr.append(s.toJson());
    o["steps"] = stepArr;

    QJsonArray logArr;
    for (const QString& l : logs) logArr.append(l);
    o["logs"] = logArr;

    return o;
}

// ============================================================
// runPipelineJson
// ============================================================
bool runPipelineJson(const QJsonDocument& doc,
                     const HeadlessOptions& opt,
                     const std::function<OpDefaults(const QString&)>& factoryFn,
                     const w7::EngineDefaultsFn& defaultsFn,
                     const w7::EngineRunner& runner,
                     HeadlessResult* out,
                     QString& errOut) {
    if (!out) { errOut = "输出参数为空"; return false; }
    HeadlessResult res;

    // ---- 1. 解析(和主程序同一份规则)----
    PipelineGraph g;
    if (!graphFromJson(doc,
                       [&res](const QString& s) { res.logs.append(s); },
                       factoryFn, &g, errOut)) {
        res.error = errOut;
        *out = res;
        return false;
    }
    if (g.nodes.isEmpty()) {
        errOut = "流水线里一个节点都没有";
        res.error = errOut;
        *out = res;
        return false;
    }

    // ---- 2. 图 -> 引擎的数据 ----
    //
    // 节点先按**图里的顺序**放,边按**图里的下标**引用。
    // 接下来由引擎决定执行序 —— headless 和主程序用的是同一个 topoOrder。
    QVector<EngineNode> rawNodes;
    rawNodes.reserve(g.nodes.size());
    for (const GraphNode& gn : g.nodes) {
        EngineNode en;
        en.opName = gn.op;
        en.uuid   = gn.uuid;
        en.params = gn.params;
        rawNodes.append(en);
    }
    QVector<EngineEdge> rawEdges;
    rawEdges.reserve(g.edges.size());
    for (const GraphEdge& ge : g.edges)
        rawEdges.append(EngineEdge{ge.fromIndex, ge.toIndex});

    // ---- 3. 执行序 ----
    bool hasCycle = false;
    const QVector<int> ord = topoOrder(rawNodes.size(), rawEdges, &hasCycle);
    if (hasCycle) {
        // 有环 -> **什么都不跑**。跑一半的话下游拿到的是上一帧的输出,
        // 数字看着正常,结论却是错的。
        errOut = QStringLiteral("流水线里有环,无法确定执行顺序");
        res.error = errOut;
        *out = res;
        return false;
    }
    QVector<EngineNode> ordered;
    ordered.reserve(ord.size());
    for (int i : ord) ordered.append(rawNodes.at(i));
    // 边换成执行序的下标(顺序原样保留 —— 多前驱合并时谁赢靠的就是它)
    const QVector<EngineEdge> edges = reindexEdges(ord, rawEdges);

    // ---- 4. 兜底输入 ----
    //
    // ⚠ 和主程序一字不差:它只是**兜底** ——
    //   上游自己产出了 image_path 就不用它。
    QJsonObject fallbackInputs;
    const QString img = opt.imagePath.isEmpty() ? g.imagePath : opt.imagePath;
    if (!img.isEmpty()) fallbackInputs["image_path"] = img;

    // ---- 5. 帧循环 ----
    //
    // 上界是**动态**的:首帧跑完才知道一共有多少帧
    // (源算子跑完才知道自己有几张图)。
    int totalFrames = 1;
    // 逐帧累计 —— 判定口径在 frame_verdict.h 里,**和主程序共用一份**。
    //
    // ⚠ 以前这一段(headless 和 mainwindow 各写一遍)已经漂过一次:
    //   headless 这边有"坏帧不算 PASS/FAIL"和"有错强制 ERROR"两条规则,
    //   主程序那边两条都没有 —— 同一条流水线,两边结论相反。
    //   现在只有这一份。
    w7::FrameTally tally;
    for (int frame = 0; frame < totalFrames; ++frame) {
        QVector<HeadlessStep> frameSteps;
        w7::FrameErrorCounter frameErrors;   // 这一帧里的坏步数(判据和主程序共用一份)

        auto hook = [&](EnginePhase phase, const EngineStepInfo& info) {
            if (phase == EnginePhase::Before) return;   // Before 只是"要跑了"
            // ⚠ 坏步要**单独数**:
            //   算子"跑成功"但报了个错(比如读不到配方文件)——
            //   引擎看它没崩,于是当成功记下了。
            //   可对用户来说那就是**没跑成**。
            //   窗口那边把这种状态点标成橙色,这边也要算成错误 ——
            //   不然退出码是 0,CI 就当它过了。
            //
            // ⚠ 判据在 frame_verdict.h 里(w7::isBadStep),和主程序共用一份:
            //     · 跑成了但输出里带 error
            //     · **压根没跑成**(info.failed)—— 2026-10-25 才补上,
            //       以前数不到:引擎把失败步骤的输出置空了,
            //       失败原因在 info.error 里,不在输出里
            if (frameErrors.noteStep(info.failed, info.output)) ++res.errorCount;
            HeadlessStep st;
            st.frame   = frame;
            st.step    = info.step;
            st.opName  = info.opName;
            st.uuid    = info.uuid;
            st.failed  = info.failed;
            st.skipped = info.skipped;
            st.error   = info.error;
            st.output  = info.output;
            frameSteps.append(st);
        };

        // headless 的画布不会变 —— 没有节点会被删,所以不用传 alive 回调。
        const EngineFrame ef = runFrame(ordered, edges, fallbackInputs, frame, -1,
                                        defaultsFn, runner, {}, hook);
        if (!ef.ok) {
            errOut = ef.error;
            res.error = ef.error;
            res.steps += frameSteps;
            // ⚠ 这一批压根没跑完 —— 没有任何结论。
            //   别让 batch_verdict 停在默认的 PASS 上:
            //   下游只读这一个字段的话,就会把"没跑完"当成"跑过了"。
            res.batchVerdict = w7::BatchVerdict::Broken;
            *out = res;
            return false;
        }
        res.steps += frameSteps;

        // ---- 本帧的判定 ----
        //
        // 取**最后一个跑了而且带 result 的节点**的输出 ——
        // 和主程序一模一样(不是"第一个",也不是"最后一个跑的")。
        // 这条规则也住在 frame_verdict.h 里,两边共用。
        int finalIndex = -1;
        const QJsonObject frameResult =
            w7::pickFrameResult(ef.outputs, ef.executed, &finalIndex);

        const QString verdict = frameResult.value("result").toString();
        // ⚠ 这一帧**有步骤报错**的话,它既不算 PASS 也不算 FAIL ——
        //   它是"跑坏了",不是"跑过了"也不是"没通过"。
        //   把坏的一帧算成 PASS,是最贵的那类静默失效:
        //   数字看着正常,结论却是错的。
        //
        //   口径在 frame_verdict.h 里(classifyFrame),和主程序共用。
        tally.addFrame(verdict, frameErrors.count());
        res.passCount  = tally.pass;
        res.failCount  = tally.fail;
        res.frameCount = tally.frames;
        // 末帧的原始判定 —— 中途失败时这个值还能说明"跑到哪儿了",
        // 所以这里用 lastResultOf(不带"有错就 ERROR"那条规则)。
        res.finalResult = w7::lastResultOf(tally);

        // 最后一帧的输出(uuid -> 输出)
        //
        // ⚠ 没有 uuid 的节点(文件里没写)用 **下标**兜底 ——
        //   键不能空:空键的话结果里就分不出是哪个节点了。
        QJsonObject outs;
        for (int idx : ef.executed) {
            const QString uuid = ordered.at(idx).uuid;
            const QString key = uuid.isEmpty()
                              ? QStringLiteral("#%1").arg(idx)
                              : uuid;
            outs.insert(key, ef.outputs.at(idx));
        }
        res.outputs = outs;

        // ---- 首帧跑完才知道一共有多少帧 ----
        //
        // ⚠ 2026-10-25:「帧协议」收成**一份**了(w7::resolveFrameCount)。
        //   以前这里是 mainwindow.cpp 和 headless_runner.cpp **各写一遍**
        //   —— 规则只有三条,两边各写一份就没有东西保证它们一致。
        //   详见 run_limits.h 里 resolveFrameCount 那段。
        if (frame == 0) {
            int reported = 1;
            bool truncated = false;
            totalFrames = resolveFrameCount(ef.outputs, &reported, &truncated);
            if (truncated) {
                res.logs.append(QStringLiteral(
                    "[Run] ⚠️ 一共有 %1 帧,超过单次上限 %2 帧,只跑前 %2 帧")
                    .arg(reported).arg(kMaxRunFrames));
            }
            // 用户给的帧数上限
            const int cap = (opt.maxFrames > 0) ? opt.maxFrames : kMaxRunFrames;
            if (totalFrames > cap) {
                res.logs.append(QStringLiteral(
                    "[Run] ⚠️ 只跑前 %1 帧(--frames 的上限)").arg(cap));
                totalFrames = cap;
            }
        }
    }

    res.ok = true;
    // ⚠ 有步骤报错的话,**不许**把判定说成 PASS。
    //   判定节点说 PASS 不代表整条流水线跑成了 ——
    //   上游报过错,那个 PASS 就是**空头**的。
    //   判定节点自己的原始输出还在 outputs 里,没丢。
    //
    //   ⚠ 这条规则住在 frame_verdict.h 里(finalResultOf),**和主程序共用**。
    //   以前只有 headless 这边有,主程序那边没有 ——
    //   于是同一条流水线,GUI 说"最终结果: PASS",这边说 ERROR。
    //   产线上判废看的是 GUI 那份,所以它是错的那一份。
    res.finalResult = w7::finalResultOf(tally);
    // 整批结论 —— 和主程序**共用** w7::classifyBatch 这一份。
    //
    // ⚠ 以前 CLI 拿 res.finalResult 自己推整批结论,
    //   而 finalResult 是**末帧**判定 —— 30 帧里第 7 帧判废、
    //   末帧正常,会被推成"通过"(退出码 0)。那是抽检漏检。
    //   现在整批结论在这里算好,CLI 只准读 res.batchVerdict。
    res.batchVerdict = w7::classifyBatch(tally, false);
    *out = res;
    return true;
}

} // namespace w7
