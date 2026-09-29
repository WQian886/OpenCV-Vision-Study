// ============================================================
// 节点相关 Command 实现 (M6 Step2/3)
// 全部按 UUID 定位节点,找不到就静默 no-op(不崩)
// ============================================================

#include "node_commands.h"
#include "../pipeline_view.h"
#include "../operator_node.h"
#include "../connection_line.h"

namespace w7 {

// ============================================================
// AddNodeCmd
// ============================================================
AddNodeCmd::AddNodeCmd(PipelineView* canvas, const QString& opName,
                       const QString& displayName, const QString& category,
                       const QPointF& pos)
    : m_canvas(canvas), m_opName(opName), m_display(displayName),
      m_category(category), m_pos(pos) {}

void AddNodeCmd::exec() {
    if (!m_canvas) return;
    OperatorNode* n = m_canvas->addNode(m_opName, m_display, m_category, m_pos);
    if (!n) return;
    // 首次创建记下 UUID;redo 时沿用它,保证已有的连线还能对上
    if (!m_haveUuid) {
        m_uuid = n->uuid();
        m_haveUuid = true;
    } else {
        n->setUuid(m_uuid);
    }
}

void AddNodeCmd::undo() {
    if (!m_canvas || !m_haveUuid) return;
    if (OperatorNode* n = m_canvas->findNodeByUuid(m_uuid)) {
        m_canvas->removeNode(n);
    }
}

void AddNodeCmd::redo() {
    exec();   // exec 里会复用已记住的 UUID
}

QString AddNodeCmd::desc() const {
    return QString("Add %1").arg(m_opName);
}

// ============================================================
// DeleteNodeCmd
// ============================================================
DeleteNodeCmd::DeleteNodeCmd(PipelineView* canvas, OperatorNode* node)
    : m_canvas(canvas) {
    if (!node) return;
    // 快照节点状态,供 undo 重建
    m_opName  = node->opName();
    m_display = node->displayName();
    m_category = node->category();
    m_pos     = node->pos();
    m_params  = node->params();
    m_uuid    = node->uuid();

    // 快照被连带删除的连线(removeNode 会一起删掉它们)
    if (m_canvas) {
        for (auto* line : m_canvas->connections()) {
            if (!line || !line->fromNode() || !line->toNode()) continue;
            bool touches = (line->fromNode() == node) || (line->toNode() == node);
            if (touches) {
                m_edges.append({line->fromNode()->uuid(), line->toNode()->uuid()});
            }
        }
    }
}

void DeleteNodeCmd::exec() {
    if (!m_canvas) return;
    if (OperatorNode* n = m_canvas->findNodeByUuid(m_uuid)) {
        m_canvas->removeNode(n);   // 连带清理连线
    }
}

void DeleteNodeCmd::undo() {
    if (!m_canvas) return;
    // 重建节点,沿用原 UUID
    OperatorNode* n = m_canvas->addNode(m_opName, m_display, m_category, m_pos);
    if (!n) return;
    n->setUuid(m_uuid);
    n->setParams(m_params);

    // 重建连线:两端节点都还在才连得上
    for (const auto& e : m_edges) {
        OperatorNode* from = m_canvas->findNodeByUuid(e.from);
        OperatorNode* to   = m_canvas->findNodeByUuid(e.to);
        if (from && to) {
            m_canvas->addConnection(from, to);
        }
    }
}

void DeleteNodeCmd::redo() {
    exec();
}

QString DeleteNodeCmd::desc() const {
    return QString("Delete %1").arg(m_opName);
}

// ============================================================
// MoveNodeCmd
// ============================================================
MoveNodeCmd::MoveNodeCmd(PipelineView* canvas, const QUuid& nodeId,
                         const QPointF& oldPos, const QPointF& newPos)
    : m_canvas(canvas), m_nodeId(nodeId),
      m_oldPos(oldPos), m_newPos(newPos) {}

void MoveNodeCmd::exec() {
    if (!m_canvas) return;
    if (OperatorNode* n = m_canvas->findNodeByUuid(m_nodeId)) {
        n->setPos(m_newPos);
    }
}

void MoveNodeCmd::undo() {
    if (!m_canvas) return;
    if (OperatorNode* n = m_canvas->findNodeByUuid(m_nodeId)) {
        n->setPos(m_oldPos);
    }
}

void MoveNodeCmd::redo() {
    exec();
}

QString MoveNodeCmd::desc() const {
    return QString("Move node");
}

} // namespace w7
