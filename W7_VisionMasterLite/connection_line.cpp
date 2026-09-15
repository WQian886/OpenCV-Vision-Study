#include "connection_line.h"
#include "operator_node.h"
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>

namespace w7 {

ConnectionLine::ConnectionLine(OperatorNode* from, OperatorNode* to,
                               QGraphicsItem* parent)
    : QGraphicsItem(parent), m_from(from), m_to(to) {
    setZValue(-1);
    // === 修复 2: 启用 selectable + 设置 z 值让选中效果可见 ===
    setFlag(QGraphicsItem::ItemIsSelectable, true);
}

void ConnectionLine::onNodesMoved() {
    if (m_from || m_to) update();
}

QRectF ConnectionLine::boundingRect() const {
    if (!m_from || !m_to) return QRectF();
    QPointF p1 = m_from->outputPortScenePos();
    QPointF p2 = m_to->inputPortScenePos();
    qreal xMin = std::min(p1.x(), p2.x()) - CTRL_OFFSET - 10;
    qreal xMax = std::max(p1.x(), p2.x()) + CTRL_OFFSET + 10;
    qreal yMin = std::min(p1.y(), p2.y()) - 30;
    qreal yMax = std::max(p1.y(), p2.y()) + 30;
    return QRectF(xMin, yMin, xMax - xMin, yMax - yMin);
}

// ============================================================
// 修复 2: shape() 返回加宽的路径,让命中区变成 16 像素宽的带
// ============================================================
QPainterPath ConnectionLine::shape() const {
    if (!m_from || !m_to) return QGraphicsItem::shape();

    QPointF p1 = m_from->outputPortScenePos();
    QPointF p2 = m_to->inputPortScenePos();

    QPointF start = mapFromScene(p1);
    QPointF end   = mapFromScene(p2);
    QPointF ctrl1(start.x() + CTRL_OFFSET, start.y());
    QPointF ctrl2(end.x()   - CTRL_OFFSET, end.y());

    // 先画一条细的贝塞尔路径
    QPainterPath path;
    path.moveTo(start);
    path.cubicTo(ctrl1, ctrl2, end);

    // 用 stroker 把路径加宽(HIT_WIDTH 像素)
    QPainterPathStroker stroker;
    stroker.setWidth(HIT_WIDTH);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(path);
}

void ConnectionLine::paint(QPainter* p, const QStyleOptionGraphicsItem* option, QWidget*) {
    if (!m_from || !m_to) return;

    QPointF p1 = m_from->outputPortScenePos();
    QPointF p2 = m_to->inputPortScenePos();

    QPointF start = mapFromScene(p1);
    QPointF end   = mapFromScene(p2);

    QPointF ctrl1(start.x() + CTRL_OFFSET, start.y());
    QPointF ctrl2(end.x()   - CTRL_OFFSET, end.y());

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

    // 箭头
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
