#pragma once
// ============================================================
// ConnectionLine — 节点间连线(QGraphicsItem 子类)
// 用 QPainterPath 画贝塞尔曲线,从 fromNode 输出端 → toNode 输入端
// 节点移动时自动跟随
// S3 修复:加 ItemIsSelectable + 加宽 shape() hit 区
// ============================================================

#include <QGraphicsItem>

namespace w7 {

class OperatorNode;

class ConnectionLine : public QGraphicsItem {
public:
    ConnectionLine(OperatorNode* from, OperatorNode* to,
                   QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

    // === 修复 2: override shape() 让 hit 区变宽 ===
    QPainterPath shape() const override;

    OperatorNode* fromNode() const { return m_from; }
    OperatorNode* toNode() const { return m_to; }

    // 让连线在节点移动时自动更新位置
    void onNodesMoved();

private:
    OperatorNode* m_from;
    OperatorNode* m_to;

    // 贝塞尔曲线控制点偏移
    static constexpr int CTRL_OFFSET = 60;
    // === 修复 2: hit 区宽度 ===
    static constexpr int HIT_WIDTH   = 16;   // 16 像素宽的命中带
};

} // namespace w7
