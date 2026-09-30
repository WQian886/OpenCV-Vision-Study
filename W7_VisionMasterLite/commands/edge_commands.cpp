// ============================================================
// 连线相关 Command 实现 (M6 Step4)
// 两端节点找不到时静默 no-op
// ============================================================

#include "edge_commands.h"
#include "../pipeline_view.h"
#include "../operator_node.h"
#include "../connection_line.h"

namespace w7 {

// ============================================================
// AddEdgeCmd
// ============================================================
AddEdgeCmd::AddEdgeCmd(PipelineView* canvas, const QUuid& fromId, const QUuid& toId,
                       OperatorNode::PortSide fromSide,
                       OperatorNode::PortSide toSide)
    : m_canvas(canvas), m_fromId(fromId), m_toId(toId),
      m_fromSide(fromSide), m_toSide(toSide) {}   // === M7: 持久化两侧的端口 ===

void AddEdgeCmd::exec() {
    if (!m_canvas) return;
    OperatorNode* from = m_canvas->findNodeByUuid(m_fromId);
    OperatorNode* to   = m_canvas->findNodeByUuid(m_toId);
    if (!from || !to) return;
    // === M7: 把存下来的 side 传给 addConnection ===
    m_canvas->addConnection(from, to, m_fromSide, m_toSide);
}

void AddEdgeCmd::undo() {
    if (!m_canvas) return;
    if (ConnectionLine* line = m_canvas->findConnection(m_fromId, m_toId)) {
        m_canvas->removeConnection(line);
    }
}

void AddEdgeCmd::redo() {
    exec();
}

QString AddEdgeCmd::desc() const {
    return QString("Connect");
}

// ============================================================
// RemoveEdgeCmd
// ============================================================
RemoveEdgeCmd::RemoveEdgeCmd(PipelineView* canvas, const QUuid& fromId, const QUuid& toId)
    : m_canvas(canvas), m_fromId(fromId), m_toId(toId) {}

void RemoveEdgeCmd::exec() {
    if (!m_canvas) return;
    if (ConnectionLine* line = m_canvas->findConnection(m_fromId, m_toId)) {
        m_canvas->removeConnection(line);
    }
}

void RemoveEdgeCmd::undo() {
    if (!m_canvas) return;
    OperatorNode* from = m_canvas->findNodeByUuid(m_fromId);
    OperatorNode* to   = m_canvas->findNodeByUuid(m_toId);
    if (!from || !to) return;
    m_canvas->addConnection(from, to);
}

void RemoveEdgeCmd::redo() {
    exec();
}

QString RemoveEdgeCmd::desc() const {
    return QString("Disconnect");
}

} // namespace w7
