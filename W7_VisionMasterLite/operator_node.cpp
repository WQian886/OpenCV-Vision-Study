#include "operator_node.h"
#include "connection_line.h"   // === perf fix: itemChange 里 dynamic_cast 用 ===
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
#include <limits>   // === M7: std::numeric_limits 用于最近端口判定 ===

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

// 节点本体矩形 —— 方框、文字都用它
QRectF OperatorNode::bodyRect() const {
    return QRectF(-W/2.0, -H/2.0, W, H);
}

// ============================================================
// boundingRect 必须包住所有真正画出去的像素
//
// 4 个端口圆画在四条边的**中心**(半径 PORT_HALF),再加一圈白色描边,
// 全部落在本体矩形**外面**:左端口圆心在 x=-W/2.0,最外缘到 -W/2.0-9。
// Qt 移动 item 时只按 boundingRect 擦除,超出范围的像素它根本不知道存在
// → 节点每拖动一次,4 个端口圆和描边就原地留下一圈残影。
//
// 之前只写 -W/2..W/2 是不够的:prepareGeometryChange() 通知 Qt 擦的是
// "旧 boundingRect",而旧 boundingRect 里压根没包含端口圆。
// ============================================================
QRectF OperatorNode::boundingRect() const {
    // 8(圆半径) + 1(白描边半宽) + 1(抗锯齿/边框描边余量)
    const qreal margin = PORT_HALF + 2.0;
    return bodyRect().adjusted(-margin, -margin, margin, margin);
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

// === M7: 上 / 下端口(垂直连线用) ===
QRectF OperatorNode::topPortRect() const {
    // 顶端口 = 顶边中心一个小矩形
    return QRectF(-PORT_HALF, -H/2.0 - PORT_HALF,
                  PORT_HALF*2, PORT_HALF*2);
}
QRectF OperatorNode::bottomPortRect() const {
    return QRectF(-PORT_HALF, H/2.0 - PORT_HALF,
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

// === M7: 通用 hit / pos,按 side 索引 ===
bool OperatorNode::hitPort(PortSide side, const QPointF& scenePos) const {
    const QPointF itemPos = mapFromScene(scenePos);
    switch (side) {
        case PortSide::Left:   return inputPortRect().contains(itemPos);
        case PortSide::Right:  return outputPortRect().contains(itemPos);
        case PortSide::Top:    return topPortRect().contains(itemPos);
        case PortSide::Bottom: return bottomPortRect().contains(itemPos);
    }
    return false;
}

QPointF OperatorNode::portScenePos(PortSide side) const {
    switch (side) {
        case PortSide::Left:   return mapToScene(QPointF(-W/2.0, 0));
        case PortSide::Right:  return mapToScene(QPointF( W/2.0, 0));
        case PortSide::Top:    return mapToScene(QPointF(0, -H/2.0));
        case PortSide::Bottom: return mapToScene(QPointF(0,  H/2.0));
    }
    return QPointF();
}

QPointF OperatorNode::outputPortScenePos() const {
    return portScenePos(PortSide::Right);
}

QPointF OperatorNode::inputPortScenePos() const {
    return portScenePos(PortSide::Left);
}

// ============================================================
// 节点位置变化时通知监听者(连线跟随)
//
// === 移动不需要(也不该)调 prepareGeometryChange() ===
// prepareGeometryChange 是给"几何变了"(改了 boundingRect/shape)用的。
// 纯移动由 QGraphicsItemPrivate::setPosHelper 自己处理:
// 它用**旧矩阵**算出旧 boundingRect、擦掉旧区域、按新位置重插 BSP 索引。
// 所以旧像素本来就会被擦掉 —— 之前那次残影的真凶是 boundingRect() 本身
// 圈不住端口圆(见本文件顶部),不是缺这句。
//
// 而且在 ItemPositionChange 里补这一句,等于鼠标每移动一个像素
// 就让 Qt 多做一次完整的索引重插 —— 实测这正是拖动卡顿的大头。
//
// 剩下的活:通知相连的连线把旧区域也擦掉,见 connection_line.cpp。
// ============================================================
QVariant OperatorNode::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged) {
        // move 完了 → 手术刀式只重画与本节点相连的连线
        if (scene()) {
            const QList<QGraphicsItem*> all = scene()->items();
            for (QGraphicsItem* gi : all) {
                if (auto* line = dynamic_cast<ConnectionLine*>(gi)) {
                    if (line->fromNode() == this || line->toNode() == this) {
                        // 必须走 onNodesMoved 而不是裸 update():
                        // update() 只擦"新" boundingRect,曲线原来那一段留在屏幕上。
                        line->onNodesMoved();
                    }
                }
            }
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

// ============================================================
// 鼠标按下:检测端口 → 启动拖拽;普通点击 → 默认拖动+选中
// === 修复 2: 移除 emit nodeClicked(没人连,且在 mousePress 中触发 selectionChanged 会重入闪退) ===
// === M7: 4 端口 — 找出离鼠标最近的端口(支持上下边的拖出) ===
// ============================================================
void OperatorNode::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    QPointF scenePos = event->scenePos();
    // === M6: 记下拖动起点,松开时对比生成 MoveNodeCmd ===
    m_dragStartPos = pos();
    m_dragMoved    = false;

    // === M7: 找最近端口 — 同时支持 4 边 ===
    // === perf fix: 用 item 本地坐标算距离,不再每次都 mapToScene 4 次 ===
    // 旧实现每次 mousePress 都 portScenePos(s) → mapToScene,4 次坐标转换。
    // 现在把 scenePos 转成 itemPos(1 次),距离计算全在本地坐标里。
    // 角点 4 端口挤在一起时,本地坐标距离更稳定(不依赖 scene 缩放/平移)。
    PortSide bestSide = PortSide::Right;   // 默认 Right(向后兼容)
    qreal    bestDist = std::numeric_limits<qreal>::infinity();
    const QPointF itemPos = mapFromScene(scenePos);
    // 端口在 item 坐标系里的中心(注意:rect 中心 = 边中心)
    auto portItemPos = [](PortSide s) -> QPointF {
        switch (s) {
            case PortSide::Left:   return QPointF(-W/2.0, 0);
            case PortSide::Right:  return QPointF( W/2.0, 0);
            case PortSide::Top:    return QPointF(0, -H/2.0);
            case PortSide::Bottom: return QPointF(0,  H/2.0);
        }
        return QPointF();
    };
    const PortSide sides[4] = { PortSide::Left, PortSide::Right,
                                PortSide::Top,  PortSide::Bottom };
    for (PortSide s : sides) {
        const QPointF center = portItemPos(s);
        const qreal d = std::hypot(itemPos.x() - center.x(),
                                  itemPos.y() - center.y());
        if (d < bestDist) {
            bestDist = d;
            bestSide = s;
        }
    }
    // 命中阈值:PORT_HALF*2 = 16 像素的容差半径
    m_dragSide = bestSide;
    qDebug() << "[NODE] mousePress enter, op=" << m_opName
             << "nearestSide=" << static_cast<int>(bestSide)
             << "dist=" << bestDist;

    if (bestDist <= PORT_HALF * 2.0 && sideIsOutput(bestSide)) {
        // === payload 编码 "uuid|side" ===
        //
        // 之前带的是 opName,落点时按算子名去画布里找第一个匹配的。
        // 算子名【不唯一】—— 画布里可以有两个 color_segment,从第二个拖出去
        // 会接到第一个身上。更糟的是 addConnection 开头会删掉"同 from→to 对"
        // 的旧线,于是第一个节点原有的连线被顶掉,看上去就像节点在乱跳。
        // UUID 才是稳定身份(节点删了重建也还是同一个)。
        QDrag* drag = new QDrag(this);
        QMimeData* mime = new QMimeData;
        const QString payload = QString("%1|%2")
            .arg(m_uuid.toString(QUuid::WithoutBraces))
            .arg(static_cast<int>(bestSide));
        mime->setData("application/x-vm-connection",
                      payload.toUtf8());
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
    p->drawRoundedRect(bodyRect(), 6, 6);

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

    // 端口(4 个:左输入 / 右输出 / 上输入 / 下输出 — M7 升级)
    p->setBrush(QBrush(borderColor));
    p->setPen(QPen(Qt::white, 2));
    p->drawEllipse(QPointF(-W/2.0, 0), PORT_HALF, PORT_HALF);  // 左:输入
    p->drawEllipse(QPointF( W/2.0, 0), PORT_HALF, PORT_HALF);  // 右:输出
    p->drawEllipse(QPointF(0, -H/2.0), PORT_HALF, PORT_HALF);  // 上:输入
    p->drawEllipse(QPointF(0,  H/2.0), PORT_HALF, PORT_HALF);  // 下:输出

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
