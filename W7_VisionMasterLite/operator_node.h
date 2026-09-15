#pragma once
// ============================================================
// OperatorNode — 流程图节点(QGraphicsItem 子类)
// 画矩形 + 标题 + 输入/输出端口 + 鼠标交互(S2: 端口可拖)
// ============================================================

#include <QGraphicsItem>
#include <QString>
#include <QJsonObject>

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

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;  // === P4 ===
    QRectF outputPortRect() const;
    QRectF inputPortRect() const;

private:
    QString m_opName;
    QString m_displayName;
    QString m_category;
    bool    m_execHighlight = false;
    QJsonObject m_params;   // === S3 ===

    static constexpr int W = 140;
    static constexpr int H = 60;
    static constexpr int PORT_HALF = 8;
};

} // namespace w7

