// ============================================================
// W7 解析层(pipeline_graph.h)的回归测试
// ============================================================
//
// 测的是哪件事:
//
//   读一个工程文件时,**认不出来的算子**该怎么办。
//
// 它以前的行为是**直接丢弃**:
//
//     // pipeline_graph.h(旧版)
//     if (!def.known) {
//         logFn(QString("[Load] 跳过未知算子: %1").arg(opName));
//         ++skipped;
//         continue;          // ← 节点被丢掉
//     }
//
// 而丢掉一个节点会**连锁**:
//
//     未知算子被跳过
//       -> 指向它的**边端点缺失**
//       -> **边也被跳过**
//       -> **链断成两截**
//       -> 下游拿**空输出**
//       -> 默认值兜底
//       -> 数字正常
//       -> 判 PASS
//
// 也就是说:**存盘里少一个算子,伤的是它后面所有算子。**
//
// 而这一切**不崩、不报错、两边都是绿的** ——
// 两边(日志里那条"[Load] 跳过未知算子")都说了,但**说的只是日志**:
// 画布上那个节点**压根不在**,连线也**压根不在**,
// 用户看到的是一条**少了一半**的流水线,跑出来**判 PASS**。
//
// 所以现在(2026-10-25,欠账 18 加载期 + 欠账 19):
//
//   1. 认不出来的节点**留在图上**,标 known=false
//   2. 它的 uuid / op 名**照样登记** -> 指向它的边**接得上** -> 链不断
//   3. 画布上标成「未安装」(灰底 + 虚线框),用户**一眼**看得出少了什么
//   4. 运行时那一步会失败(算子没注册) -> 整批判 ERROR,不是静默 PASS
//
// 这就是「**画布忠实度**」:用户存的什么,画布上就是什么。
//
// ============================================================

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "pipeline_graph.h"   // OpDefaults / GraphNode / PipelineGraph / graphFromJson
#include "source_scan.h"      // 剥注释 / 读源码(共享的测试工具)

using namespace w7;
using namespace w7test;

// ============================================================
// 极简断言(与其余套件同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}
static void check(bool cond, const QString& what) {
    if (cond) { ++g_pass; qInfo().noquote() << "  [PASS]" << what; }
    else { ++g_fail; qWarning().noquote() << "  [FAIL]" << what
           << "  (case:" << g_case << ")"; }
}

// ============================================================
// 假算子工厂:只认这几个名字,别的当"没装"
// ============================================================
static OpDefaults fakeFactory(const QString& name) {
    if (name == "src" || name == "mid" || name == "dst" || name == "judge")
        return {true, name, QStringLiteral("test")};
    return {};   // 不认识 -> known = false
}

// ------------------------------------------------------------
// 造文件的零件
// ------------------------------------------------------------
//
// ⚠⚠ uuid **必须是真 UUID** —— 这不是讲究,是个陷阱:
//
//   解析层判断「这条边是不是新版格式」靠的是 QUuid::fromString();
//   随便写个 "u-src" 解析不出 UUID,于是那条边被当成**旧格式**
//   (两端存算子名)去按 FIFO 兜底 ——
//   于是测试护的成了一个**磁盘上根本不存在的格式**。
//
//   那种测试是**绿的**,但它什么也没护住:
//   真实文件里的边接不接得上,和它一点关系都没有。
//   而且它会**骗人**:测试里「边接上了」,可真实文件根本不是这么写的。
//
//   (这个坑在 test_headless_runner.cpp 的注释里也记着一次。)
//
//   所以这里用 MD5 派生出**合法**的 UUID,和 GUI 存盘写出来的形状一样。
static QString uuidOf(const QString& label) {
    const QByteArray h = QCryptographicHash::hash(label.toUtf8(),
                                                    QCryptographicHash::Md5).toHex();
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(QString(h.mid(0, 8)), QString(h.mid(8, 4)), QString(h.mid(12, 4)),
             QString(h.mid(16, 4)), QString(h.mid(20, 12)));
}

// 新版节点:带 uuid
static QJsonObject mkNode(const QString& op,
                          const QString& uuidLabel = QString(),
                          const QString& display = QString()) {
    QJsonObject n;
    n["op"] = op;
    if (!uuidLabel.isEmpty()) n["uuid"] = uuidOf(uuidLabel);
    if (!display.isEmpty())   n["display"] = display;
    return n;
}

// 旧版节点:**没有** uuid 字段(老文件长这样)
static QJsonObject mkLegacyNode(const QString& op) {
    QJsonObject n;
    n["op"] = op;
    return n;
}

// 新版边:两端是**真 UUID**
static QJsonObject mkEdge(const QString& fromLabel, const QString& toLabel) {
    QJsonObject e;
    e["from"] = uuidOf(fromLabel);
    e["to"]   = uuidOf(toLabel);
    return e;
}

// 旧版边:两端是**算子名**(老文件长这样)
static QJsonObject mkLegacyEdge(const QString& fromOp, const QString& toOp) {
    QJsonObject e;
    e["from"] = fromOp;
    e["to"]   = toOp;
    return e;
}

static QJsonDocument mkDoc(const QJsonArray& nodes, const QJsonArray& edges) {
    QJsonObject root;
    root["nodes"] = nodes;
    root["edges"] = edges;
    return QJsonDocument(root);
}

// 解析一次,顺便把日志收下来
struct Parsed {
    bool ok = false;
    PipelineGraph graph;
    QString err;
    QStringList logs;
};

static Parsed parse(const QJsonDocument& doc) {
    Parsed p;
    p.ok = graphFromJson(doc,
                          [&p](const QString& s) { p.logs.append(s); },
                          fakeFactory, &p.graph, p.err);
    return p;
}

// ============================================================
// 用例 1:未知算子**留在图上**,链**不断**
// ============================================================
//
// 这条是本轮的核心。以前这条链在读盘之后就断了,
// 而整批最后判 PASS。
static void test_unknown_node_is_kept_and_chain_intact() {
    beginCase("1. 未知算子留在图上,指向它的边接得上(链不断)");

    // src -> ghost -> judge,ghost 是没装的算子
    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-src"),
                    mkNode("ghost", "u-ghost"),
                    mkNode("judge", "u-judge") },
        QJsonArray{ mkEdge("u-src", "u-ghost"),
                    mkEdge("u-ghost", "u-judge") });

    const Parsed p = parse(doc);

    check(p.ok, "读盘成功");
    check(p.graph.nodes.size() == 3,
          QString("三个节点**都在**(实际 %1,以前是 2 —— 那个被丢掉了)")
              .arg(p.graph.nodes.size()));
    check(p.graph.edges.size() == 2,
          QString("两条边**都在**(实际 %1,以前是 0 —— 链断成两截)")
              .arg(p.graph.edges.size()));
    check(p.graph.brokenEdges == 0,
          QString("没有断掉的边(实际 %1)").arg(p.graph.brokenEdges));
    check(p.graph.unknownNodes == 1,
          QString("一个算子没装(实际 %1)").arg(p.graph.unknownNodes));

    // 哪个节点是那个没装的?
    int ghostIdx = -1;
    for (int i = 0; i < p.graph.nodes.size(); ++i)
        if (p.graph.nodes.at(i).op == "ghost") ghostIdx = i;
    check(ghostIdx >= 0, "找得到那个没装的节点");
    if (ghostIdx >= 0) {
        check(!p.graph.nodes.at(ghostIdx).known,
              "它被标成 known=false(不是「装作没事」)");
    }
    check(p.graph.nodes.size() > 0 && p.graph.nodes.at(0).known,
          "认识的算子还是 known=true(没被误伤)");

    // 边真的接在**那个节点**上(不是接在别的节点上凑数)
    if (ghostIdx >= 0 && p.graph.edges.size() == 2) {
        check(p.graph.edges.at(0).fromIndex == 0
              && p.graph.edges.at(0).toIndex == ghostIdx,
              "第一条边:src -> 那个没装的节点");
        check(p.graph.edges.at(1).fromIndex == ghostIdx
              && p.graph.edges.at(1).toIndex == 2,
              "第二条边:那个没装的节点 -> judge(**链没断**)");
    }

    // 必须**报出来** —— 静默失效比崩溃更难查
    bool reported = false;
    for (const QString& l : p.logs)
        if (l.contains(QStringLiteral("ghost"))) reported = true;
    check(reported, "那个算子的名字在日志里(不是静默丢弃)");
}

// ============================================================
// 用例 2:端点**真的**不存在 -> 仍然跳过,而且要报出来
// ============================================================
//
// ⚠ 这条和用例 1 是**两回事**,不能混:
//   「算子没装」-> 端点**在**,只是跑不了 -> 边留着
//   「端点压根不在文件里」-> 边接不上 -> 边跳过
// 以前这两种混在一个 skippedEdges 里,分不清。
static void test_truly_missing_endpoint_is_still_broken() {
    beginCase("2. 端点真的不存在:边还是断,但要报出来");

    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
        QJsonArray{ mkEdge("u-src", "u-judge"),
                    mkEdge("u-src", "u-压根不存在") });

    const Parsed p = parse(doc);

    check(p.ok, "读盘成功");
    check(p.graph.edges.size() == 1,
          QString("好的一条边留着(实际 %1)").arg(p.graph.edges.size()));
    check(p.graph.brokenEdges == 1,
          QString("断了一条(实际 %1)").arg(p.graph.brokenEdges));
    check(p.graph.unknownNodes == 0,
          QString("没有算子没装(实际 %1 —— 这条边的问题不是「算子没装」)")
              .arg(p.graph.unknownNodes));

    bool reported = false;
    for (const QString& l : p.logs)
        if (l.contains(QStringLiteral("端点缺失"))) reported = true;
    check(reported, "断掉的边在日志里说了(不是悄悄咽下去)");
}

// ============================================================
// 用例 3:旧格式(按 op 名 FIFO)也得算上没装的算子
// ============================================================
//
// 老文件里 from/to 存的是**算子名**,没有 uuid。
// 配对走 FIFO。
//
// ⚠ 以前没装的算子被丢掉了,于是 FIFO 配对时它**不在名单里**,
//   后面的同名节点被错配 —— 或者干脆配不上,边全断。
static void test_legacy_opname_pairing_counts_unknown_ops() {
    beginCase("3. 旧格式(op 名 FIFO):没装的算子也参与配对");

    // 没有 uuid 字段 -> 旧格式
    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkLegacyNode("src"), mkLegacyNode("ghost"), mkLegacyNode("judge") },
        QJsonArray{ mkLegacyEdge("src", "ghost"), mkLegacyEdge("ghost", "judge") });

    const Parsed p = parse(doc);

    check(p.ok, "读盘成功");
    check(p.graph.nodes.size() == 3,
          QString("三个节点都在(实际 %1)").arg(p.graph.nodes.size()));
    check(p.graph.edges.size() == 2,
          QString("两条边都配上了(实际 %1)").arg(p.graph.edges.size()));
    check(p.graph.brokenEdges == 0,
          QString("没有断边(实际 %1)").arg(p.graph.brokenEdges));

    if (p.graph.edges.size() == 2) {
        check(p.graph.edges.at(0).fromIndex == 0 && p.graph.edges.at(0).toIndex == 1,
              "第一条边配到 ghost(不是别的节点)");
        check(p.graph.edges.at(1).fromIndex == 1 && p.graph.edges.at(1).toIndex == 2,
              "第二条边从 ghost 出发");
    }
}

// ============================================================
// 用例 4:没装的算子**画布上得有名字**
// ============================================================
//
// 一个**空白方块**在用户眼里和别的节点没区别 ——
// 他不知道少了什么,也不知道少了的是哪个。
static void test_unknown_node_has_a_name_on_canvas() {
    beginCase("4. 没装的算子:画布上至少要显示它的名字");

    // 文件里**没有** display 字段
    {
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("ghost", "u-ghost") }, QJsonArray{});
        const Parsed p = parse(doc);
        check(p.graph.nodes.size() == 1, "一个节点");
        if (p.graph.nodes.size() == 1) {
            check(p.graph.nodes.at(0).display == "ghost",
                  QString("显示名兜底成算子名(实际 \"%1\")")
                      .arg(p.graph.nodes.at(0).display));
        }
    }

    // 文件里**有** display -> 用文件里的(用户当年看到的就是它)
    {
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("ghost", "u-ghost", "我当年叫它啥") },
            QJsonArray{});
        const Parsed p = parse(doc);
        check(p.graph.nodes.size() == 1, "一个节点");
        if (p.graph.nodes.size() == 1) {
            check(p.graph.nodes.at(0).display == "我当年叫它啥",
                  QString("显示名用文件里那个(实际 \"%1\")")
                      .arg(p.graph.nodes.at(0).display));
        }
    }
}

// ============================================================
// 用例 5:认识的算子**一点没变**
// ============================================================
//
// ⚠ 改规则最容易出的事:把好的也一起弄坏了。
//   所以必须有一条「什么都没变」的对照。
static void test_known_operators_are_untouched() {
    beginCase("5. 认识的算子:行为一点没变");

    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
        QJsonArray{ mkEdge("u-src", "u-judge") });

    const Parsed p = parse(doc);

    check(p.ok, "读盘成功");
    check(p.graph.nodes.size() == 2,
          QString("两个节点(实际 %1)").arg(p.graph.nodes.size()));
    check(p.graph.edges.size() == 1,
          QString("一条边(实际 %1)").arg(p.graph.edges.size()));
    check(p.graph.unknownNodes == 0,
          QString("没有算子没装(实际 %1)").arg(p.graph.unknownNodes));
    check(p.graph.brokenEdges == 0,
          QString("没有断边(实际 %1)").arg(p.graph.brokenEdges));
    if (p.graph.nodes.size() == 2) {
        check(p.graph.nodes.at(0).known, "认识的算子 known=true");
        check(p.graph.nodes.at(0).uuid == uuidOf("u-src"),
              QString("uuid 原样(实际 \"%1\")").arg(p.graph.nodes.at(0).uuid));
    }
}

// ============================================================
// 用例 6:三个计数**不许说谎**
// ============================================================
//
// 混合场景:1 个没装的算子 + 1 条真断边 + 1 条缺方向的边。
// 三个计数各管各的,不能互相顶包。
static void test_the_three_counts_are_honest() {
    beginCase("6. 三个计数各管各的,不许互相顶包");

    QJsonObject noSide = mkEdge("u-src", "u-judge");
    // 故意不给 from_side / to_side
    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-src"),
                    mkNode("ghost", "u-ghost"),
                    mkNode("judge", "u-judge") },
        QJsonArray{ noSide, mkEdge("u-src", "u-压根不存在") });

    const Parsed p = parse(doc);

    check(p.graph.unknownNodes == 1,
          QString("unknownNodes = 1(实际 %1)").arg(p.graph.unknownNodes));
    check(p.graph.brokenEdges == 1,
          QString("brokenEdges = 1(实际 %1)").arg(p.graph.brokenEdges));
    check(p.graph.defaultedSides == 1,
          QString("defaultedSides = 1(实际 %1)").arg(p.graph.defaultedSides));
}

// ============================================================
// 用例 7:这三个计数**不许再变成死字段**
// ============================================================
//
// 欠账 19 的原话:「**写了没人读**」。
//
// 以前它们**只写不读** —— 生产代码里一处都不读,
// 只有测试断言它们是 0。那证明的是「它们一直是 0」,
// 不是「有人在看」。
//
// ⚠⚠ 2026-10-25:第一版的守卫是这么写的:
//       check(code.contains("loadedGraph.unknownNodes"), ...);
//   然后**变异 M179 活了** ——
//   因为把 .arg(loadedGraph.unknownNodes) 换成硬编码 .arg(0) 之后,
//   「loadedGraph.unknownNodes」这个词**还在**(在 if 条件里),
//   而**值已经没了**。守卫量的是「名字在不在」,
//   它要量的却是「**值有没有被用上**」。
//
//   所以现在:
//     · **内容**由用例 8 直接测(喂一张图进去,看它说没说对)
//     · GUI 那一侧的判据换成「**可测入口**」——
//       mainwindow 必须调 w7::describeLoadAnomalies,
//       而且把结果**报出来**。
//
// ⚠ 判据落在**可测入口**上,不是某种写法;
//   查源码之前**先剥掉注释**(本项目栽过:守卫被一句注释喂饱)。
static void test_the_three_counts_are_not_dead_fields() {
    beginCase("7. 三个计数不许再变成死字段");

    const QString base = QString(W7_SOURCE_DIR);

    // ---- mainwindow 必须**用**这三个计数 ----
    {
        const QString code = stripComments(readSource(base + "/mainwindow.cpp"));
        check(code.contains("w7::describeLoadAnomalies"),
              "mainwindow.cpp 调的是 w7::describeLoadAnomalies(...) —— "
              "话由**一份**生成,而且那份**能直接测**");
        check(code.contains("appendLog(anomalies)"),
              "mainwindow.cpp 把读盘异常**报出来**了(不是算完就扔)");
    }

    // ---- headless_runner 必须读(机器可读的那条路) ----
    {
        const QString code = stripComments(readSource(base + "/headless_runner.cpp"));
        check(code.contains("g.unknownNodes"),
              "headless_runner.cpp 读了 unknownNodes");
        check(code.contains("g.brokenEdges"),
              "headless_runner.cpp 读了 brokenEdges");
        check(code.contains("g.defaultedSides"),
              "headless_runner.cpp 读了 defaultedSides");
    }

    // ---- 名字不许改回说谎的那两个 ----
    {
        const QString code = stripComments(readSource(base + "/pipeline_graph.h"));
        check(!code.contains("skippedNodes"),
              "pipeline_graph.h 里没有 skippedNodes 了(那名字现在是**说谎**的)");
        check(!code.contains("skippedEdges"),
              "pipeline_graph.h 里没有 skippedEdges 了(同上)");
        check(code.contains("unknownNodes"),
              "pipeline_graph.h 里有 unknownNodes(诚实的新名字)");
        check(code.contains("brokenEdges"),
              "pipeline_graph.h 里有 brokenEdges(诚实的新名字)");
    }

    // ---- 画布上必须标出来 ----
    {
        // 「节点留在画布上」还不够 —— 得**看得出门道**。
        // 一个灰扑扑的方块,用户会当成正常节点。
        const QString code = stripComments(readSource(base + "/pipeline_serialize.cpp"));
        check(code.contains("setOperatorMissing"),
              "pipeline_serialize.cpp 调了 setOperatorMissing(节点被标成「未安装」)");
    }
}

// ============================================================
// 用例 8:那段人话**说得对**
// ============================================================
//
// 这条是「三个计数不许变成死字段」的**强**版本:
// 用例 7 只能证明「有人提到它们」,
// 这条证明「**用出来的数是真的**」。
//
// ⚠ 变异 M179 就是这么死的:
//   把 .arg(g.unknownNodes) 换成 .arg(0),
//   消息里就会出现「有 0 个算子没装」—— 一句假话。
//   而用例 7 的守卫**看不见**(那个词还在),
//   用例 8 看得见。
static void test_the_message_says_the_real_numbers() {
    beginCase("8. 那段人话说的是真的数");

    // 没有毛病 -> 一句话都不说(不刷屏)
    {
        const PipelineGraph g;   // 三个计数全是 0
        check(describeLoadAnomalies(g).isEmpty(),
              "没有异常时**一句话都不说**(不刷屏)");
    }

    // 有算子没装 -> 消息里必须有**真的数**
    {
        PipelineGraph g;
        g.unknownNodes = 3;
        const QString msg = describeLoadAnomalies(g);
        check(msg.contains("3"),
              QString("消息里写的是 3(实际 \"%1\")").arg(msg));
        check(!msg.contains("0 个算子"),
              "不是硬编码的 0(变异 M179 就会这么活)");
    }

    // 断边
    {
        PipelineGraph g;
        g.brokenEdges = 7;
        const QString msg = describeLoadAnomalies(g);
        check(msg.contains("7"),
              QString("断边的数也是真的(实际 \"%1\")").arg(msg));
    }

    // 缺方向
    {
        PipelineGraph g;
        g.defaultedSides = 5;
        const QString msg = describeLoadAnomalies(g);
        check(msg.contains("5"),
              QString("缺方向的数也是真的(实际 \"%1\")").arg(msg));
    }

    // 三种毛病同时在 -> 一件一件说
    {
        PipelineGraph g;
        g.unknownNodes   = 1;
        g.brokenEdges    = 2;
        g.defaultedSides = 4;
        const QString msg = describeLoadAnomalies(g);
        check(msg.contains("1") && msg.contains("2") && msg.contains("4"),
              "三种毛病的数都在消息里");
        check(msg.split(QLatin1Char('\n')).size() == 3,
              QString("说了三句(实际 %1 句)")
                  .arg(msg.split(QLatin1Char('\n')).size()));
    }
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 解析层(pipeline_graph)回归测试";

    test_unknown_node_is_kept_and_chain_intact();
    test_truly_missing_endpoint_is_still_broken();
    test_legacy_opname_pairing_counts_unknown_ops();
    test_unknown_node_has_a_name_on_canvas();
    test_known_operators_are_untouched();
    test_the_three_counts_are_honest();
    test_the_three_counts_are_not_dead_fields();
    test_the_message_says_the_real_numbers();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
