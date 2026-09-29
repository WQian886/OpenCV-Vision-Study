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
      m_opName(opName), m_displayName(displayName), m_category(category),
      m_uuid(QUuid::createUuid()) {   // === M6: 每个节点自带唯一 id ===
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
    // === M6: 记下拖动起点,松开时对比生成 MoveNodeCmd ===
    m_dragStartPos = pos();
    m_dragMoved    = false;
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

// === M6: 松开鼠标 → 若位置真的变了,通知外部生成 MoveNodeCmd ===
void OperatorNode::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    QGraphicsItem::mouseReleaseEvent(event);

    const QPointF newPos = pos();
    if ((newPos - m_dragStartPos).manhattanLength() > 0.5) {
        m_dragMoved = true;
        emit nodeMoved(this, m_dragStartPos, newPos);
    }
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

    // === pin 类型(一行小字) ===
    // 没有它,连线被拒时用户只能看到"类型不匹配"却不知道
    // 每个口到底是什么类型 —— 那等于让人猜。
    if (!m_inLabel.isEmpty() || !m_outLabel.isEmpty()) {
        font.setPointSize(7);
        p->setFont(font);
        p->setPen(QColor(90, 110, 150));
        p->drawText(QRectF(-W/2.0, H/2.0 - 15, W, 13),
                    Qt::AlignCenter,
                    m_inLabel.isEmpty()
                        ? m_outLabel
                        : (m_outLabel.isEmpty() ? m_inLabel
                                                 : m_inLabel + " → " + m_outLabel));
    }

    // 端口(输入左 / 输出右)
    p->setBrush(QBrush(borderColor));
    p->setPen(QPen(Qt::white, 2));
    p->drawEllipse(QPointF(-W/2.0, 0), PORT_HALF, PORT_HALF);  // 输入
    p->drawEllipse(QPointF( W/2.0, 0), PORT_HALF, PORT_HALF);  // 输出

    // === 每步运行状态(右上角小圆点) ===
    // 画在端口之后、方框之内,避免和端口图形重叠。
    if (m_stepStatus != StepStatus::None) {
        QColor dot;
        switch (m_stepStatus) {
            case StepStatus::Ok:    dot = QColor(40, 170, 60);   break;
            case StepStatus::Fail:  dot = QColor(220, 50, 50);  break;
            case StepStatus::Error: dot = QColor(240, 140, 20); break;  // 橙=算子自己报错
            default: break;
        }
        const QPointF c(W/2.0 - 12, -H/2.0 + 12);
        p->setBrush(QBrush(dot));
        p->setPen(QPen(Qt::white, 1));
        p->drawEllipse(c, 5, 5);
    }
}

OperatorNode::StepStatus OperatorNode::statusFromOutput(const QJsonObject& out) {
    // 顺序不能换:error 和 result 可能同时存在(算子判了 FAIL 又发现
    // 写盘失败),这时"执行失败"比"判废"更值得先说。
    if (out.contains("error"))          return StepStatus::Error;
    const QString r = out.value("result").toString();
    if (r == "FAIL")                    return StepStatus::Fail;
    // 没有 result 字段的算子(分割/计数/找模板)不做判定,
    // 它们没报错就是跑成功了 —— 画红点等于谎报。
    // 空的 out 同样按 Ok:算子跑了但什么字段都没产,不是失败。
    return StepStatus::Ok;
}

void OperatorNode::setSelected(bool sel) {
    QGraphicsItem::setSelected(sel);
    update();
}

// === P4: 右键菜单 - 删除当前节点 ===
// === P0 修复:不再 delete this,改为发信号让 MainWindow 走 PipelineView::removeNode ===
// 旧代码在这里 sc->removeItem(this) + delete this,但 PipelineView::m_nodes
// 仍持有该指针 → 悬垂指针 → 右键删完立刻 Run 会段错误。
void OperatorNode::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
    QMenu menu;
    // === 每步结果查看 ===
    // 没跑过 / 跑完又改了流水线,缓存里的结果已经不对了 —— 置灰,
    // 让"看到的是旧结果"这件事在菜单上就说得清,而不是点进去才发现。
    QAction* viewAct = menu.addAction(QObject::tr("查看该步输出"));
    viewAct->setEnabled(m_hasCachedResult);
    if (m_hasCachedResult) {
        viewAct->setToolTip(QObject::tr("显示这一步上次运行时的输出图和全部字段"));
    }
    menu.addSeparator();
    QAction* delAct = menu.addAction(QObject::tr("删除节点"));
    QAction* selected = menu.exec(event->screenPos());
    if (selected == viewAct) {
        emit viewStepResultRequested(this);
        event->accept();
    } else if (selected == delAct) {
        // 只发请求,真正的销毁由 PipelineView::removeNode 统一负责
        emit deleteRequested(this);
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace w7
