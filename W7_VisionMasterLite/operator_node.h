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
    // === 每步结果查看:节点在画布上显示的运行态 ===
    // 以前只有 m_execHighlight(执行瞬间闪一下绿),跑完就没了 ——
    // 一条 8 步的流水线跑完,画布上看不出哪一步出了错,
    // 只能去日志里翻。VM 的做法是每步跑完留个状态标记。
    enum class StepStatus { None, Ok, Fail, Error };

    OperatorNode(const QString& opName, const QString& displayName,
                 const QString& category, QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;
    // 节点本体矩形(方框本体)。boundingRect() 比它大一圈 ——
    // 画框和文字都按 bodyRect 算,只有 boundingRect 负责"擦除范围"。
    QRectF bodyRect() const;

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

    // === M7: 4 端口升级(左右 + 上下),支持水平+垂直连线 ===
    // 端口位于节点边的中心位置;按 side 索引可拿到对应位置的 hit/pos。
    // Left / Top 是 Input,Right / Bottom 是 Output。
    // 鼠标按下时按距离最近判定 hit,避免 4 个端口挤在角点时误中。
    enum class PortSide { Left, Right, Top, Bottom };
    bool        hitPort(PortSide side, const QPointF& scenePos) const;
    QPointF     portScenePos(PortSide side) const;
    static bool sideIsInput(PortSide s) { return s == PortSide::Left || s == PortSide::Top; }
    static bool sideIsOutput(PortSide s) { return s == PortSide::Right || s == PortSide::Bottom; }
    PortSide    dragSide() const { return m_dragSide; }   // 鼠标按下时落在哪条边

    // === S3: 节点参数 ===
    void setParams(const QJsonObject& p) { m_params = p; }
    QJsonObject params() const { return m_params; }

    // === S2: itemChange + 节点移动通知 ===
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

    // === 每步结果查看 ===
    StepStatus stepStatus() const { return m_stepStatus; }
    void setStepStatus(StepStatus s) { m_stepStatus = s; update(); }
    // 从算子 output 推出该画什么状态点。
    // 抽成静态函数是为了能单独测 —— 这段判断错了不会崩,只会让
    // 一条全 FAIL 的流水线画出一排绿点,属于最难发现的那类错。
    static StepStatus statusFromOutput(const QJsonObject& out);
    // 有没有缓存过这一步的输出(决定右键菜单里"查看该步输出"能不能点)
    bool hasCachedResult() const { return m_hasCachedResult; }
    void setHasCachedResult(bool b) { m_hasCachedResult = b; }

    // === pin 类型(画布上显示,连错线时用户才知道为什么被拒) ===
    // 存的是中文名而不是 enum:画布只管画,不想为了显示一行字
    // 就去依赖 pin_types 的实现。类型转换在 PipelineView 里做完。
    void setPinLabels(const QString& inLabel, const QString& outLabel) {
        m_inLabel  = inLabel;
        m_outLabel = outLabel;
        setToolTip(inLabel.isEmpty() && outLabel.isEmpty()
                       ? displayName()
                       : tr("%1\n输入:%2   输出:%3")
                             .arg(displayName(),
                                  inLabel.isEmpty()  ? tr("无") : inLabel,
                                  outLabel.isEmpty() ? tr("无") : outLabel));
        update();
    }

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
    // === 每步结果查看:右键"查看该步输出" ===
    // 只发请求,数据在 MainWindow 的缓存里 —— 节点自己不持有 output
    // (output 可能很大,每个节点存一份会让画布白白吃掉几百 MB)
    void viewStepResultRequested(OperatorNode* node);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;  // === M6 ===
    void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;  // === P4 ===
    QRectF outputPortRect() const;
    QRectF inputPortRect() const;
    QRectF topPortRect() const;       // 顶部 Input
    QRectF bottomPortRect() const;    // 底部 Output

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

    QString m_inLabel;
    QString m_outLabel;

    // === M7: 鼠标按下时落在哪条边,拖出连接时携带在 MIME payload 里 ===
    PortSide m_dragSide = PortSide::Right;

    // === 每步结果查看 ===
    StepStatus m_stepStatus = StepStatus::None;
    bool       m_hasCachedResult = false;

    static constexpr int W = 140;
    static constexpr int H = 60;
    static constexpr int PORT_HALF = 8;
};

} // namespace w7

