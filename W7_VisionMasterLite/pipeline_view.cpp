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

ConnectionLine* PipelineView::addConnection(OperatorNode* from, OperatorNode* to) {
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

    ConnectionLine* line = new ConnectionLine(from, to);
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
        QString fromOpName = QString::fromUtf8(payload);
        QPointF scenePos = mapToScene(event->position().toPoint());

        // 找源节点
        OperatorNode* fromNode = nullptr;
        for (auto* n : m_nodes) {
            if (n->opName() == fromOpName) { fromNode = n; break; }
        }
        if (!fromNode) { event->ignore(); return; }

        // 找最近的输入端口所在节点
        OperatorNode* targetNode = nullptr;
        qreal bestDist = 50.0;  // 容差半径 50 像素
        for (auto* n : m_nodes) {
            if (n == fromNode) continue;
            QPointF inputPos = n->inputPortScenePos();
            qreal dist = std::hypot(scenePos.x() - inputPos.x(),
                                    scenePos.y() - inputPos.y());
            if (dist < bestDist) {
                bestDist = dist;
                targetNode = n;
            }
        }
        if (targetNode) {
            // === M6: 发请求让 MainWindow 走 Command 栈(否则 Ctrl+Z 撤不掉连线) ===
            emit connectionAddRequested(fromNode, targetNode);
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
