#pragma once
// ============================================================
// 节点相关 Command 子类 (M6 Step2/3)
//
// 关键设计:所有 Command 都用 QUuid 记录节点身份,而不是裸指针。
//   原因:撤销删除会"重建"节点(新地址),若 Command 存旧指针,
//         后续撤销会操作已释放内存 → 悬垂指针。
//   UUID 在节点创建时生成,重建时沿用 → 命令永远能找回正确的节点。
//
// 找不到 UUID 对应的节点时,所有命令静默 no-op(而不是崩溃):
//   例如 Load 了新工程把节点全清了,此时按 Ctrl+Z 不应该闪退。
// ============================================================

#include "command.h"
#include "../operator_node.h"   // === M7: PortSide ===
#include <QPointF>
#include <QString>
#include <QJsonObject>
#include <QUuid>

namespace w7 {

class PipelineView;

// ============================================================
// AddNodeCmd — 拖算子库创建节点
//   exec(): 建节点,记下它的 UUID
//   undo(): 删掉这个节点
//   redo(): 重建节点并沿用原 UUID(否则后续边的 UUID 对不上)
// ============================================================
class AddNodeCmd : public Command {
public:
    AddNodeCmd(PipelineView* canvas, const QString& opName,
               const QString& displayName, const QString& category,
               const QPointF& pos);
    void exec() override;
    void undo() override;
    void redo() override;
    QString desc() const override;
private:
    PipelineView* m_canvas;
    QString m_opName;
    QString m_display;
    QString m_category;
    QPointF  m_pos;
    QUuid     m_uuid;      // exec 后由新节点填充,redo 时复用
    bool      m_haveUuid = false;
};

// ============================================================
// DeleteNodeCmd — 删除节点(含它身上的连线)
//   undo() 麻烦在于节点已被 destroy,只能重建。所以必须存下
//   重建所需的全部状态:op/display/category/pos/params/UUID,
//   以及被连带删除的每条连线的两端 UUID。
// ============================================================
class DeleteNodeCmd : public Command {
public:
    DeleteNodeCmd(PipelineView* canvas, class OperatorNode* node);
    void exec() override;
    void undo() override;
    void redo() override;
    QString desc() const override;
private:
    PipelineView* m_canvas;

    // 被删节点的快照(重建用)
    QString m_opName;
    QString m_display;
    QString m_category;
    QPointF m_pos;
    QJsonObject m_params;
    QUuid    m_uuid;

    // 被连带删除的连线:每项是 (fromUuid, toUuid, fromSide, toSide)
    // === M7: 加 side 字段,undo 重建时用对的端口连 ===
    struct EdgeRef {
        QUuid from;
        QUuid to;
        OperatorNode::PortSide fromSide = OperatorNode::PortSide::Right;
        OperatorNode::PortSide toSide   = OperatorNode::PortSide::Left;
    };
    QList<EdgeRef> m_edges;
};

// ============================================================
// MoveNodeCmd — 拖动节点改变位置
//   最简单的一个:只记 oldPos / newPos
// ============================================================
class MoveNodeCmd : public Command {
public:
    MoveNodeCmd(PipelineView* canvas, const QUuid& nodeId,
                const QPointF& oldPos, const QPointF& newPos);
    void exec() override;
    void undo() override;
    void redo() override;
    QString desc() const override;
private:
    PipelineView* m_canvas;
    QUuid    m_nodeId;
    QPointF  m_oldPos;
    QPointF  m_newPos;
};

} // namespace w7
