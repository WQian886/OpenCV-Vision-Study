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
// === M7: 也要携带 fromSide / toSide ===
//   和 AddEdgeCmd 成对:那条是"新建时记下来",这条是"删掉时记下来",
//   undo 才还原得回去。少记一个字段,撤销回来就变成默认方向 ——
//   一条竖线静默变成横线,连线关系还在,所以没有任何报错。
// ============================================================
class RemoveEdgeCmd : public Command {
public:
    RemoveEdgeCmd(PipelineView* canvas, const QUuid& fromId, const QUuid& toId,
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

} // namespace w7
