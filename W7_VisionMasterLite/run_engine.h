#pragma once
// ============================================================
// 流水线执行引擎 —— 拓扑序 + 前驱输出合并
// ============================================================
//
// 为什么要有这个文件:
//
//   跑一条流水线的逻辑以前全在 mainwindow.cpp 里,而
//   mainwindow.cpp **不在任何测试目标里**(它要真窗口 + 整套算子,
//   建一次目标要好几分钟)。
//
//   于是两件有真实语义的事,一直是**只有源码 lint 看着**的:
//     · 拓扑序怎么算、有环能不能检出
//     · 多个前驱的输出合并时,谁赢
//
//   第二件尤其要紧 —— 2026-10-15 的一次性探针
//   (手抄 mainwindow.cpp 的合并逻辑跑出来的)实测到:
//
//     顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue / 5000
//     顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red  / 10000
//
//   两个上游状态点都是绿的、都不报错,只有下游那个数悄悄变了。
//   而且**连"先连线的赢"都不成立** —— 真正决定胜负的是
//   connections() 的遍历顺序。
//
//   那份探针是**一次性**的:合并逻辑一动,它就过期了,
//   而且**没人会知道**它过期了 —— 这正是本项目栽过五遍的「抄副本」。
//   所以把它抽成**一份**能直接测的代码。
//
// ⚠ 为什么用**下标**而不是指针:
//   引擎跑的时候节点可能已经被删了(跑批期间右键删了/Load 了)。
//   mainwindow 那边用 QPointer 兜着,换成裸指针就是往已释放内存上写 ——
//   实测过,表现是随机的卡死或闪退。
//   引擎这一层干脆不持有节点指针:它只认"第几个节点"。
//
// ⚠ 依赖面:
//   只依赖 QtCore(QJsonObject / QString / QVector / functional),
//   **不碰 QtWidgets,也不碰 OpenCV** ——
//   所以依赖它的测试目标能保持"零 OpenCV"这个属性(建得快)。
// ============================================================

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace w7 {
// ============================================================
// 一条边:两端都是**下标**
// ============================================================
// from/to 是 EngineNode 数组里的下标,不是指针。
struct EngineEdge {
    int from = -1;   // 上游节点下标
    int to   = -1;   // 下游节点下标
};

// ============================================================
// 拓扑序
// ============================================================
//
// 用的是 Kahn 算法(入度 + BFS):
//   1. 入度 0 的节点按**下标顺序**入队
//   2. 出队一个就把它下游的入度减一,减到 0 就入队
//   3. 出队数 != 节点数 → 图里有环
//
// ⚠ 为什么"入队顺序"要按下标顺序:
//   同一张图可能有多个合法的拓扑序。选哪个是**实现细节**,
//   但它必须是**确定的** —— 不确定的话,同一份流水线两次跑
//   可能给出不同的执行顺序,那是极难查的一类 bug。
//   这里按节点下标(也就是画布上的节点顺序)入队,确定且可复现。
//
// ⚠ 环:返回空 + hasCycle=true。
//   宁可什么都不跑,也不要跑一半 —— 跑一半的话,
//   下游拿到的是**上一轮**的输出,数字看着正常,结论却是错的。
inline QVector<int> topoOrder(int nodeCount,
                              const QVector<EngineEdge>& edges,
                              bool* hasCycle) {
    if (hasCycle) *hasCycle = false;
    QVector<int> order;
    if (nodeCount <= 0) return order;

    // 1. 建图 + 入度统计
    QVector<QVector<int>> graph(nodeCount);
    QVector<int> inDegree(nodeCount, 0);
    for (const EngineEdge& e : edges) {
        // 越界的边直接跳过 —— 指向已删节点的连线不该让整个图崩掉。
        // ⚠ 这不是"宽容":边是数据,数据坏了就当它不存在,
        //   但**必须**有人负责报出来(见 run_pipeline 那一层)。
        if (e.from < 0 || e.to < 0) continue;
        if (e.from >= nodeCount || e.to >= nodeCount) continue;
        graph[e.from].append(e.to);
        ++inDegree[e.to];
    }

    // 2. 入度 0 的节点入队(按下标顺序 —— 见上面的说明)
    QVector<int> queue;
    for (int i = 0; i < nodeCount; ++i)
        if (inDegree[i] == 0) queue.append(i);

    // 3. BFS 出队
    for (int head = 0; head < queue.size(); ++head) {
        const int n = queue.at(head);
        order.append(n);
        for (int m : graph.at(n)) {
            if (--inDegree[m] == 0) queue.append(m);
        }
    }

    // 4. 环检测
    if (order.size() != nodeCount) {
        if (hasCycle) *hasCycle = true;
        return {};
    }
    return order;
}

// ============================================================
// 合并多个前驱的输出 —— **后写覆盖**
// ============================================================
//
// 语义:**后进来的覆盖先来的**(逐键覆盖)。
//
// ⚠ 这个"后写覆盖"是**已实测复现的静默失效**:
//   胜负取决于**上游列表的顺序**,而那个顺序来自
//   connections() 的遍历顺序 —— 不是"谁先连线的"。
//
//   所以主程序那边**必须**在用户连第二条线到同一个下游时
//   就把他拦下来(pin_types.cpp::checkMultiplePredecessors 干的活),
//   并在提示里说清根因是"后写覆盖"。
//   这个函数**只负责"真发生了合并时按什么规则合"**,
//   拦不拦是**另一件事**,两者分开才说得清责任。
//
// ⚠ 为什么参数是"上游输出的列表"而不是"图 + 下标":
//   画布那边跑批期间节点可能被删,下标映射会漂;
//   而"哪些连线指向这个节点"的筛选逻辑只有三行,而且
//   **遍历顺序必须原样保留**(它就是胜负)。
//   把顺序原样传进来,这个函数就与画布解耦了 ——
//   也就**能直接测**。
inline QJsonObject mergeUpstreamOutputs(const QVector<QJsonObject>& upstream) {
    QJsonObject merged;
    for (const QJsonObject& u : upstream) {
        for (auto it = u.begin(); it != u.end(); ++it) {
            merged[it.key()] = it.value();   // 简单合并:后写覆盖
        }
    }
    return merged;
}

// ============================================================
// 跑一帧
// ============================================================
//
// 这一层以前住在 mainwindow.cpp 的节点循环里。它做的事按重要性排:
//
//   1. 组装这次调用的 inputs —— 上游输出合并 + 兜底 + frame_index + node_uuid
//   2. 组装这次调用的 params —— 节点参数优先,空则用算子的**声明默认值**
//   3. 跑算子,把输出存下来
//   4. 每步的前后通知(UI 用):状态点、日志、每步缓存
//
// ⚠ 为什么 UI 的活儿留在外面:
//   状态点、日志、每步缓存都要 QWidget 和 QTextEdit,引擎碰了就拖进整个 GUI。
//   所以引擎只留"纯"的那部分,UI 通过 hook 回调。
//   引擎不持有任何 QWidget 指针 —— 这一点是下面那些设计的前提。
//
// ⚠ 为什么节点是**快照**而不是指针:
//   跑批期间画布是活的(右键删、拖节点、Load 工程),
//   裸指针会在节点中途消失时变成 use-after-free ——
//   实测过,表现是随机的卡死或闪退。
//   快照(engineNode 里全是值)天然没有这个问题:
//   节点被删了,引擎手里那份**数据**还在,只是这个节点不该再跑了。
//   "该不该跑"由 alive 回调回答。
// ============================================================

// 一个节点的纯数据快照
struct EngineNode {
    QString opName;      // 算子名
    QString uuid;        // 节点实例标识(算子靠它生成唯一文件名)
    QJsonObject params;  // 节点上的参数(空 = 用算子的声明默认值)
};

// 取某个算子的**声明默认值**。
// 返回空 QJsonObject = 这个算子没有默认值(或压根没注册)。
using EngineDefaultsFn = std::function<QJsonObject(const QString& opName)>;

// 跑一个算子。
//   opName  算子名
//   params  已经组装好的参数(引擎负责组装,这里不用再兜底)
//   inputs  已经组装好的输入
//   out     输出
//   err     失败原因(返回 false 时必须写清)
// 返回 false = 这一步失败(算子没注册 / 算子自己报错)。
using EngineRunner = std::function<bool(const QString& opName,
                                        const QJsonObject& params,
                                        const QJsonObject& inputs,
                                        QJsonObject* out,
                                        QString* err)>;

// 这个节点**现在还在不在**。
// 跑批期间节点可能被删 —— 返回 false 的节点会被跳过(和以前一样)。
using EngineAliveFn = std::function<bool(int nodeIndex)>;

// 一步的阶段
enum class EnginePhase {
    Before,   // 即将跑这个节点(UI:点亮它、抽帧限频)
    After,    // 这个节点跑完了(成功或失败)
    Skipped,  // 这个节点被跳过了(节点已被删除)
};

struct EngineStepInfo {
    int nodeIndex = -1;      // 节点下标(nodes 数组里的位置)
    int step = -1;           // 第几步(0 起)
    QString opName;
    QString uuid;
    QJsonObject output;      // 只有 After 阶段才有
    bool failed = false;     // 算子没注册 / 算子自己报错
    bool skipped = false;    // 这个节点被跳过了(节点已被删除)
    QString error;           // 失败原因
};

using EngineStepHook = std::function<void(EnginePhase, const EngineStepInfo&)>;

// 一帧的结果
struct EngineFrame {
    QVector<QJsonObject> outputs;  // 按节点下标(没跑的节点是空对象)
    QVector<int> executed;          // 实际跑了哪些节点(按执行顺序)
    QString error;                  // 整体性错误(顺序不对 / 一个节点都没有)
    bool ok = false;
};

// ============================================================
// runFrame —— 跑一帧
// ============================================================
//
// ⚠ nodes 必须**已经按执行顺序排好**。
//   引擎**不重新排序** —— 排序是调用方的事(w7::topoOrder)。
//   理由:主程序那边还有"单步执行 = 拓扑序前缀"这条规则
//   (step_scope.h),两处必须用**同一个**顺序,
//   否则单步的前缀会切错 —— 而那种错不崩、只是下游拿到上一帧的残留。
//
//   但引擎会**验**一遍顺序对不对(见下面):顺序错了要报错,不能闷头跑。
//
// ⚠ maxSteps = -1 表示全跑;>=0 表示只跑前 maxSteps 个(单步执行用)。
//
// ⚠ 整帧的 inputs 组装顺序(和主程序以前的做法一字不差):
//   1. 上游输出合并(后写覆盖)
//   2. fallbackInputs 里**缺了才补**的键(比如 image_path)
//   3. frame_index / node_uuid —— **总是覆盖**
inline EngineFrame runFrame(const QVector<EngineNode>& nodes,
                            const QVector<EngineEdge>& edges,
                            const QJsonObject& fallbackInputs,
                            int frameIndex,
                            int maxSteps,
                            const EngineDefaultsFn& defaults,
                            const EngineRunner& runner,
                            const EngineAliveFn& alive,
                            const EngineStepHook& hook) {
    EngineFrame frame;

    if (nodes.isEmpty()) {
        frame.error = QStringLiteral("画布为空,没有任何节点");
        return frame;
    }

    // === 验顺序 ===
    //
    // 引擎不排序,但也不能闭着眼相信调用方给的顺序 ——
    // 顺序错了的话,下游会拿到**空的上游输出**,
    // 数字看着正常(有默认值兜底),结论却是错的。
    // 那是最贵的一类静默失效。
    {
        QVector<int> pos(nodes.size(), -1);
        for (int i = 0; i < nodes.size(); ++i) pos[i] = i;   // nodes 已是执行序
        for (const EngineEdge& e : edges) {
            if (e.from < 0 || e.to < 0) continue;
            if (e.from >= nodes.size() || e.to >= nodes.size()) continue;
            if (e.from >= e.to) {
                frame.error = QStringLiteral("执行顺序不对:节点 %1 排在了它的上游 %2 后面")
                                  .arg(e.to).arg(e.from);
                return frame;
            }
        }
    }

    const int limit = (maxSteps < 0) ? nodes.size() : qMin(maxSteps, nodes.size());

    // 输出数组先开好 —— 不开的话下面 `frame.outputs[step] = ...`
    // 就是**越界写**(QVector 的 operator[] 不做边界检查)。
    // ⚠ 这不是洁癖:越界写的是堆,表现是随机的崩或者静默改坏别的东西。
    frame.outputs.resize(nodes.size());

    for (int step = 0; step < limit; ++step) {
        const EngineNode& node = nodes.at(step);

        // === 节点还在不在 ===
        //
        // 跑批期间画布是活的(右键删、拖节点、Load 工程)。
        // 以前这一步是 QPointer 变 null,跳过就行。
        // 现在引擎手里是**快照**(没有指针,不会 UAF),
        // 但"该不该跑"还是要问画布 —— 不问的话,
        // 用户明明删掉了这个节点,它却还跑一遍,结果还进了下游。
        if (alive && !alive(step)) {
            EngineStepInfo info;
            info.nodeIndex = step;
            info.step = step;
            info.opName = node.opName;
            info.uuid = node.uuid;
            info.skipped = true;
            if (hook) hook(EnginePhase::Skipped, info);
            continue;
        }

        EngineStepInfo info;
        info.nodeIndex = step;
        info.step = step;
        info.opName = node.opName;
        info.uuid = node.uuid;
        if (hook) hook(EnginePhase::Before, info);

        // === 组装 inputs ===
        QVector<QJsonObject> upstream;
        for (const EngineEdge& e : edges) {
            if (e.to != step) continue;
            if (e.from < 0 || e.from >= nodes.size()) continue;
            upstream.append(frame.outputs.at(e.from));
        }
        QJsonObject inputs = mergeUpstreamOutputs(upstream);

        // 兜底:上游没产出的键,用 fallbackInputs 补(比如 image_path)
        for (auto it = fallbackInputs.begin(); it != fallbackInputs.end(); ++it) {
            if (!inputs.contains(it.key())) inputs[it.key()] = it.value();
        }
        // 这两个**总是覆盖** —— 它们是引擎的约定,不是用户的数据
        inputs[QStringLiteral("frame_index")] = frameIndex;
        inputs[QStringLiteral("node_uuid")] = node.uuid;

        // === 组装 params ===
        //
        // 节点参数优先;**空**就用算子的声明默认值。
        // ⚠ 判据是"空",不是"缺某个键" ——
        //   节点上一个键都没存,才轮到声明默认值说话。
        QJsonObject params = node.params;
        if (params.isEmpty() && defaults) params = defaults(node.opName);

        // === 跑 ===
        QJsonObject out;
        QString err;
        const bool ok = runner ? runner(node.opName, params, inputs, &out, &err) : false;
        if (!ok) {
            info.failed = true;
            info.error = err.isEmpty() ? QStringLiteral("算子执行失败") : err;
            frame.outputs[step] = QJsonObject();   // 失败的节点不产出输出
            if (hook) hook(EnginePhase::After, info);
            continue;                 // ← 和以前一样:跳过,不中断整条链
        }

        info.output = out;
        frame.outputs[step] = out;
        frame.executed.append(step);
        if (hook) hook(EnginePhase::After, info);
    }

    frame.ok = true;
    return frame;
}

} // namespace w7
