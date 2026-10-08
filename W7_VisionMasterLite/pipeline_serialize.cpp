#include "pipeline_serialize.h"

#include "pipeline_view.h"
#include "connection_line.h"
// 刻意**不** include operator_base.h —— 理由见头文件里 OpDefaults 那段。

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>

namespace w7 {

// ============================================================
// 端口方向 ↔ 整数
// ============================================================
// 存 0..3,顺序和 OperatorNode::PortSide 的声明顺序一致:
//   0=Left  1=Right  2=Top  3=Bottom
// 用整数而不是字符串:文件小一点,而且手改的时候不容易写出"top "带空格这种
// 静默失配(字符串比较不匹配 → 退回默认 → 你还以为方向存对了)。
// ============================================================
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
bool pipelineFromJson(PipelineView* canvas, const QJsonDocument& doc,
                      std::function<void(const QString&)> logFn,
                      std::function<OpDefaults(const QString&)> factoryFn,
                      QString& errOut) {
    if (!doc.isObject()) { errOut = "JSON 根不是对象"; return false; }
    QJsonObject root = doc.object();
    if (!root.contains("nodes") || !root["nodes"].isArray()) {
        errOut = "缺少 nodes 数组";
        return false;
    }

    canvas->clearPipeline();

    // === M6: 两种索引 ===
    // uuidIndex: 新版文件按 UUID 精确指认(同名节点也不歧义)
    // opIndex:   旧版文件(无 uuid 字段)按 op 名 FIFO 兜底
    QHash<QUuid, OperatorNode*> uuidIndex;
    QHash<QString, QList<OperatorNode*>> opIndex;

    // 1. 按数组顺序重建节点
    QJsonArray nodesJson = root["nodes"].toArray();
    int created = 0, skipped = 0;
    for (const auto& v : nodesJson) {
        QJsonObject nj = v.toObject();
        QString opName = nj["op"].toString();
        const OpDefaults def = factoryFn(opName);
        if (!def.known) {
            logFn(QString("[Load] 跳过未知算子: %1").arg(opName));
            ++skipped;
            continue;
        }
        // 用 JSON 里的 display/category（没有则用算子默认）
        QString display  = nj.value("display").toString(def.display);
        QString category = nj.value("category").toString(def.category);
        double x = nj.value("x").toDouble(100);
        double y = nj.value("y").toDouble(200);
        OperatorNode* node = canvas->addNode(opName, display, category, {x, y});
        // 恢复参数
        QJsonObject params = nj.value("params").toObject();
        if (!params.isEmpty()) node->setParams(params);
        // 恢复 UUID(旧文件没有则保留自动生成的)
        const QString uuidStr = nj["uuid"].toString();
        if (!uuidStr.isEmpty()) {
            const QUuid id = QUuid::fromString(uuidStr);
            if (!id.isNull()) {
                node->setUuid(id);
                uuidIndex.insert(id, node);
            }
        }
        opIndex[opName].append(node);
        ++created;
    }

    // 2. 重建连线:有 UUID 用 UUID,没有则退回 op 名 FIFO
    QJsonArray edgesJson = root.value("edges").toArray();
    int linked = 0, edgeSkip = 0, sideDefaulted = 0;
    for (const auto& v : edgesJson) {
        QJsonObject ej = v.toObject();
        QString fromKey = ej["from"].toString();
        QString toKey   = ej["to"].toString();

        const QUuid fromId = QUuid::fromString(fromKey);
        const QUuid toId   = QUuid::fromString(toKey);
        const bool useUuid = !fromId.isNull() && !toId.isNull();

        OperatorNode* fromN = nullptr;
        OperatorNode* toN   = nullptr;

        if (useUuid) {
            fromN = uuidIndex.value(fromId, nullptr);
            toN   = uuidIndex.value(toId, nullptr);
        } else {
            // 旧格式:from/to 存的是 op 名
            if (opIndex.contains(fromKey) && !opIndex[fromKey].isEmpty())
                fromN = opIndex[fromKey].takeFirst();
            if (opIndex.contains(toKey) && !opIndex[toKey].isEmpty())
                toN = opIndex[toKey].takeFirst();
        }

        if (!fromN || !toN) {
            logFn(QString("[Load] 边的端点缺失: %1 → %2").arg(fromKey, toKey));
            ++edgeSkip;
            continue;
        }

        // === v1.1: 读回方向 ===
        // 老文件(没有这两个字段)走 fallback,行为和 v1.0 完全一致 ——
        // 这就是"不用升版本号也不用迁移"的原因。
        // 越界值(手改坏的文件)也走 fallback:一条线的方向画错了还能改,
        // 整条线加载失败没法用。
        const OperatorNode::PortSide fromSide =
            parseSide(ej.value("from_side"), OperatorNode::PortSide::Right);
        const OperatorNode::PortSide toSide =
            parseSide(ej.value("to_side"),   OperatorNode::PortSide::Left);

        if (!ej.contains("from_side") || !ej.contains("to_side"))
            ++sideDefaulted;

        canvas->addConnection(fromN, toN, fromSide, toSide);
        ++linked;
    }

    // 老文件一个字段都没有才值得说一句;正常的新文件不该刷这个日志。
    if (sideDefaulted > 0) {
        logFn(QString("[Load] %1 条连线没有方向字段(旧版文件),已按 右出→左进 重画")
              .arg(sideDefaulted));
    }

    logFn(QString("[Load] 完成: 创建 %1 个节点(跳过 %2),连线 %3 条(跳过 %4)")
        .arg(created).arg(skipped).arg(linked).arg(edgeSkip));
    return true;
}

} // namespace w7