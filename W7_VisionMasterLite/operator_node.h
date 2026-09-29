#pragma once
// ============================================================
// OperatorNode — 流程图节点(QGraphicsItem 子类)
// 画矩形 + 标题 + 输入/输出端口 + 鼠标交互(S2: 端口可拖)
// ============================================================

#include <QGraphicsItem>
#include <QString>
#include <QJsonObject>
#include <QUuid>

class QDrag;

namespace w7 {

class OperatorNode : public QGraphicsObject {
    Q_OBJECT
public:
    OperatorNode(const QString& opName, const QString& displayName,
                 const QString& category, QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

    QString opName() const { return m_opName; }
    QString displayName() const { return m_displayName; }
    QString category() const { return m_category; }

    // === M6: 稳定唯一标识 ===
    // 不用裸指针做身份:Command / Save-Load 都需要在节点被删又重建后
    // 仍能找回同一个逻辑节点(重建会换新地址)。
    QUuid uuid() const { return m_uuid; }
    void setUuid(const QUuid& id) { m_uuid = id; }

    void setSelected(bool sel);
    bool isExecHighlighted() const { return m_execHighlight; }
    void setExecHighlight(bool h) { m_execHighlight = h; update(); }

    // === S2: 端口 hit test ===
    bool hitOutputPort(const QPointF& scenePos) const;
    bool hitInputPort(const QPointF& scenePos) const;
    QPointF outputPortScenePos() const;
    QPointF inputPortScenePos() const;

    // === S3: 节点参数 ===
    void setParams(const QJsonObject& p) { m_params = p; }
    QJsonObject params() const { return m_params; }

    // === S2: itemChange + 节点移动通知 ===
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

signals:
    // === S3: 节点被点击(让 MainWindow 切换参数面板) ===
    void nodeClicked(OperatorNode* node);
    // === S3: 参数被外部更新(从参数面板) ===
    void paramsChanged(const QJsonObject& params);
    // === P0: 请求删除自己 ===
    // 节点不能自己 delete this —— PipelineView::m_nodes 仍持有该指针,
    // 留下悬垂指针后 Run 引擎遍历会段错误。
    // 改为发信号,由 MainWindow 统一走 PipelineView::removeNode(已实现正确的清理逻辑)。
    void deleteRequested(OperatorNode* node);
    // === M6: 拖动结束(位置真的变了才发) → 生成 MoveNodeCmd ===
    void nodeMoved(OperatorNode* node, const QPointF& oldPos, const QPointF& newPos);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;  // === M6 ===
    void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;  // === P4 ===
    QRectF outputPortRect() const;
    QRectF inputPortRect() const;

private:
    QString m_opName;
    QString m_displayName;
    QString m_category;
    QUuid    m_uuid;        // === M6 ===
    bool    m_execHighlight = false;
    QJsonObject m_params;   // === S3 ===

    // === M6: 拖动起点(松开时用来生成 MoveNodeCmd) ===
    QPointF m_dragStartPos;
    bool    m_dragMoved = false;

    static constexpr int W = 140;
    static constexpr int H = 60;
    static constexpr int PORT_HALF = 8;
};

} // namespace w7

