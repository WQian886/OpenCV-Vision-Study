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

    // === perf fix v2: 缓存 shape()/boundingRect() ===
    // Qt 在 hover/select/拖拽过程中会反复调 shape() 判定命中区;
    // shape() 里有 QPainterPathStroker::createStroke(O(N) 贝塞尔采样),
    // 不缓存会让 hover 频繁时持续重算。端点不变 → 直接返回缓存。
    mutable bool        m_shapeCacheValid = false;
    mutable QPainterPath m_cachedShapePath;
    mutable QRectF      m_cachedBoundingRect;
    // === perf v3: 上次 paint 时的 boundingRect,用于自动 union 旧位置防止残影
    //   (Qt 不允许外部对它调 prepareGeometryChange())
    mutable QRectF      m_lastBoundingRect;
    // 端点变化侦测:用 4 个 double 描述两个 scene 坐标 + side 索引
    struct EndpointKey {
        qreal x1, y1, x2, y2;
        int  s1, s2;
        bool operator==(const EndpointKey& o) const {
            return x1==o.x1 && y1==o.y1 && x2==o.x2 && y2==o.y2
                && s1==o.s1 && s2==o.s2;
        }
    };
    mutable EndpointKey m_lastKey{};
    EndpointKey currentKey() const;
};

} // namespace w7
