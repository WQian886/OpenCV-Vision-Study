#pragma once
// ============================================================
// 连线拖拽 payload 的编解码 —— "uuid|side"
//
// 单独抽成一个头,是因为这段逻辑出过一次很难查的错,而它本来
// 埋在一堆 QDropEvent / mousePressEvent 样板里,纯逻辑根本测不到。
//
// 以前编的是【算子名】,而算子名不唯一:画布里放两个 color_segment,
// 两个节点编出来的 payload 完全一样,落点根本无从分辨,必然接错。
// GUI 上的表现还特别误导 —— 看起来是"被连的那个节点自己跳走",
// 因为 addConnection 会顶掉第一个节点原有的连线,真凶隔了三层。
//
// UUID 才是稳定身份(节点删了重建也还是同一个)。
//
// 头文件而不是 .cpp:operator_node 编 payload、pipeline_view 解 payload,
// 头文件让两边都直接可用,不用给任何一个测试目标新增链接依赖 ——
// 而"能被无头测试直接调"正是把它拆出来的全部理由。
// ============================================================

#include <QString>
#include <QUuid>

namespace w7 {

// --- payload 里的 side 合法取值 ---
// 必须和 OperatorNode::PortSide 的四个值一一对应。
// 这里不 include operator_node.h:那个头拖 QGraphicsObject 进来,
// 而这个文件的价值就在于"零 QtWidgets 依赖、随便哪个测试都能编"。
enum : int { kPortSideMin = 0, kPortSideMax = 3 };

inline QString encodeConnectionPayload(const QUuid& nodeId, int side) {
    return QString("%1|%2")
        .arg(nodeId.toString(QUuid::WithoutBraces))
        .arg(side);
}

// 解不出就返回 false,绝不"猜一个默认值"。
// 猜错的后果不是这次连线失败,而是接到别的节点上 —— 静默接错
// 比明显失败危险得多,所以这里没有默认值可猜。
inline bool decodeConnectionPayload(const QString& payload,
                                    QUuid* nodeId, int* side) {
    const int sep = payload.lastIndexOf('|');
    // 必须是 "<uuid>|<side>" 两段,两侧都不能空
    if (sep <= 0 || sep >= payload.size() - 1) return false;

    const QUuid id = QUuid::fromString(payload.left(sep));
    if (id.isNull()) return false;   // 左半段不是一个 UUID

    bool ok = false;
    const int s = payload.mid(sep + 1).toInt(&ok);
    if (!ok || s < kPortSideMin || s > kPortSideMax) return false;

    if (nodeId) *nodeId = id;
    if (side)   *side   = s;
    return true;
}

} // namespace w7
