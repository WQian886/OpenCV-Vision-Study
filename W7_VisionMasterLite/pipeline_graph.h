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
//   未知算子**留在图上并标 known=false**(不再丢弃)、
//   边端点**真的不存在**才跳过、旧格式按 op 名 FIFO 兜底、
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
#include <QStringList>
#include <QSet>
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
//
// ⚠ `known` 是 2026-10-25 加的(欠账 18 加载期 / 欠账 19):
//
//   以前这一层**直接丢弃**认不出来的算子节点。
//   丢掉一个节点,指向它的**边端点就没了** -> 边也跟着被跳过 ->
//   **整条链断成两截** -> 下游拿**空输出** -> 默认值兜底 ->
//   数字看着正常 -> 判 PASS。
//
//   也就是说:**存盘里少一个算子,伤的是它后面所有算子。**
//
//   现在**认不出来的节点也留在图上**,只是标 known=false ——
//   画布上看得见(画布忠实度),运行时那一步会失败(算子压根没注册),
//   于是这一帧是**坏帧**,整批判 ERROR 而不是静默 PASS。
//
//   链条**不断**,下游拿到的是**空输出**(因为上游那一步失败了),
//   而空输出 + 「那一步失败了」= 整批作废 —— 这正是我们要的。
struct GraphNode {
    QString op;                 // 算子名
    QString display;
    QString category;
    QString uuid;               // 文件里带的 uuid(可能为空)
    QJsonObject params;         // 节点参数
    double x = 100.0, y = 200.0;
    bool   known = true;        // false = 这个算子**没装/没注册**(但节点仍在图上)
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
//
// ⚠ 2026-10-25:三个计数字段**改名**了,原因和上面 GraphNode::known 一样 ——
//   名字改了,是因为**含义**变了,不是为了让名字好看:
//
//   | 旧名 | 新名 | 为什么 |
//   |---|---|---|
//   | skippedNodes | **unknownNodes** | 以前是"跳过了几个"(节点被**丢弃**);现在节点**留着**,数的是"有几个算子没装" |
//   | skippedEdges  | **brokenEdges**  | 以前混着两件事(指向未知算子的边 + 端点真不存在的边);现在只有后者才算"断" |
//   | defaultedSides | defaultedSides  | 名字没变,含义也没变(缺方向字段、按默认重画) |
//
//   ⚠ 旧名留着会**说谎** ——
//     `skippedNodes` 现在一个节点都不跳了,还叫这个名字就是骗人。
//   规则会搬家,但**不许改口**。
struct PipelineGraph {
    QVector<GraphNode> nodes;
    QVector<GraphEdge> edges;
    QString imagePath;          // 工具栏图路径
    QString name;               // 工程名
    int unknownNodes = 0;       // 算子没装/没注册的节点数(**节点仍在图上**)
    int brokenEdges = 0;        // 端点**真的不存在**的连线数
    int defaultedSides = 0;     // 缺方向字段、按默认重画的连线数
};

// ============================================================
// graphFromJson —— JSON -> 图
// ============================================================
//
// factoryFn: 算子名 -> 默认显示名/分类。返回 known=false 表示不认识。
//            ⚠ 2026-10-25:以前 known=false 的节点**被直接丢弃**,
//              现在它**留在图上**并标 known=false(见 GraphNode::known)。
// logFn:     过程中的日志(未知算子 / 端点缺失 / 方向字段缺失 / 汇总)。
//
// 返回 false = JSON 本身不能用(根不是对象 / 缺 nodes 数组)。
// ⚠ 这两种情况和"解析成功但有节点没装"是两回事:
//   前者整个文件不能用,后者是**部分**能用 —— 剩下的照跑,
//   没装的那部分**必须报出来**(不报就是静默失效),
//   而且那一步**必须失败**(不然整批会静默 PASS)。
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
    // opFromIndex / opToIndex: 旧版文件(无 uuid 字段)按 op 名 FIFO 兜底
    //
    // ⚠ uuidIndex 用 **QUuid** 当键,不是字符串:
    //   文件里的 uuid 可能有花括号、大小写也可能不同,
    //   按字符串比会漏掉 —— 而漏掉的代价是"这条边接不上"。
    //
    // ⚠⚠ 2026-10-25:旧格式的 FIFO 以前**只有一份队列**,from 和 to 共用。
    //   那是个**真 bug**,而且很隐蔽:
    //
    //     节点: src -> mid -> dst
    //     边 1: {from:"src", to:"mid"}
    //     边 2: {from:"mid", to:"dst"}
    //
    //   共用一份队列时:
    //     边 1 的 to="mid" 把队列里那个 mid **消耗掉**了
    //     -> 边 2 的 from="mid" 找不到人 -> **断**
    //
    //   也就是说:**旧格式里三节点以上的链必断**,
    //   而默认流水线(四节点三连线)正好就是这种形状。
    //   老的测试只用了两个节点,所以这个洞一直没被看见。
    //
    //   修法:from 和 to **各用各的队列** ——
    //   一个节点当然可以**既是**某条边的下游、**又是**另一条边的上游
    //   (那就是链)。
    QHash<QUuid, int> uuidIndex;
    QHash<QString, QList<int>> opFromIndex;
    QHash<QString, QList<int>> opToIndex;

    // 1. 按数组顺序重建节点
    //
    // ⚠ 认不出来的算子**也留在图上**(known=false)。
    //   以前这里是 `continue` —— 节点被丢掉,于是指向它的边端点缺失,
    //   边跟着被跳过,链断成两截,下游拿空输出兜底,数字看着正常,判 PASS。
    //   少一个算子,伤的是它**后面所有算子**。
    //
    //   留着的三个好处:
    //     1. 画布上看得见(用户存的什么,画布上就是什么)
    //     2. 指向它的边**接得上** -> 链不断
    //     3. 运行时那一步会失败(算子没注册) -> 整批 ERROR,而不是静默 PASS
    const QJsonArray nodesJson = root["nodes"].toArray();
    int created = 0, unknown = 0;
    for (const auto& v : nodesJson) {
        const QJsonObject nj = v.toObject();
        const QString opName = nj["op"].toString();
        const OpDefaults def = factoryFn ? factoryFn(opName) : OpDefaults();

        GraphNode gn;
        gn.op       = opName;
        gn.display  = nj.value("display").toString(def.display);
        gn.category = nj.value("category").toString(def.category);
        gn.x        = nj.value("x").toDouble(100);
        gn.y        = nj.value("y").toDouble(200);
        gn.params   = nj.value("params").toObject();
        gn.known    = def.known;

        // ⚠ 认不出来的算子,画布上**至少要显示它的名字** ——
        //   否则画布上出现一个空白方块,用户不知道少了什么。
        //   （认得出来的算子 display 一定有值,不受这条影响。）
        if (!def.known && gn.display.isEmpty())
            gn.display = opName;

        const int idx = out->nodes.size();
        out->nodes.append(gn);

        // uuid 存**规范化**过的(和 GUI 那边一致)。
        // 文件里没有 / 不合法的话留空 —— 由调用方决定怎么办
        // (GUI 会用自动生成的,headless 用下标兜底)。
        //
        // ⚠ 认不出来的算子**照样登记 uuid** ——
        //   不登记的话,指向它的边就接不上,又回到"链断成两截"。
        const QUuid id = QUuid::fromString(nj["uuid"].toString());
        if (!id.isNull()) {
            out->nodes[idx].uuid = id.toString(QUuid::WithoutBraces);
            uuidIndex.insert(id, idx);
        }
        opFromIndex[opName].append(idx);
        opToIndex[opName].append(idx);
        ++created;

        if (!def.known) {
            ++unknown;
            logFn(QString("[Load] 未知算子: %1(已保留在画布上,运行到这一步会失败)")
                      .arg(opName));
        }
    }
    out->unknownNodes = unknown;

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
            //
            // ⚠ from 和 to **各用各的队列** —— 理由见上面 opFromIndex 那段:
            //   共用一份的话,三节点以上的链会**在中间断开**。
            if (opFromIndex.contains(fromKey) && !opFromIndex[fromKey].isEmpty())
                fromIdx = opFromIndex[fromKey].takeFirst();
            if (opToIndex.contains(toKey) && !opToIndex[toKey].isEmpty())
                toIdx = opToIndex[toKey].takeFirst();
        }

        if (fromIdx < 0 || toIdx < 0) {
            // ⚠ 这里的"端点缺失"**只**指端点在文件里**压根不存在**
            //   (uuid 对不上 / op 名 FIFO 配完了都没配到)。
            //   指向一个「算子没装」的节点**不算缺失** ——
            //   那个节点在图上,边接得上(见上面"未知算子也留在图上")。
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
    out->brokenEdges   = edgeSkip;
    out->defaultedSides = sideDefaulted;

    // 老文件一个字段都没有才值得说一句;正常的新文件不该刷这个日志。
    if (sideDefaulted > 0) {
        logFn(QString("[Load] %1 条连线没有方向字段(旧版文件),已按 右出→左进 重画")
              .arg(sideDefaulted));
    }

    logFn(QString("[Load] 完成: 创建 %1 个节点(其中 %2 个算子没装),连线 %3 条(断掉 %4)")
        .arg(created).arg(unknown).arg(linked).arg(edgeSkip));
    return true;
}

// ============================================================
// missingOperatorNames —— 收拢「没装的算子名」
// ============================================================
//
// ⚠ 2026-10-25 第六笔(P0-2):
//   「算子没装」这条规则当时有**两份实现**
//   (headless 一份、GUI 一份,消息还不一样)。
//   现在收成一份(missing_operator.h),
//   这里只负责**把名字收拢出来** ——
//   纯层不能 new 算子,所以「装没装」这个事实是解析层给的。
//
// ⚠ 为什么按**算子名**收,而不是按节点:
//   `known` 本来就是**按算子名**判的(同一个算子装没装,跟节点无关),
//   所以两个节点用同一个算子,结论一定一样。
//   执行层的 runner 回调只拿到**算子名**,拿不到节点 ——
//   这正是这里要收成名字集合的原因。
inline QSet<QString> missingOperatorNames(const QVector<GraphNode>& nodes) {
    QSet<QString> names;
    for (const auto& n : nodes) {
        if (!n.known) names.insert(n.op);
    }
    return names;
}

// ============================================================
// describeLoadAnomalies —— 读盘有毛病时,讲清楚是什么毛病
// ============================================================
//
// ⚠ 2026-10-25:这段话以前住在 mainwindow.cpp 里。
//   于是「GUI 到底报没报」只能靠**源码守卫**看着 ——
//   而守卫查的是**文本**,它有两个已知的漏法:
//     · 会被一句注释喂饱(M154)
//     · 会被**硬编码**骗过 —— 本轮变异 M179 就是这么活的:
//         把 .arg(loadedGraph.unknownNodes) 换成 .arg(0),
//         守卫查「这个名字在不在」—— 名字还在,**值已经没了**。
//       画布上就会说「有 0 个算子没装」—— 一句**假话**。
//
//   现在它住在**纯层**,于是:
//     · 内容**能直接测**(喂一张图进去,看它说没说对)
//     · GUI 那一侧退化成「必须调 w7::describeLoadAnomalies」——
//       一个**可测入口**,不是一句文本。
//
//   ⚠ 这也是「能测的就搬出去,不能测的才留源码守卫」的又一例:
//     这段话**本来就能测**,只是以前没人搬。
//
// ⚠ 为什么它住在**解析层**而不是 GUI:
//   这段话说的就是**读盘**读出了什么毛病 ——
//   它和 PipelineGraph 是同一件事的两面:
//   一面是数,一面是人话。放在一起,数和人话**不会走散**。
inline QString describeLoadAnomalies(const PipelineGraph& g) {
    QStringList parts;
    if (g.unknownNodes > 0) {
        parts << QStringLiteral(
            "[Load] ⚠️ 有 %1 个算子没装(节点已保留在画布上,标着「未安装」)。"
            "装上这些算子才能跑通;不装的话,流水线跑到那一步会失败,整批判 ERROR。")
            .arg(g.unknownNodes);
    }
    if (g.brokenEdges > 0) {
        parts << QStringLiteral(
            "[Load] ⚠️ 有 %1 条连线的端点在文件里不存在,已断开。")
            .arg(g.brokenEdges);
    }
    if (g.defaultedSides > 0) {
        parts << QStringLiteral(
            "[Load] ⚠️ 有 %1 条连线在文件里没记方向,画布上按「右出→左进」画的。")
            .arg(g.defaultedSides);
    }
    return parts.join(QLatin1Char('\n'));
}

} // namespace w7
