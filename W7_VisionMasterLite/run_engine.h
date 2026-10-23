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

} // namespace w7
