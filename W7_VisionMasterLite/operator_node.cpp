#include "operator_node.h"
#include <QPainter>
#include <QBrush>
#include <QPen>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QMenu>
#include <QAction>

namespace w7 {

OperatorNode::OperatorNode(const QString& opName, const QString& displayName,
                           const QString& category, QGraphicsItem* parent)
    : QGraphicsObject(parent),
      m_opName(opName), m_displayName(displayName), m_category(category) {
    setFlag(QGraphicsItem::ItemIsMovable, true);
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    // 让 itemChange 能触发
    setFlag(QGraphicsItem::ItemSendsScenePositionChanges, true);
}

QRectF OperatorNode::boundingRect() const {
    return QRectF(-W/2.0, -H/2.0, W, H);
}

// ============================================================
// 端口 hit area(矩形简化)
// ============================================================
QRectF OperatorNode::outputPortRect() const {
    // 右端口 = boundingRect 右边缘的一个小矩形
    return QRectF(W/2.0 - PORT_HALF, -PORT_HALF,
                  PORT_HALF*2, PORT_HALF*2);
}

QRectF OperatorNode::inputPortRect() const {
    return QRectF(-W/2.0 - PORT_HALF, -PORT_HALF,
                  PORT_HALF*2, PORT_HALF*2);
}

bool OperatorNode::hitOutputPort(const QPointF& scenePos) const {
    // scenePos → item 坐标
    QPointF itemPos = mapFromScene(scenePos);
    return outputPortRect().contains(itemPos);
}

bool OperatorNode::hitInputPort(const QPointF& scenePos) const {
    QPointF itemPos = mapFromScene(scenePos);
    return inputPortRect().contains(itemPos);
}

QPointF OperatorNode::outputPortScenePos() const {
    return mapToScene(QPointF(W/2.0, 0));
}

QPointF OperatorNode::inputPortScenePos() const {
    return mapToScene(QPointF(-W/2.0, 0));
}

// ============================================================
// 节点位置变化时通知监听者(连线跟随)+ 限制在画布内
// ============================================================
QVariant OperatorNode::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionChange) {
        // 通知 scene:位置变了(连线跟随重画)
        if (scene()) scene()->update();
    }
    return QGraphicsItem::itemChange(change, value);
}

// ============================================================
// 鼠标按下:检测端口 → 启动拖拽;普通点击 → 默认拖动+选中
// === 修复 2: 移除 emit nodeClicked(没人连,且在 mousePress 中触发 selectionChanged 会重入闪退) ===
// ============================================================
void OperatorNode::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    QPointF scenePos = event->scenePos();
    qDebug() << "[NODE] mousePress enter, op=" << m_opName
             << "onOutputPort=" << hitOutputPort(scenePos);

    // === S2: 检查是否点在输出端口上 ===
    if (hitOutputPort(scenePos)) {
        QDrag* drag = new QDrag(this);
        QMimeData* mime = new QMimeData;
        mime->setData("application/x-vm-connection",
                      m_opName.toUtf8());
        drag->setMimeData(mime);

        Qt::DropAction action = drag->exec(Qt::CopyAction);
        event->accept();
        qDebug() << "[NODE] mousePress drag returned, action=" << action;
        return;
    }

    // 走默认(节点移动 + 选中 → scene 触发 selectionChanged → MainWindow 切换参数面板)
    QGraphicsItem::mousePressEvent(event);
    qDebug() << "[NODE] mousePress default done, isSelected=" << isSelected();
}

// ============================================================
// 绘制
// ============================================================
void OperatorNode::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) {
    QColor borderColor;
    if      (m_category == "positioning")    borderColor = QColor(50, 150, 250);
    else if (m_category == "measurement")    borderColor = QColor(250, 150, 50);
    else if (m_category == "identification") borderColor = QColor(50, 200, 100);
    else if (m_category == "detection")      borderColor = QColor(220, 50, 50);
    else                                     borderColor = QColor(120, 120, 120);

    QColor fillColor = isSelected() ? QColor(255, 240, 200)
                   : m_execHighlight ? QColor(220, 255, 220)
                   : QColor(245, 245, 245);

    p->setBrush(QBrush(fillColor));
    p->setPen(QPen(borderColor, isSelected() ? 3 : 2));
    p->drawRoundedRect(boundingRect(), 6, 6);

    // 中文显示名
    p->setPen(QColor(30, 30, 30));
    QFont font = p->font();
    font.setPointSize(10);
    font.setBold(true);
    p->setFont(font);
    p->drawText(QRectF(-W/2.0, -H/2.0, W, 24), Qt::AlignCenter, m_displayName);

    // 英文名
    font.setBold(false);
    font.setPointSize(8);
    p->setFont(font);
    p->setPen(QColor(100, 100, 100));
    p->drawText(QRectF(-W/2.0, -H/2.0 + 22, W, H - 24),
                Qt::AlignCenter, m_opName);

    // 端口(输入左 / 输出右)
    p->setBrush(QBrush(borderColor));
    p->setPen(QPen(Qt::white, 2));
    p->drawEllipse(QPointF(-W/2.0, 0), PORT_HALF, PORT_HALF);  // 输入
    p->drawEllipse(QPointF( W/2.0, 0), PORT_HALF, PORT_HALF);  // 输出
}

void OperatorNode::setSelected(bool sel) {
    QGraphicsItem::setSelected(sel);
    update();
}

// === P4: 右键菜单 - 删除当前节点 ===
void OperatorNode::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
    QMenu menu;
    QAction* delAct = menu.addAction(QObject::tr("删除节点"));
    QAction* selected = menu.exec(event->screenPos());
    if (selected == delAct) {
        // 从场景移除 + delete（让 scene 回调安全执行）
        QGraphicsScene* sc = scene();
        if (sc) sc->removeItem(this);
        // 通知 PipelineView 从 m_nodes 删除（通过 dynamic_cast 父容器）
        // 简化方案:PipelineView 重写了 selectionChanged,这里直接 delete 让 Qt 自动清理
        delete this;
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace w7
