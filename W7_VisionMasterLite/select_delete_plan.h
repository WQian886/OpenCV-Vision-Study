#pragma once
// ============================================================
// 批量删除的"计划"层
//
// ============================================================
// Delete 键一次可能选中一堆东西:若干节点 + 若干连线。
// 其中**沾了待删节点**的线,会随节点一起被 PipelineView::removeNode
// 连带 delete,不该再单独记一笔 RemoveEdgeCmd —— 那会和
// DeleteNodeCmd 里记录的线重复,撤销栈里两笔账互相打架。
//
// ============================================================
// 为什么必须先"计划"再"执行"
// ============================================================
// removeNode 会把待删节点**和它的所有连线**一起 delete。
//
// 原来的写法是:
//     for (node : nodesToDelete) doDeleteNode(node);   // ← 线在这里就没了
//     for (line : connsToDelete)
//         if (line->fromNode() == n || ...)           // ← 读已释放内存
//
// 第二轮遍历里的 line 有可能正是第一轮刚 delete 掉的那些 ——
// 这是货真价实的 use-after-free。
//
// 它为什么"有时候才崩":Release 下堆内存不会被自动填毒,
// 被释放的连线对象里还留着原来的指针值,读出来往往还是对的;
// 直到那块内存被别的分配复用,才读到垃圾 → 段错误。
// 这类 bug 现场就没了,最难查。
//
// 所以这里的约定是:**所有指针读取只允许发生在 planSelectDelete 内部**,
// 返回的是 QUuid(值语义),执行阶段一个裸指针都不碰。
//
// ============================================================
// 纯函数、头文件内联 —— 没有状态、没有副作用,
// 单元测试可以不带 MainWindow 直接调它。
// ============================================================

#include <QList>
#include <QUuid>

#include "operator_node.h"
#include "connection_line.h"

namespace w7 {

struct EdgeEndpoint {
    QUuid from;
    QUuid to;
    bool operator==(const EdgeEndpoint& o) const { return from == o.from && to == o.to; }
};

struct SelectDeletePlan {
    QList<QUuid>        nodes;   // 要走 DeleteNodeCmd 的节点
    QList<EdgeEndpoint> edges;   // 要单独走 RemoveEdgeCmd 的"独立连线"
    int absorbedEdges    = 0;    // 随节点一起被连带删除的连线(只用于日志)

    int totalEdges() const { return edges.size() + absorbedEdges; }
    bool isEmpty() const { return nodes.isEmpty() && edges.isEmpty(); }
};

// ============================================================
// selectedNodes  —— 选中的节点
// selectedLines  —— 选中的连线
// allLines       —— 画布上全部连线(用来把"沾了待删节点"的线收进来)
// ============================================================
// 注意:本函数必须在**任何 delete 之前**调用。
inline SelectDeletePlan planSelectDelete(const QList<OperatorNode*>&   selectedNodes,
                                         const QList<ConnectionLine*>& selectedLines,
                                         const QList<ConnectionLine*>& allLines) {
    SelectDeletePlan plan;

    QList<OperatorNode*> doomed;
    for (auto* n : selectedNodes) {
        if (!n) continue;
        doomed.append(n);
        plan.nodes.append(n->uuid());
    }

    // 待删集合:沾上任一待删节点的线,都会被 removeNode 连带删除
    QList<ConnectionLine*> doomedLines;
    for (auto* line : allLines) {
        if (!line) continue;
        const bool touches = doomed.contains(line->fromNode())
                          || doomed.contains(line->toNode());
        if (touches) doomedLines.append(line);
    }
    plan.absorbedEdges = doomedLines.size();

    // 只有"既没被选中、又没沾待删节点"的线,才是需要单独记一笔的。
    // 沾待删节点的线不该再单独记一笔 —— 它已经记在 DeleteNodeCmd 里了,
    // 撤销栈里两笔账会互相打架。
    for (auto* line : selectedLines) {
        if (!line || doomedLines.contains(line)) continue;
        if (!line->fromNode() || !line->toNode()) continue;
        plan.edges.append(EdgeEndpoint{line->fromNode()->uuid(), line->toNode()->uuid()});
    }
    return plan;
}

} // namespace w7