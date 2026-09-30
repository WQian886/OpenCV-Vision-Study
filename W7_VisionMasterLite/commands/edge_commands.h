#pragma once
// ============================================================
// 连线相关 Command 子类 (M6 Step4)
//
// AddEdgeCmd 记录两端节点的 UUID(不是指针):
//   两端节点若被删,addConnection 拿不到指针,命令退化为 no-op。
//   但由于 DeleteNodeCmd::undo 会先重建节点和它们的连线,
//   正常操作序列下两端总是存在的。
//
// 为什么不把 AddEdgeCmd 做成 DeleteEdgeCmd 的兄弟类?
//   Delete 节点时连线是被"连带"删的,已经整体记在 DeleteNodeCmd 里,
//   单独再建一个 RemoveEdgeCmd 会和它重复、互相打架。
//   独立删一条连线(Delete 键选中线)走单独的 RemoveEdgeCmd。
// ============================================================

#include "command.h"
#include "../operator_node.h"   // === M7: PortSide ===
#include <QUuid>
#include <QString>

namespace w7 {

class PipelineView;

// ============================================================
// AddEdgeCmd — 端口拖拽连线
// === M7: 携带 fromSide / toSide,支持 4 端口 + 垂直连线 ===
// ============================================================
class AddEdgeCmd : public Command {
public:
    AddEdgeCmd(PipelineView* canvas, const QUuid& fromId, const QUuid& toId,
               OperatorNode::PortSide fromSide = OperatorNode::PortSide::Right,
               OperatorNode::PortSide toSide   = OperatorNode::PortSide::Left);
    void exec() override;
    void undo() override;
    void redo() override;
    QString desc() const override;
private:
    PipelineView* m_canvas;
    QUuid m_fromId;
    QUuid m_toId;
    OperatorNode::PortSide m_fromSide;   // === M7 ===
    OperatorNode::PortSide m_toSide;     // === M7 ===
};

// ============================================================
// RemoveEdgeCmd — 单独删除一条连线(选中连线按 Delete)
// ============================================================
class RemoveEdgeCmd : public Command {
public:
    RemoveEdgeCmd(PipelineView* canvas, const QUuid& fromId, const QUuid& toId);
    void exec() override;
    void undo() override;
    void redo() override;
    QString desc() const override;
private:
    PipelineView* m_canvas;
    QUuid m_fromId;
    QUuid m_toId;
};

} // namespace w7
