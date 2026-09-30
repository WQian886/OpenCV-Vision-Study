#include "pipeline_view.h"
#include "operator_node.h"
#include "connection_line.h"
#include <QPainter>
#include <QWheelEvent>
#include <QGraphicsScene>
#include <QBrush>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <algorithm>

namespace w7 {

const QString PipelineView::MIME_OPERATOR    = "application/x-vm-operator";
const QString PipelineView::MIME_CONNECTION = "application/x-vm-connection";

PipelineView::PipelineView(QWidget* parent)
    : QGraphicsView(parent) {
    QGraphicsScene* scene = new QGraphicsScene(this);
    scene->setSceneRect(0, 0, 2000, 1500);
    setScene(scene);
    setRenderHint(QPainter::Antialiasing);
    // === perf v2: SmartViewportUpdate — Qt 自己判断每次该重画哪一块,
    //   对快速移动 / 拖拽比 MinimalViewportUpdate 更稳(更少残影) ===
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    // === perf v2: 关掉 painter state 保存,加速 paint() ===
    setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setDragMode(QGraphicsView::RubberBandDrag);
    setBackgroundBrush(QBrush(QColor(250, 250, 250)));

    setAcceptDrops(true);

    // === S3: 监听 scene 选中变化,转发给 MainWindow ===
    connect(scene, &QGraphicsScene::selectionChanged,
            this, &PipelineView::selectionChangedInScene);
}

OperatorNode* PipelineView::addNode(const QString& opName,
                                    const QString& displayName,
                                    const QString& category,
                                    const QPointF& pos) {
    OperatorNode* node = new OperatorNode(opName, displayName, category);
    scene()->addItem(node);
    node->setPos(pos);
    m_nodes.append(node);
    // === P0: 节点右键"删除"→ 转发给 MainWindow 统一销毁 ===
    // 节点不能自己 delete(PipelineView::m_nodes 会留悬垂指针)
    connect(node, &OperatorNode::deleteRequested,
            this, &PipelineView::nodeDeleteRequested);
    // === M6: 节点拖动 → 转发给 MainWindow 生成 MoveNodeCmd ===
    connect(node, &OperatorNode::nodeMoved,
            this, &PipelineView::nodeMovedSignal);
    // === 每步结果查看:节点右键"查看该步输出" → 转发给 MainWindow ===
    connect(node, &OperatorNode::viewStepResultRequested,
            this, &PipelineView::nodeViewResultRequested);
    // === pin 类型显示 ===
    // 图像源没有输入口,那一侧留空(节点上就不显示"入:")
    if (m_registry.contains(opName)) {
        const OpMeta& meta = m_registry.value(opName);
        const QString in  = meta.isSource ? QString() : w7::pinTypeName(meta.inType);
        node->setPinLabels(in, w7::pinTypeName(meta.outType));
    }
    return node;
}

// === M6: 按 UUID 找回节点(节点可能被删又重建,地址会变但 UUID 不变) ===
OperatorNode* PipelineView::findNodeByUuid(const QUuid& id) const {
    if (id.isNull()) return nullptr;
    for (auto* n : m_nodes) {
        if (n && n->uuid() == id) return n;
    }
    return nullptr;
}

// === M6: 找一条 from→to 的连线(多重连线时返回第一条) ===
ConnectionLine* PipelineView::findConnection(const QUuid& fromId,
                                             const QUuid& toId) const {
    for (auto* c : m_connections) {
        if (!c || !c->fromNode() || !c->toNode()) continue;
        if (c->fromNode()->uuid() == fromId && c->toNode()->uuid() == toId) {
            return c;
        }
    }
    return nullptr;
}

ConnectionLine* PipelineView::addConnection(OperatorNode* from, OperatorNode* to,
                                           OperatorNode::PortSide fromSide,
                                           OperatorNode::PortSide toSide) {
    if (!from || !to || from == to) return nullptr;

    // === 修复 3: 只删除"同 from→to 对"的旧线,允许多输入/多输出(fan-in/fan-out) ===
    for (auto it = m_connections.begin(); it != m_connections.end(); ) {
        if ((*it)->fromNode() == from && (*it)->toNode() == to) {
            scene()->removeItem(*it);
            delete *it;
            it = m_connections.erase(it);
        } else {
            ++it;
        }
    }

    ConnectionLine* line = new ConnectionLine(from, to, fromSide, toSide);
    scene()->addItem(line);
    m_connections.append(line);
    return line;
}

void PipelineView::clearPipeline() {
    // === 修复 1: 先从容器全部移除,再统一 delete ===
    for (auto* c : m_connections) scene()->removeItem(c);
    for (auto* n : m_nodes)     scene()->removeItem(n);
    qDeleteAll(m_connections);
    qDeleteAll(m_nodes);
    m_connections.clear();
    m_nodes.clear();
}

// === 修复 1 + 2: 安全删除(先从容器全部移除,再统一 delete) ===
void PipelineView::removeNode(OperatorNode* node) {
    if (!node) return;

    // 1. 收集所有要删的连线
    QList<ConnectionLine*> linesToDelete;
    for (auto* line : m_connections) {
        if (line && (line->fromNode() == node || line->toNode() == node)) {
            linesToDelete.append(line);
        }
    }

    // 2. 先把节点和所有关联连线从容器和 scene 移除
    for (auto* line : linesToDelete) {
        m_connections.removeAll(line);
        scene()->removeItem(line);
    }
    m_nodes.removeAll(node);
    scene()->removeItem(node);

    // 3. 统一 delete(此时 scene 已不再引用它们,update 不会访问野指针)
    qDeleteAll(linesToDelete);
    delete node;
}

void PipelineView::removeConnection(ConnectionLine* line) {
    if (!line) return;
    m_connections.removeAll(line);
    scene()->removeItem(line);
    delete line;
}

void PipelineView::highlightNode(const QString& opName, bool on) {
    for (auto* n : m_nodes) {
        if (n->opName() == opName) {
            n->setExecHighlight(on);
            break;
        }
    }
}

void PipelineView::setOperatorRegistry(const QHash<QString, OpMeta>& registry) {
    m_registry = registry;
}

// ============================================================
// 拖拽事件
// ============================================================
void PipelineView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(MIME_OPERATOR) ||
        event->mimeData()->hasFormat(MIME_CONNECTION)) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PipelineView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat(MIME_OPERATOR) ||
        event->mimeData()->hasFormat(MIME_CONNECTION)) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void PipelineView::dragLeaveEvent(QDragLeaveEvent* event) {
    event->accept();
}

void PipelineView::dropEvent(QDropEvent* event) {
    // === S2: 处理连线 mime ===
    if (event->mimeData()->hasFormat(MIME_CONNECTION)) {
        QByteArray payload = event->mimeData()->data(MIME_CONNECTION);
        const QString payloadStr = QString::fromUtf8(payload);
        // === M7: payload 格式 "opName|side",side ∈ {0,1,2,3} ===
        // 旧格式(只有 opName)在 M7 之前存的工程里可能存在,
        // 向后兼容:拆不出 | 就当 1(Right 输出端)。
        QString fromOpName;
        OperatorNode::PortSide fromSide = OperatorNode::PortSide::Right;
        const int sepIdx = payloadStr.lastIndexOf('|');
        if (sepIdx >= 0) {
            fromOpName = payloadStr.left(sepIdx);
            bool ok = false;
            const int sideInt = payloadStr.mid(sepIdx + 1).toInt(&ok);
            if (ok && sideInt >= 0 && sideInt <= 3) {
                fromSide = static_cast<OperatorNode::PortSide>(sideInt);
            }
        } else {
            fromOpName = payloadStr;
        }
        QPointF scenePos = mapToScene(event->position().toPoint());

        // 找源节点
        OperatorNode* fromNode = nullptr;
        for (auto* n : m_nodes) {
            if (n->opName() == fromOpName) { fromNode = n; break; }
        }
        if (!fromNode) { event->ignore(); return; }

        // === M7: 找最近的输入端口(Left 或 Top 都查),同时返回 toSide ===
        OperatorNode* targetNode = nullptr;
        OperatorNode::PortSide toSide = OperatorNode::PortSide::Left;
        qreal bestDist = 50.0;  // 容差半径 50 像素
        for (auto* n : m_nodes) {
            if (n == fromNode) continue;
            // 输入端口有 Left 和 Top 两个,逐个比对距离
            const QPointF inputLeft  = n->portScenePos(OperatorNode::PortSide::Left);
            const qreal   distLeft   = std::hypot(scenePos.x() - inputLeft.x(),
                                                  scenePos.y() - inputLeft.y());
            if (distLeft < bestDist) {
                bestDist = distLeft;
                targetNode = n;
                toSide = OperatorNode::PortSide::Left;
            }
            const QPointF inputTop   = n->portScenePos(OperatorNode::PortSide::Top);
            const qreal   distTop    = std::hypot(scenePos.x() - inputTop.x(),
                                                  scenePos.y() - inputTop.y());
            if (distTop < bestDist) {
                bestDist = distTop;
                targetNode = n;
                toSide = OperatorNode::PortSide::Top;
            }
        }
        if (targetNode) {
            // === M6: 发请求让 MainWindow 走 Command 栈(否则 Ctrl+Z 撤不掉连线) ===
            // === M7: 携带 fromSide/toSide ===
            emit connectionAddRequested(fromNode, targetNode, fromSide, toSide);
        }
        event->acceptProposedAction();
        return;
    }

    // === S1: 处理算子拖拽创建 ===
    if (event->mimeData()->hasFormat(MIME_OPERATOR)) {
        QByteArray payload = event->mimeData()->data(MIME_OPERATOR);
        QString opName = QString::fromUtf8(payload);

        OpMeta meta;
        if (m_registry.contains(opName)) {
            meta = m_registry.value(opName);
        } else {
            meta.name = opName;
            meta.display = opName;
            meta.category = "detection";
        }

        QPointF scenePos = mapToScene(event->position().toPoint());
        // === M6: 发请求让 MainWindow 走 Command 栈(否则 Ctrl+Z 撤不掉节点) ===
        emit nodeAddRequested(meta.name, meta.display, meta.category, scenePos);

        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void PipelineView::drawBackground(QPainter* p, const QRectF& rect) {
    QGraphicsView::drawBackground(p, rect);
    p->setPen(QPen(QColor(230, 230, 230), 1, Qt::DotLine));
    const int gridSize = 40;
    qreal left   = rect.left();
    qreal top    = rect.top();
    qreal right  = rect.right();
    qreal bottom = rect.bottom();
    for (qreal x = std::floor(left / gridSize) * gridSize; x < right; x += gridSize)
        p->drawLine(QPointF(x, top), QPointF(x, bottom));
    for (qreal y = std::floor(top / gridSize) * gridSize; y < bottom; y += gridSize)
        p->drawLine(QPointF(left, y), QPointF(right, y));
}

void PipelineView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        double factor = event->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
        scale(factor, factor);
        event->accept();
    } else {
        QGraphicsView::wheelEvent(event);
    }
}

} // namespace w7
