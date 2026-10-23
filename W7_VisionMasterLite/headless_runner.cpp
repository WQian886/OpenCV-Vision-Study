#include "headless_runner.h"
#include "run_limits.h"         // clampFrameCount / kMaxRunFrames(纯算术,零 QtWidgets)

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
    for (int frame = 0; frame < totalFrames; ++frame) {
        QVector<HeadlessStep> frameSteps;
        int frameErrors = 0;      // 这一帧里报错的步数

        auto hook = [&](EnginePhase phase, const EngineStepInfo& info) {
            if (phase == EnginePhase::Before) return;   // Before 只是"要跑了"
            // ⚠ 输出里带 error 的步骤要**单独数**:
            //   算子"跑成功"但报了个错(比如读不到配方文件)——
            //   引擎看它没崩,于是当成功记下了。
            //   可对用户来说那就是**没跑成**。
            //   窗口那边把这种状态点标成橙色,这边也要算成错误 ——
            //   不然退出码是 0,CI 就当它过了。
            if (info.output.contains(QStringLiteral("error"))) {
                ++frameErrors;
                ++res.errorCount;
            }
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
            *out = res;
            return false;
        }
        res.steps += frameSteps;

        // ---- 本帧的判定 ----
        //
        // 取**最后一个跑了而且带 result 的节点**的输出 ——
        // 和主程序一模一样(不是"第一个",也不是"最后一个跑的")。
        QJsonObject frameResult;
        int finalIndex = -1;
        for (int idx : ef.executed) {
            finalIndex = idx;
            const QJsonObject& o = ef.outputs.at(idx);
            if (o.contains("result")) frameResult = o;
        }
        if (!frameResult.contains("result") && finalIndex >= 0)
            frameResult = ef.outputs.at(finalIndex);

        const QString verdict = frameResult.value("result").toString();
        // ⚠ 这一帧**有步骤报错**的话,它既不算 PASS 也不算 FAIL ——
        //   它是"跑坏了",不是"跑过了"也不是"没通过"。
        //   把坏的一帧算成 PASS,是最贵的那类静默失效:
        //   数字看着正常,结论却是错的。
        if (frameErrors == 0) {
            if (verdict == "PASS")      ++res.passCount;
            else if (verdict == "FAIL") ++res.failCount;
        }
        ++res.frameCount;
        res.finalResult = verdict.isEmpty() ? QStringLiteral("UNKNOWN") : verdict;

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
        if (frame == 0) {
            for (const QJsonObject& o : ef.outputs) {
                if (!o.contains("frame_count")) continue;
                const int reported = o.value("frame_count").toInt(1);
                bool truncated = false;
                totalFrames = clampFrameCount(reported, &truncated);
                if (truncated) {
                    res.logs.append(QStringLiteral(
                        "[Run] ⚠️ 一共有 %1 帧,超过单次上限 %2 帧,只跑前 %2 帧")
                        .arg(reported).arg(kMaxRunFrames));
                }
                break;
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
    if (res.errorCount > 0) res.finalResult = QStringLiteral("ERROR");
    *out = res;
    return true;
}

} // namespace w7
