#include "connection_line.h"
#include "operator_node.h"
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>

namespace w7 {

ConnectionLine::ConnectionLine(OperatorNode* from, OperatorNode* to,
                               OperatorNode::PortSide fromSide,
                               OperatorNode::PortSide toSide,
                               QGraphicsItem* parent)
    : QGraphicsItem(parent),
      m_from(from), m_to(to),
      m_fromSide(fromSide), m_toSide(toSide) {
    setZValue(-1);
    // === 修复 2: 启用 selectable + 设置 z 值让选中效果可见 ===
    setFlag(QGraphicsItem::ItemIsSelectable, true);
}

void ConnectionLine::onNodesMoved() {
    if (m_from || m_to) update();
}

// === M7: 自适应控制点 ===
// Left/Right 端口 → 沿水平方向偏移(曲线在水平面延展)
// Top/Bottom 端口 → 沿垂直方向偏移(曲线在垂直面延展)
// 这样不管是水平连接还是垂直连接,曲线都"从端口平滑出发"。
QPointF ConnectionLine::controlPoint(QPointF endpoint,
                                    OperatorNode::PortSide side) const {
    switch (side) {
        case OperatorNode::PortSide::Left:
            return QPointF(endpoint.x() - CTRL_OFFSET, endpoint.y());
        case OperatorNode::PortSide::Right:
            return QPointF(endpoint.x() + CTRL_OFFSET, endpoint.y());
        case OperatorNode::PortSide::Top:
            return QPointF(endpoint.x(), endpoint.y() - CTRL_OFFSET);
        case OperatorNode::PortSide::Bottom:
            return QPointF(endpoint.x(), endpoint.y() + CTRL_OFFSET);
    }
    return endpoint;
}

QRectF ConnectionLine::boundingRect() const {
    if (!m_from || !m_to) return QRectF();
    QPointF p1 = m_from->portScenePos(m_fromSide);
    QPointF p2 = m_to->portScenePos(m_toSide);
    qreal xMin = std::min(p1.x(), p2.x()) - CTRL_OFFSET - 10;
    qreal xMax = std::max(p1.x(), p2.x()) + CTRL_OFFSET + 10;
    qreal yMin = std::min(p1.y(), p2.y()) - CTRL_OFFSET - 10;
    qreal yMax = std::max(p1.y(), p2.y()) + CTRL_OFFSET + 10;
    return QRectF(xMin, yMin, xMax - xMin, yMax - yMin);
}

// ============================================================
// 修复 2: shape() 返回加宽的路径,让命中区变成 16 像素宽的带
// === M7: 控制点按 fromSide/toSide 自适应 — 水平 or 垂直延展 ===
// ============================================================
QPainterPath ConnectionLine::shape() const {
    if (!m_from || !m_to) return QGraphicsItem::shape();

    QPointF p1 = m_from->portScenePos(m_fromSide);
    QPointF p2 = m_to->portScenePos(m_toSide);

    QPointF start = mapFromScene(p1);
    QPointF end   = mapFromScene(p2);
    QPointF ctrl1 = mapFromScene(controlPoint(p1, m_fromSide));
    QPointF ctrl2 = mapFromScene(controlPoint(p2, m_toSide));

    QPainterPath path;
    path.moveTo(start);
    path.cubicTo(ctrl1, ctrl2, end);

    QPainterPathStroker stroker;
    stroker.setWidth(HIT_WIDTH);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(path);
}

void ConnectionLine::paint(QPainter* p, const QStyleOptionGraphicsItem* option, QWidget*) {
    if (!m_from || !m_to) return;

    QPointF p1 = m_from->portScenePos(m_fromSide);
    QPointF p2 = m_to->portScenePos(m_toSide);

    QPointF start = mapFromScene(p1);
    QPointF end   = mapFromScene(p2);
    QPointF ctrl1 = mapFromScene(controlPoint(p1, m_fromSide));
    QPointF ctrl2 = mapFromScene(controlPoint(p2, m_toSide));

    QPainterPath path(start);
    path.cubicTo(ctrl1, ctrl2, end);

    p->setRenderHint(QPainter::Antialiasing, true);

    // === 修复 2: 选中时高亮显示 ===
    QColor lineColor = isSelected() ? QColor(255, 100, 0)   // 选中橙色
                                    : QColor(80, 80, 80);   // 默认灰
    int lineWidth = isSelected() ? 3 : 2;

    p->setPen(QPen(lineColor, lineWidth));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);

    // 箭头 — 朝向 end 的切线方向
    QPointF tangent(end.x() - ctrl2.x(), end.y() - ctrl2.y());
    qreal len = std::hypot(tangent.x(), tangent.y());
    if (len > 1e-3) {
        tangent /= len;
        QPointF normal(-tangent.y(), tangent.x());
        QPointF arrowTip = end;
        QPointF arrowBase = end - tangent * 8.0;
        QPolygonF arrow;
        arrow << arrowTip
              << arrowBase + normal * 4.0
              << arrowBase - normal * 4.0;
        p->setBrush(QBrush(lineColor));
        p->drawPolygon(arrow);
    }
}

} // namespace w7