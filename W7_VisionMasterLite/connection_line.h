#pragma once
// ============================================================
// ConnectionLine — 节点间连线(QGraphicsItem 子类)
// 用 QPainterPath 画贝塞尔曲线,从 fromNode 输出端 → toNode 输入端
// 节点移动时自动跟随
// S3 修复:加 ItemIsSelectable + 加宽 shape() hit 区
// M7 升级:4 端口(Left/Right/Top/Bottom),连线两端记 side,贝塞尔控制点
// 按 side 自适应方向(水平或垂直)。
// ============================================================

#include <QGraphicsItem>
#include "operator_node.h"

namespace w7 {

class ConnectionLine : public QGraphicsItem {
public:
    ConnectionLine(OperatorNode* from, OperatorNode* to,
                   OperatorNode::PortSide fromSide,
                   OperatorNode::PortSide toSide,
                   QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

    // === 修复 2: override shape() 让 hit 区变宽 ===
    QPainterPath shape() const override;

    OperatorNode* fromNode() const { return m_from; }
    OperatorNode* toNode() const { return m_to; }
    OperatorNode::PortSide fromSide() const { return m_fromSide; }
    OperatorNode::PortSide toSide()   const { return m_toSide; }

    // 让连线在节点移动时自动更新位置
    void onNodesMoved();

private:
    OperatorNode* m_from;
    OperatorNode* m_to;
    OperatorNode::PortSide m_fromSide;
    OperatorNode::PortSide m_toSide;

    // 贝塞尔曲线控制点偏移
    static constexpr int CTRL_OFFSET = 60;
    // === 修复 2: hit 区宽度 ===
    static constexpr int HIT_WIDTH   = 16;   // 16 像素宽的命中带

    // === M7: 自适应控制点 — 起点/终点分别在左右 → 水平延展;
    // 在上下 → 垂直延展;左右交叉(右下→左上等) → 走主延展方向的那条 ===
    QPointF controlPoint(QPointF endpoint, OperatorNode::PortSide side) const;
};

} // namespace w7
