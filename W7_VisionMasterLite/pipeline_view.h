#pragma once
// ============================================================
// PipelineView — 流程图画布(QGraphicsView 子类)
// - 接收算子拖拽创建节点(S1)
// - 接收节点端口连线(S2)
// - 监听选中变化转发给 MainWindow(S3)
// ============================================================

#include <QGraphicsView>
#include <QHash>
#include <QString>
#include <QList>
#include <QUuid>
#include <QPointF>

namespace w7 {

class OperatorNode;
class ConnectionLine;

struct OpMeta {
    QString name;
    QString display;
    QString category;
    QString description;
};

class PipelineView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PipelineView(QWidget* parent = nullptr);

    OperatorNode* addNode(const QString& opName, const QString& displayName,
                          const QString& category, const QPointF& pos);
    ConnectionLine* addConnection(OperatorNode* from, OperatorNode* to);

    void clearPipeline();
    void highlightNode(const QString& opName, bool on);

    const QList<OperatorNode*>& nodes() const { return m_nodes; }
    const QList<ConnectionLine*>& connections() const { return m_connections; }

    QList<OperatorNode*>& nodesRef() { return m_nodes; }
    QList<ConnectionLine*>& connectionsRef() { return m_connections; }

    void removeNode(OperatorNode* node);
    void removeConnection(ConnectionLine* line);

    // === M6: 按 UUID 找回节点 ===
    // Command 在 undo/redo 时用它定位节点 —— 节点被删又重建后地址会变,
    // 但 UUID 保持不变,所以命令不会操作到野指针。
    OperatorNode* findNodeByUuid(const QUuid& id) const;
    ConnectionLine* findConnection(const QUuid& fromId, const QUuid& toId) const;

    void setOperatorRegistry(const QHash<QString, OpMeta>& registry);

signals:
    void selectionChangedInScene();
    // === P0: 节点请求删除(来自节点右键菜单) ===
    // 节点自己不 delete,由 MainWindow 调 removeNode 统一销毁
    void nodeDeleteRequested(OperatorNode* node);
    // === M6: 节点被拖动 ===
    void nodeMovedSignal(OperatorNode* node, const QPointF& oldPos, const QPointF& newPos);
    // === M6: 画布请求创建节点(由 MainWindow 决定是否包成 Command) ===
    // 画布自己不 addNode —— 否则新增动作绕过了命令栈,Ctrl+Z 撤不掉
    void nodeAddRequested(const QString& opName, const QString& display,
                          const QString& category, const QPointF& pos);
    // === M6: 画布请求连线 ===
    void connectionAddRequested(OperatorNode* from, OperatorNode* to);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QList<OperatorNode*>   m_nodes;
    QList<ConnectionLine*> m_connections;
    QHash<QString, OpMeta> m_registry;

    static const QString MIME_OPERATOR;
    static const QString MIME_CONNECTION;
};

} // namespace w7
