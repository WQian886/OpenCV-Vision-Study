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

    void setOperatorRegistry(const QHash<QString, OpMeta>& registry);

signals:
    void selectionChangedInScene();

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
