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
#include <QPixmap>
#include "operators/pin_types.h"
#include "operator_node.h"   // === M7: 拿 PortSide 枚举 ===

namespace w7 {

class OperatorNode;
class ConnectionLine;

struct OpMeta {
    QString name;
    QString display;
    QString category;
    QString description;
    // === pin 类型(画布上显示,连线时检查) ===
    // 存在这里而不是让 PipelineView 去 new 算子:画布只负责画,
    // 不该为了显示一个标签就去构造带 OpenCV 依赖的算子对象。
    PinType inType  = PinType::Any;
    PinType outType = PinType::Any;
    bool     isSource = false;
};

class PipelineView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PipelineView(QWidget* parent = nullptr);

    OperatorNode* addNode(const QString& opName, const QString& displayName,
                          const QString& category, const QPointF& pos);
    // === M7: addConnection 加 fromSide / toSide 参数 ===
    ConnectionLine* addConnection(OperatorNode* from, OperatorNode* to,
                                 OperatorNode::PortSide fromSide = OperatorNode::PortSide::Right,
                                 OperatorNode::PortSide toSide   = OperatorNode::PortSide::Left);

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
    // === M7: 加 fromSide/toSide — 拖出时携带,落点时确定 ===
    void connectionAddRequested(OperatorNode* from, OperatorNode* to,
                                OperatorNode::PortSide fromSide,
                                OperatorNode::PortSide toSide);
    // === 每步结果查看:节点请求显示该步输出(来自节点右键菜单) ===
    void nodeViewResultRequested(OperatorNode* node);
    // === 单步执行:节点请求"只跑到我这里"(来自节点右键菜单) ===
    void nodeSingleStepRequested(OperatorNode* node);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // === 点阵网格瓦片 ===
    // 原来 drawBackground 每次都按 40px 逐格 drawLine 画点阵:
    // 要重画多大区域,就画多少格。而拖动节点时连线会把"跨半个画布"的
    // 矩形标脏,于是每帧要把几百条带抗锯齿的 DotLine 虚线重描一遍
    // → 拖一下就卡成幻灯片(节点跟不住鼠标,看着像在乱跑)。
    // 改成画一次 40x40 的小块贴图,之后 drawTiledPixmap 一次搞定,
    // 开销与区域大小无关。
    QPixmap gridTile();
    QPixmap m_gridTile;

    QList<OperatorNode*>   m_nodes;
    QList<ConnectionLine*> m_connections;
    QHash<QString, OpMeta> m_registry;

    static const QString MIME_OPERATOR;
    static const QString MIME_CONNECTION;
};

} // namespace w7
