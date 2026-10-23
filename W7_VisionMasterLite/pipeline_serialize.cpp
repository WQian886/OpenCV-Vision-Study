#include "pipeline_serialize.h"

#include "pipeline_view.h"
#include "connection_line.h"
// 刻意**不** include operator_base.h —— 理由见 pipeline_graph.h 里 OpDefaults 那段。

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QUuid>

namespace w7 {

// ============================================================
// 端口方向 ↔ 整数
// ============================================================
// 存 0..3,顺序和 OperatorNode::PortSide 的声明顺序一致:
//   0=Left  1=Right  2=Top  3=Bottom
// 用整数而不是字符串:文件小一点,而且手改的时候不容易写出"top "带空格这种
// 静默失配(字符串比较不匹配 → 退回默认 → 你还以为方向存对了)。
static inline int sideToInt(OperatorNode::PortSide s) {
    switch (s) {
        case OperatorNode::PortSide::Left:   return 0;
        case OperatorNode::PortSide::Right:  return 1;
        case OperatorNode::PortSide::Top:    return 2;
        case OperatorNode::PortSide::Bottom: return 3;
    }
    return 1;   // 理论上到不了;真到了也不能让编译告警
}

// ============================================================
// 存盘
// ============================================================
QJsonDocument pipelineToJson(PipelineView* canvas, const QString& imagePath) {
    QJsonObject root;
    root["version"]    = "1.1";
    root["name"]       = "untitled";
    root["image_path"] = imagePath;

    // 序列化所有节点
    QJsonArray nodesJson;
    for (auto* n : canvas->nodes()) {
        QJsonObject nj;
        nj["op"]       = n->opName();
        nj["display"]  = n->displayName();
        nj["category"] = n->category();
        nj["x"]        = n->pos().x();
        nj["y"]        = n->pos().y();
        nj["params"]   = n->params();
        nj["uuid"]     = n->uuid().toString(QUuid::WithoutBraces);  // === M6 ===
        nodesJson.append(nj);
    }
    root["nodes"] = nodesJson;

    // === M6: 边按 UUID 引用 ===
    // 旧版按 op 名 + FIFO 匹配,两个同名 color_segment 会连错 ——
    // UUID 是唯一身份,同名节点也能精确指认。
    //
    // === v1.1: 连线的两端方向也存下来 ===
    // 以前只记"谁连谁",加载时一律按默认的 Right→Left 重画。
    // 于是用户从节点**下边**拉出去、从上一个节点**上边**接进来的
    // 竖线,存一次再打开就变成一条横线了 —— 而且没有任何报错。
    // 这两个字段就是补上那半个信息。
    QJsonArray edgesJson;
    for (auto* c : canvas->connections()) {
        if (!c->fromNode() || !c->toNode()) continue;
        QJsonObject ej;
        ej["from"]      = c->fromNode()->uuid().toString(QUuid::WithoutBraces);
        ej["to"]        = c->toNode()->uuid().toString(QUuid::WithoutBraces);
        ej["from_side"] = sideToInt(c->fromSide());
        ej["to_side"]   = sideToInt(c->toSide());
        edgesJson.append(ej);
    }
    root["edges"] = edgesJson;

    return QJsonDocument(root);
}

// ============================================================
// 读盘
// ============================================================
//
// ⚠ 这一层只做「图 -> 画布」。
//   **解析规则不在这里** —— 在 pipeline_graph.h 的 graphFromJson 里。
//   headless runner 用的是同一份规则,所以两边不会漂。
//
//   为什么解析要拆出去:
//     · 解析是纯数据(QtCore),测它不用拖 QGraphicsScene;
//     · headless runner 根本不想要画布;
//     · 规则只有一处 —— 抄第二份就是抄副本。
bool pipelineFromJson(PipelineView* canvas, const QJsonDocument& doc,
                      std::function<void(const QString&)> logFn,
                      std::function<OpDefaults(const QString&)> factoryFn,
                      QString& errOut,
                      PipelineGraph* outGraph) {
    PipelineGraph g;
    if (!graphFromJson(doc, logFn, factoryFn, &g, errOut)) return false;
    if (outGraph) *outGraph = g;

    canvas->clearPipeline();

    // === 图 -> 画布 ===
    //
    // uuidIndex: 按 UUID 找回节点(同名的两个节点也不歧义)
    //
    // byIndex: 图里的下标 -> 画布上的节点。
    // ⚠ 图里下标是**解析层**定的(它按 uuid 或 op 名 FIFO 对过两端),
    //   这一层只负责把下标翻译成节点指针 ——
    //   对端这件事**只做一次**,别在这儿再做一遍。
    QVector<OperatorNode*> byIndex;
    byIndex.reserve(g.nodes.size());
    for (const GraphNode& gn : g.nodes) {
        OperatorNode* n = canvas->addNode(gn.op, gn.display, gn.category, {gn.x, gn.y});
        if (!gn.params.isEmpty()) n->setParams(gn.params);
        const QUuid id = QUuid::fromString(gn.uuid);
        if (!id.isNull()) n->setUuid(id);   // 文件里没有 uuid 就保留自动生成的
        byIndex.append(n);
    }

    for (const GraphEdge& ge : g.edges) {
        if (ge.fromIndex < 0 || ge.toIndex < 0) continue;   // 端点缺失(解析层已报过)
        if (ge.fromIndex >= byIndex.size() || ge.toIndex >= byIndex.size()) continue;
        OperatorNode* fromN = byIndex.at(ge.fromIndex);
        OperatorNode* toN   = byIndex.at(ge.toIndex);
        if (!fromN || !toN) continue;   // 端点缺失 —— 解析层已经报过了

        // === v1.1: 读回方向 ===
        // 老文件(没有这两个字段)走 fallback,行为和 v1.0 完全一致 ——
        // 这就是"不用升版本号也不用迁移"的原因。
        // 越界值(手改坏的文件)也走 fallback:一条线的方向画错了还能改,
        // 整条线加载失败没法用。
        const OperatorNode::PortSide fromSide =
            parseSide(ge.fromSide, OperatorNode::PortSide::Right);
        const OperatorNode::PortSide toSide =
            parseSide(ge.toSide,   OperatorNode::PortSide::Left);

        canvas->addConnection(fromN, toN, fromSide, toSide);
    }

    return true;
}

} // namespace w7
