#pragma once
// ============================================================
// 流水线 JSON -> 图(纯解析层)
// ============================================================
//
// ⚠ 为什么要有这个文件:
//   「读盘」的逻辑以前住在 pipeline_serialize.cpp 里,函数签名是
//   `pipelineFromJson(PipelineView*, ...)` —— 它**要一个画布**。
//
//   于是想跑一条流水线但不要窗口的人(headless runner)只有两条路:
//     a) 造一个离屏画布,喂给它,再从画布上把节点抠出来
//     b) 自己再写一份解析
//
//   (a) 把整个 QGraphicsScene 拖进 headless 路径;
//   (b) 就是**抄副本** —— 本项目栽过五遍的坑,而且这一次是"抄逻辑"。
//
//   所以这里把**解析**从**画图**里拆出来:
//     · 这一层:JSON -> 图(纯数据,QtCore,零 OpenCV 零 QtWidgets)
//     · pipeline_serialize.cpp:图 -> 画布上的节点和连线(QtWidgets)
//
//   两边共用**同一份**解析 —— 规则只有一处:
//   未知算子跳过、边端点缺失跳过、旧格式按 op 名 FIFO 兜底、
//   方向字段缺失时按默认重画。
//
// ============================================================
// 依赖面
// ============================================================
// 只依赖 QtCore(QJsonDocument / QJsonObject / QVector / QString / QUuid)。
// **零 OpenCV、零 QtWidgets** —— 所以依赖它的测试目标建得快,
// 而且不需要显示器。
//
// ⚠ 端口方向在这一层存成**整数**(0=左 1=右 2=上 3=下),
//   不存 OperatorNode::PortSide —— 那个枚举住在 operator_node.h 里,
//   而 operator_node.h 拖 QGraphicsItem,会把这层拖进 QtWidgets。
//   转成枚举是 GUI 层的事(见 pipeline_serialize.h 的 parseSide)。
//
// ============================================================
// 格式版本
// ============================================================
// v1.0  节点(op/display/category/x/y/params/uuid) + 边(from/to)
// v1.1  边多了 from_side / to_side —— 见下面"为什么要记方向"
//
// ============================================================

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QHash>
#include <QVector>
#include <QString>
#include <functional>

namespace w7 {

// --- 算子的显示信息 ---
//
// 这里**故意不返回 OperatorBase***。
// 解析层真正需要的只有两件事:这个算子名认不认识、它的显示名和分类是什么。
// 早先的签名是 `OperatorBase*(name)` —— 于是读一个文件要先 new 一个算子,
// 用完立刻 delete,只为了取两个字符串。而 OperatorBase 的头文件里
// `#include <opencv2/opencv.hpp>`,于是解析层也跟着依赖 OpenCV。
//
// 改成 POD 之后:解析层不认识算子(它只认名字和标签),
// 测试目标保持"零 OpenCV"这个属性,调用方那点构造逻辑收到上层。
struct OpDefaults {
    bool    known    = false;   // 认不认识这个算子名
    QString display;            // 认得的话,给什么显示名
    QString category;
};

// --- 图里的一个节点 ---
struct GraphNode {
    QString op;                 // 算子名
    QString display;
    QString category;
    QString uuid;               // 文件里带的 uuid(可能为空)
    QJsonObject params;         // 节点参数
    double x = 100.0, y = 200.0;
};

// --- 图里的一条边 ---
//
// ⚠ 两端用**下标**指认(在 GraphNode 数组里的位置),不是字符串:
//   解析层已经按 uuid(新版)或 op 名 FIFO(旧版)把两端对上了,
//   留着字符串只会让下一个人再做一次同样的匹配。
//
// fromKey / toKey 只是**原始的键**,留着是为了出错时能说清是哪条线。
//
// ⚠ 端口方向存的是**文件里的原值**(QJsonValue),不在这一层换算:
//   换算成 OperatorNode::PortSide 要拖 QtWidgets(operator_node.h),
//   而 headless 那边根本不关心方向。哪个方向算"上"是**画布**的事。
//   谁要用,自己拿 parseSide() 换(见 pipeline_serialize.h)。
struct GraphEdge {
    int fromIndex = -1;         // 上游节点在 GraphNode 数组里的下标
    int toIndex   = -1;         // 下游节点
    QJsonValue fromSide;        // 文件里的原值(可能是 Undefined)
    QJsonValue toSide;
    bool sidesGiven = false;    // 两个方向字段**都在**(少一个就算缺)
    QString fromKey, toKey;     // 原始的键(报错用)
};

// --- 整张图 ---
struct PipelineGraph {
    QVector<GraphNode> nodes;
    QVector<GraphEdge> edges;
    QString imagePath;          // 工具栏图路径
    QString name;               // 工程名
    int skippedNodes = 0;       // 因算子不认识而跳过的节点数
    int skippedEdges = 0;       // 因端点缺失而跳过的连线数
    int defaultedSides = 0;     // 缺方向字段、按默认重画的连线数
};

// ============================================================
// graphFromJson —— JSON -> 图
// ============================================================
//
// factoryFn: 算子名 -> 默认显示名/分类。返回 known=false 表示不认识,
//            该节点会被跳过并记一条日志。
// logFn:     过程中的日志(未知算子 / 端点缺失 / 方向字段缺失 / 汇总)。
//
// 返回 false = JSON 本身不能用(根不是对象 / 缺 nodes 数组)。
// ⚠ 这两种情况和"解析成功但有节点被跳过"是两回事:
//   前者整个文件不能用,后者是**部分**能用 —— 剩下的照跑,
//   被跳过的那部分**必须报出来**(不报就是静默失效)。
inline bool graphFromJson(const QJsonDocument& doc,
                          const std::function<void(const QString&)>& logFn,
                          const std::function<OpDefaults(const QString&)>& factoryFn,
                          PipelineGraph* out,
                          QString& errOut) {
    if (!out) { errOut = "输出参数为空"; return false; }
    if (!doc.isObject()) { errOut = "JSON 根不是对象"; return false; }
    const QJsonObject root = doc.object();
    if (!root.contains("nodes") || !root["nodes"].isArray()) {
        errOut = "缺少 nodes 数组";
        return false;
    }

    out->imagePath = root.value("image_path").toString();
    out->name      = root.value("name").toString();

    // === 两种索引 ===
    // uuidIndex: 新版文件按 UUID 精确指认(同名节点也不歧义)
    // opIndex:   旧版文件(无 uuid 字段)按 op 名 FIFO 兜底
    //
    // ⚠ uuidIndex 用 **QUuid** 当键,不是字符串:
    //   文件里的 uuid 可能有花括号、大小写也可能不同,
    //   按字符串比会漏掉 —— 而漏掉的代价是"这条边接不上"。
    QHash<QUuid, int> uuidIndex;
    QHash<QString, QList<int>> opIndex;

    // 1. 按数组顺序重建节点
    const QJsonArray nodesJson = root["nodes"].toArray();
    int created = 0, skipped = 0;
    for (const auto& v : nodesJson) {
        const QJsonObject nj = v.toObject();
        const QString opName = nj["op"].toString();
        const OpDefaults def = factoryFn ? factoryFn(opName) : OpDefaults();
        if (!def.known) {
            logFn(QString("[Load] 跳过未知算子: %1").arg(opName));
            ++skipped;
            continue;
        }
        GraphNode gn;
        gn.op       = opName;
        gn.display  = nj.value("display").toString(def.display);
        gn.category = nj.value("category").toString(def.category);
        gn.x        = nj.value("x").toDouble(100);
        gn.y        = nj.value("y").toDouble(200);
        gn.params   = nj.value("params").toObject();

        const int idx = out->nodes.size();
        out->nodes.append(gn);

        // uuid 存**规范化**过的(和 GUI 那边一致)。
        // 文件里没有 / 不合法的话留空 —— 由调用方决定怎么办
        // (GUI 会用自动生成的,headless 用下标兜底)。
        const QUuid id = QUuid::fromString(nj["uuid"].toString());
        if (!id.isNull()) {
            out->nodes[idx].uuid = id.toString(QUuid::WithoutBraces);
            uuidIndex.insert(id, idx);
        }
        opIndex[opName].append(idx);
        ++created;
    }
    out->skippedNodes = skipped;

    // 2. 重建连线:两端都是合法 UUID 就按 UUID 精确指认,
    //    否则退回旧格式(op 名 + FIFO)。
    //
    // ⚠ "是不是合法 UUID"是**格式**判据,不是"在不在节点表里":
    //   一个文件如果整体是 UUID 格式,但某条边指向一个不存在的节点,
    //   那它就是一条坏边(报出来、跳过)—— 不是"旧格式"。
    //   拿"在不在表里"当判据,会把两种情况搅在一起。
    const QJsonArray edgesJson = root.value("edges").toArray();
    int linked = 0, edgeSkip = 0, sideDefaulted = 0;
    for (const auto& v : edgesJson) {
        const QJsonObject ej = v.toObject();
        const QString fromKey = ej["from"].toString();
        const QString toKey   = ej["to"].toString();

        int fromIdx = -1, toIdx = -1;

        const QUuid fromId = QUuid::fromString(fromKey);
        const QUuid toId   = QUuid::fromString(toKey);
        if (!fromId.isNull() && !toId.isNull()) {
            // 新版:按 UUID 精确指认
            fromIdx = uuidIndex.value(fromId, -1);
            toIdx   = uuidIndex.value(toId, -1);
        } else {
            // 旧格式:from/to 存的是 op 名,按 FIFO 兜底
            // (同名节点一个一个配对,先连线的用第一个)
            if (opIndex.contains(fromKey) && !opIndex[fromKey].isEmpty())
                fromIdx = opIndex[fromKey].takeFirst();
            if (opIndex.contains(toKey) && !opIndex[toKey].isEmpty())
                toIdx = opIndex[toKey].takeFirst();
        }

        if (fromIdx < 0 || toIdx < 0) {
            logFn(QString("[Load] 边的端点缺失: %1 → %2").arg(fromKey, toKey));
            ++edgeSkip;
            continue;
        }

        GraphEdge ge;
        ge.fromIndex = fromIdx;
        ge.toIndex   = toIdx;
        ge.fromKey   = fromKey;
        ge.toKey     = toKey;
        // 方向:老文件(没有这两个字段)走默认,行为和 v1.0 完全一致 ——
        // 这就是"不用升版本号也不用迁移"的原因。
        //
        // ⚠ "缺字段"的判据是**两个都在**才算有:
        //   少一个就算缺(和以前一样)。而且"缺"和"值是 0"是两回事 ——
        //   0=Left 是个合法值,所以判据不能看 toInt() 是不是 0。
        //   真正的换算交给 parseSide(),这一层只负责说清"有没有"。
        ge.fromSide = ej.value("from_side");
        ge.toSide   = ej.value("to_side");
        ge.sidesGiven = ej.contains("from_side") && ej.contains("to_side");
        if (!ge.sidesGiven) ++sideDefaulted;

        out->edges.append(ge);
        ++linked;
    }
    out->skippedEdges  = edgeSkip;
    out->defaultedSides = sideDefaulted;

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
