// ============================================================
// W7 流水线执行引擎的回归测试(拓扑序 + 前驱输出合并)
// ============================================================
//
// 测的是哪两件事:
//
//   1. 拓扑序(含**环检测**)
//   2. 多个前驱的输出合并时,谁赢
//
// 为什么这两件事以前没有测试:
//
//   它们以前住在 mainwindow.cpp 里,而 mainwindow.cpp
//   **不在任何测试目标里**(它要真窗口 + 整套算子,建一次要好几分钟)。
//   于是它们只有源码 lint 看着 —— 而 lint 看不见"顺序反了会怎样"。
//
// 第二件尤其要紧。2026-10-15 有一次一次性探针
// (手抄 mainwindow.cpp 的合并逻辑跑出来的)实测到:
//
//   顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue / 5000
//   顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red  / 10000
//
// 两个上游状态点都是绿的、都不报错,只有下游那个数悄悄变了。
// 而且**连"先连线的赢"都不成立** —— 决定胜负的是遍历顺序。
//
// 那份探针是一次性的:合并逻辑一动它就过期,而且**没人会知道**。
// 现在这段逻辑是**一份**,在 run_engine.h 里,这里直接测它。
//
// ============================================================

#include <QJsonObject>
#include <QDebug>
#include <QFile>

#include <memory>

#include "run_engine.h"

using namespace w7;

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

// 把 int 序列转成能看的字符串
static QString show(const QVector<int>& v) {
    QStringList s;
    for (int i : v) s << QString::number(i);
    return s.join(QLatin1Char(','));
}

// ============================================================
// 用例 1:拓扑序 —— 各种形状
// ============================================================
static void test_topological_order() {
    beginCase("1. 拓扑序:上游一定排在下游前面");

    // 空图
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(0, {}, &cyc);
        check(o.isEmpty(), "空图 -> 空序");
        check(!cyc, "空图不算有环");
    }

    // 单节点
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(1, {}, &cyc);
        check(o.size() == 1 && o.first() == 0, "单节点 -> [0]");
        check(!cyc, "单节点不算有环");
    }

    // 一条链 0->1->2
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(3, {EngineEdge{0, 1}, EngineEdge{1, 2}}, &cyc);
        check(o == QVector<int>({0, 1, 2}),
              QString("链 0->1->2 -> [0,1,2](实际 %1)").arg(show(o)));
        check(!cyc, "链不算有环");
    }

    // 反着给的链 2->1->0:拓扑序还是得让 2 在前
    //
    // ⚠ 这条不是为了"链要正着写",而是为了证明:
    //   拓扑序是**算出来的**,不是照着边的顺序抄的。
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(3, {EngineEdge{2, 1}, EngineEdge{1, 0}}, &cyc);
        check(o == QVector<int>({2, 1, 0}),
              QString("反着给的链 2->1->0 -> [2,1,0](实际 %1)").arg(show(o)));
    }

    // 菱形 0->1, 0->2, 1->3, 2->3
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(4, {EngineEdge{0, 1}, EngineEdge{0, 2},
                                             EngineEdge{1, 3}, EngineEdge{2, 3}}, &cyc);
        check(o.size() == 4, QString("菱形 -> 4 个节点都排到了(实际 %1)").arg(show(o)));
        // 0 必须在最前,3 必须在最后
        check(o.first() == 0, QString("菱形的入度 0 节点(0)排最前(实际 %1)").arg(show(o)));
        check(o.last() == 3, QString("菱形的汇合点(3)排最后(实际 %1)").arg(show(o)));
        // 1 和 2 都在 3 之前
        const int i1 = o.indexOf(1), i2 = o.indexOf(2), i3 = o.indexOf(3);
        check(i1 >= 0 && i2 >= 0 && i1 < i3 && i2 < i3,
              QString("菱形里 1 和 2 都排在汇合点 3 前面(实际 %1)").arg(show(o)));
    }

    // 边的顺序**不影响**拓扑序
    //
    // 为什么这条重要:拓扑序有多个合法解,选哪个是实现细节 ——
    // 但它必须**确定**。不的话,同一份流水线两次跑可能给出不同的
    // 执行顺序,那是极难查的一类 bug(症状是"同样的图,结果时对时错")。
    {
        bool c1 = true, c2 = true;
        const QVector<int> a = topoOrder(3, {EngineEdge{0, 1}, EngineEdge{1, 2}}, &c1);
        const QVector<int> b = topoOrder(3, {EngineEdge{1, 2}, EngineEdge{0, 1}}, &c2);
        check(a == b,
              QString("边顺序打乱,拓扑序不变(实际 %1 vs %2)").arg(show(a), show(b)));
    }
}

// ============================================================
// 用例 2:环 —— 必须检出,而且**什么都不跑**
// ============================================================
static void test_cycle_detection() {
    beginCase("2. 环:必须检出,而且返回空序(宁可什么都不跑)");

    // 三角环 0->1->2->0
    {
        bool cyc = false;
        const QVector<int> o = topoOrder(3, {EngineEdge{0, 1}, EngineEdge{1, 2},
                                             EngineEdge{2, 0}}, &cyc);
        check(cyc, "三角环 0->1->2->0 -> 检出有环");
        check(o.isEmpty(),
              QString("有环时返回空序(实际 %1)—— 跑一半的话,下游拿到的是"
                      "上一轮的输出,数字看着正常,结论却是错的").arg(show(o)));
    }

    // 自环
    {
        bool cyc = false;
        const QVector<int> o = topoOrder(2, {EngineEdge{0, 0}}, &cyc);
        check(cyc, "自环 0->0 -> 检出有环");
        check(o.isEmpty(), QString("自环时返回空序(实际 %1)").arg(show(o)));
    }

    // 环挂在链尾:前面那截没问题,但**整条**都不能跑
    {
        bool cyc = false;
        const QVector<int> o = topoOrder(4, {EngineEdge{0, 1}, EngineEdge{1, 2},
                                             EngineEdge{2, 3}, EngineEdge{3, 1}}, &cyc);
        check(cyc, "环挂在链尾 -> 照样检出有环");
        check(o.isEmpty(),
              QString("有环时整条链都不跑(实际 %1)—— 跑前半截再停,"
                      "等于给出半个结论").arg(show(o)));
    }
}

// ============================================================
// 用例 3:坏边 —— 跳过,但不能崩
// ============================================================
static void test_bad_edges() {
    beginCase("3. 坏边:跳过,不许崩");

    // 越界的边
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(2, {EngineEdge{0, 1}, EngineEdge{5, 1},
                                             EngineEdge{0, 9}}, &cyc);
        check(o == QVector<int>({0, 1}),
              QString("越界的边被跳过,好边照排(实际 %1)").arg(show(o)));
        check(!cyc, "越界的边不算有环");
    }

    // from/to 是 -1(没填)
    {
        bool cyc = true;
        const QVector<int> o = topoOrder(2, {EngineEdge{-1, 1}, EngineEdge{0, -1}}, &cyc);
        check(o == QVector<int>({0, 1}),
              QString("没填的边被跳过(实际 %1)").arg(show(o)));
    }
}

// ============================================================
// 用例 4:合并 —— 后写覆盖
// ============================================================
static void test_merge_outputs() {
    beginCase("4. 合并多个前驱的输出:后写覆盖");

    // 空
    {
        const QJsonObject m = mergeUpstreamOutputs({});
        check(m.isEmpty(), "没有前驱 -> 空对象");
    }

    // 一个
    {
        QJsonObject a;
        a["x"] = 1;
        const QJsonObject m = mergeUpstreamOutputs({a});
        check(m.value("x").toInt() == 1, "一个前驱 -> 原样带过来");
    }

    // 两个,键不冲突
    {
        QJsonObject a, b;
        a["x"] = 1;
        b["y"] = 2;
        const QJsonObject m = mergeUpstreamOutputs({a, b});
        check(m.value("x").toInt() == 1 && m.value("y").toInt() == 2,
              "两个前驱的键不冲突 -> 都留着");
    }

    // 两个,键冲突:**后者赢**
    {
        QJsonObject a, b;
        a["count"] = 10000;
        b["count"] = 5000;
        const QJsonObject m = mergeUpstreamOutputs({a, b});
        check(m.value("count").toInt() == 5000,
              QString("键冲突 -> 后写覆盖(实际 %1,期望 5000)")
                  .arg(m.value("count").toInt()));
    }

    // 三个冲突:最后一个赢
    {
        QJsonObject a, b, c;
        a["k"] = 1; b["k"] = 2; c["k"] = 3;
        const QJsonObject m = mergeUpstreamOutputs({a, b, c});
        check(m.value("k").toInt() == 3,
              QString("三个冲突 -> 最后一个赢(实际 %1,期望 3)")
                  .arg(m.value("k").toInt()));
    }

    // 类型也覆盖(不只是数值)
    {
        QJsonObject a, b;
        a["k"] = "text";
        b["k"] = 42;
        const QJsonObject m = mergeUpstreamOutputs({a, b});
        check(m.value("k").isDouble() && m.value("k").toInt() == 42,
              "键冲突时**类型**也跟着换(不能只换值、留着旧类型)");
    }

    // 空的上游不干扰
    {
        QJsonObject a, empty;
        a["x"] = 1;
        const QJsonObject m = mergeUpstreamOutputs({a, empty});
        check(m.value("x").toInt() == 1, "空的上游不干扰别的键");
    }
}

// ============================================================
// 用例 5:顺序敏感性 —— 那条静默失效的机制本身
// ============================================================
static void test_merge_is_order_sensitive() {
    beginCase("5. 合并对顺序敏感:这条静默失效的机制必须被钉住");

    // 这就是 2026-10-15 那次探针实测到的两个场景。
    //
    // ⚠ 这两条断言看着像"在断言一个 bug",
    //   但它们断言的是**当前真实语义** —— 引擎必须和主程序一致。
    //   哪天把语义改成"先连线的赢",这两条会红,
    //   那时候要一起改的是:引擎 + 主程序的合并顺序 + pin_types 的提示文案。
    QJsonObject a, b;               // A = 红色块 10000px,B = 蓝色块 5000px
    a["mask_color"] = "red";   a["area"] = 10000;
    b["mask_color"] = "blue";  b["area"] = 5000;

    const QJsonObject ab = mergeUpstreamOutputs({a, b});
    const QJsonObject ba = mergeUpstreamOutputs({b, a});

    check(ab.value("mask_color").toString() == "blue" &&
          ab.value("area").toInt() == 5000,
          "顺序 [A,B] -> 下游拿到 blue / 5000");
    check(ba.value("mask_color").toString() == "red" &&
          ba.value("area").toInt() == 10000,
          "顺序 [B,A] -> 下游拿到 red / 10000");
    check(ab.value("area").toInt() != ba.value("area").toInt(),
          "换个顺序下游那个数就变 —— 这就是静默失效的根源:"
          "两个上游状态点都是绿的、都不报错,只有下游悄悄变了");
    check(ab.value("mask_color").toString() != ba.value("mask_color").toString(),
          "换个顺序下游连颜色都变 —— 状态点看不出来,数字对不对只有下游知道");
}

// ============================================================
// 用例 6:守卫 —— 这条用例自己不能是空转
// ============================================================
static void test_not_vacuous() {
    beginCase("6. 守卫:这条用例不是空转");

    // 故意写一条**错**的断言,看它会不会红 ——
    // 不行,测试不能自己制造失败。
    // 改成:用一条**已知成立**的反向检查证明引擎真的在干活:
    // 把边的方向反过来,拓扑序必须跟着变。
    bool cyc = false;
    const QVector<int> fwd = topoOrder(2, {EngineEdge{0, 1}}, &cyc);
    const QVector<int> rev = topoOrder(2, {EngineEdge{1, 0}}, &cyc);
    check(fwd != rev,
          QString("同一条边方向反过来,拓扑序就变(实际 %1 vs %2)")
              .arg(show(fwd), show(rev)));
    check(!fwd.isEmpty() && !rev.isEmpty(), "两次都排出了序(不是恒真的空断言)");
}

// ============================================================
// 用例 7:runFrame —— 跑一帧
// ============================================================
namespace {

// 假的算子执行器:把收到的 inputs / params 记下来,输出一个带 opName 的对象
//
// ⚠ 记录用的状态必须放在**共享**的地方:
//   runFrame 收的是 std::function,而 std::function **按值存** ——
//   传进去的 functor 会被拷贝一份,记到副本上,外面看不到。
//   这个坑很安静:测试不会崩,只会"断言莫名其妙地红"。
struct Recorder {
    QVector<QJsonObject> gotInputs;
    QVector<QJsonObject> gotParams;
    QString failOn;                 // 这个算子名要失败
    QString failMessage = QStringLiteral("算子炸了");
};

struct FakeRunner {
    std::shared_ptr<Recorder> rec;
    bool operator()(const QString& opName, const QJsonObject& params,
                    const QJsonObject& inputs, QJsonObject* out, QString* err) {
        rec->gotInputs.append(inputs);
        rec->gotParams.append(params);
        if (!rec->failOn.isEmpty() && opName == rec->failOn) {
            *err = rec->failMessage;
            return false;
        }
        QJsonObject o;
        o["op"] = opName;
        if (inputs.contains("echo")) o["echo"] = inputs.value("echo");
        *out = o;
        return true;
    }
};

// 一个"爱乱写"的上游:它自己的输出里带 frame_index / node_uuid。
// 引擎给下游的 inputs 必须是**引擎自己**那两个,不是上游乱写的。
struct LiarRunner {
    std::shared_ptr<Recorder> rec;
    bool operator()(const QString& opName, const QJsonObject&,
                    const QJsonObject& inputs, QJsonObject* out, QString*) {
        rec->gotInputs.append(inputs);
        QJsonObject o;
        o["op"] = opName;
        if (opName == QStringLiteral("src")) {
            o["frame_index"] = 999;
            o["node_uuid"] = QStringLiteral("fake-uuid");
        }
        *out = o;
        return true;
    }
};

// 上游自己产出了 image_path —— 用来验"兜底只补缺的,不覆盖上游的"
//
// ⚠ 它**也记 inputs** —— 因为这条规则真正要验的是
//   「**下游**拿到的 image_path 是上游那个还是兜底那个」,
//   不是「上游自己的输出有没有被改」。
//   (变异 M55 就是从这儿活的:当初只验了上游自己的输出,
//    兜底改成"总是覆盖"照样全绿 —— 详见 notes/atomic 那张卡。)
struct SrcRunner {
    std::shared_ptr<Recorder> rec;
    bool operator()(const QString&, const QJsonObject&, const QJsonObject& inputs,
                    QJsonObject* out, QString*) {
        if (rec) rec->gotInputs.append(inputs);
        QJsonObject o;
        o["op"] = QStringLiteral("src");
        o["image_path"] = QStringLiteral("from-upstream.png");
        *out = o;
        return true;
    }
};

EngineNode mkNode(const QString& opName, const QString& uuid = QString()) {
    EngineNode n;
    n.opName = opName;
    n.uuid = uuid.isEmpty() ? (QStringLiteral("u-") + opName) : uuid;
    return n;
}

} // namespace

static void test_run_frame() {
    beginCase("7. runFrame:跑一帧");

    // ---- 空画布 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        const EngineFrame f = runFrame({}, {}, {}, 0, -1,
                                       {}, r, {}, {});
        check(!f.ok, "空画布 -> 不跑");
        check(!f.error.isEmpty(),
              QString("空画布要说清为什么(实际 \"%1\")").arg(f.error));
    }

    // ---- 一条链:上游的输出要流到下游 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("mid"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}, EngineEdge{1, 2}};
        const EngineFrame f = runFrame(nodes, edges, {}, 0, -1, {}, r, {}, {});
        check(f.ok, "一条三节点的链 -> 跑通");
        check(f.executed.size() == 3,
              QString("三个节点都跑了(实际 %1)").arg(f.executed.size()));
        check(f.outputs.size() == 3,
              QString("输出数组和节点数一样长(实际 %1)—— "
                      "下标必须和节点一一对应,否则主程序按 idx 取会取错")
                  .arg(f.outputs.size()));
        check(f.outputs.at(0).value("op").toString() == "src", "节点 0 的输出是 src");
        check(f.outputs.at(2).value("op").toString() == "dst", "节点 2 的输出是 dst");
        // 下游拿到的 inputs 里应该有上游的输出
        check(rec->gotInputs.size() == 3, "执行器被调了 3 次");
        check(rec->gotInputs.at(2).contains("op"),
              "下游拿到的 inputs 里带着上游的输出");
        check(rec->gotInputs.at(2).value("op").toString() == "mid",
              QString("下游拿到的是**直接上游**的输出(实际 \"%1\")")
                  .arg(rec->gotInputs.at(2).value("op").toString()));
    }

    // ---- inputs 的组装顺序:合并 -> 兜底(缺了才补) -> 总是覆盖 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}};
        QJsonObject fallback;
        fallback["image_path"] = QStringLiteral("fallback.png");

        const EngineFrame f = runFrame(nodes, edges, fallback, 7, -1, {}, r, {}, {});
        check(f.ok, "带兜底输入 -> 跑通");
        const QJsonObject& in = rec->gotInputs.at(1);
        check(in.value("image_path").toString() == "fallback.png",
              "上游没给 image_path -> 用兜底填上");
        check(in.value("frame_index").toInt() == 7,
              QString("frame_index 传下去了(实际 %1)").arg(in.value("frame_index").toInt()));
        check(in.value("node_uuid").toString() == "u-dst",
              QString("node_uuid 传下去了(实际 \"%1\")").arg(in.value("node_uuid").toString()));
    }

    // ---- 兜底**只补缺的**;上游给了就不能覆盖 ----
    {
        // 上游自己产出了 image_path(用命名空间里那个 SrcRunner,别在这儿再抄一份)
        auto rec = std::make_shared<Recorder>();
        SrcRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}};
        QJsonObject fallback;
        fallback["image_path"] = QStringLiteral("fallback.png");
        const EngineFrame f = runFrame(nodes, edges, fallback, 0, -1, {},
                                       r, {}, {});
        check(f.ok, "上游自己产 image_path -> 跑通");
        check(f.outputs.at(0).value("image_path").toString() == "from-upstream.png",
              "上游自己的输出里 image_path 还在");
        // ⚠⚠ **这一条才是这条规则真正要验的**
        //
        // 上面那条验的是"上游的**输出**没被改" ——
        // 而兜底只动下游的 inputs,**压根碰不到上游的输出**。
        // 所以上面那条对"兜底是否覆盖上游"这个变异是**免疫**的
        // (变异 M55 实测:全绿)。
        //
        // 真正会出事的地方在**下游**:下游拿到的 image_path
        // 到底该是上游给的那个,还是兜底给的 fallback.png?
        // 兜底**只该补缺的** —— 上游给了就不能覆盖,
        // 不然上游辛苦算出来的结果就被一条兜底规则顶掉了。
        check(rec->gotInputs.size() == 2, "两个节点都跑了");
        check(rec->gotInputs.at(1).value("image_path").toString() == "from-upstream.png",
              QString("上游给了 image_path -> 下游拿到的还是**上游那个**"
                      "(实际 \"%1\",兜底给的是 fallback.png)")
                  .arg(rec->gotInputs.at(1).value("image_path").toString()));
    }

    // ---- frame_index / node_uuid **总是覆盖**上游给的 ----
    {
        // 一个"爱乱写"的上游:它自己的输出里带 frame_index / node_uuid。
        // 引擎给下游的 inputs 必须是**引擎自己**那两个,不是上游乱写的。
        auto rec = std::make_shared<Recorder>();
        LiarRunner liar{rec};
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}};
        const EngineFrame f = runFrame(nodes, edges, {}, 3, -1, {}, liar, {}, {});
        check(f.ok, "上游乱写 frame_index -> 跑通");
        check(rec->gotInputs.size() == 2, "两个节点都跑了");
        check(rec->gotInputs.at(1).value("frame_index").toInt() == 3,
              QString("下游拿到的 frame_index 是引擎给的那个(实际 %1,上游乱写的是 999)")
                  .arg(rec->gotInputs.at(1).value("frame_index").toInt()));
        check(rec->gotInputs.at(1).value("node_uuid").toString() == "u-dst",
              QString("下游拿到的 node_uuid 是引擎给的那个(实际 \"%1\",上游乱写的是 fake-uuid)")
                  .arg(rec->gotInputs.at(1).value("node_uuid").toString()));
    }

    // ---- params:节点参数优先,空则用声明默认值 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("a"), mkNode("b")};
        nodes[0].params["k"] = 1;          // 节点自己存了参数
        nodes[1].params = QJsonObject();    // 空的 -> 该用声明默认值

        int defaultsCalls = 0;
        auto defaults = [&](const QString& opName) -> QJsonObject {
            ++defaultsCalls;
            QJsonObject d;
            d["declared"] = opName;
            return d;
        };

        const EngineFrame f = runFrame(nodes, {}, {}, 0, -1, defaults, r, {}, {});
        check(f.ok, "params 组装 -> 跑通");
        check(rec->gotParams.at(0).value("k").toInt() == 1,
              "节点自己存了参数 -> 用节点的");
        check(rec->gotParams.at(1).contains("declared"),
              "节点参数为空 -> 用算子的**声明默认值**");
        check(defaultsCalls == 1,
              QString("声明默认值**只在需要时**去取(实际取了 %1 次 —— "
                      "节点有参数就不该多跑一趟)").arg(defaultsCalls));
    }

    // ---- 算子失败:这一步没产出,但**链要继续往下跑** ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        rec->failOn = QStringLiteral("mid");
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("mid"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}, EngineEdge{1, 2}};

        QString seenError;
        bool sawFailed = false;
        auto hook = [&](EnginePhase, const EngineStepInfo& info) {
            if (info.failed) { sawFailed = true; seenError = info.error; }
        };

        const EngineFrame f = runFrame(nodes, edges, {}, 0, -1, {}, r, {}, hook);
        check(f.ok, "中间那个算子失败 -> 整帧**不算崩**");
        check(sawFailed, "hook 收到了失败通知");
        check(seenError == rec->failMessage,
              QString("失败原因传上来了(实际 \"%1\")").arg(seenError));
        check(f.outputs.at(1).isEmpty(), "失败的节点没有产出");
        check(f.executed.size() == 2,
              QString("失败的那步不算执行成功,其余两步照跑(实际执行了 %1 步)")
                  .arg(f.executed.size()));
    }

    // ---- 节点已被删除:跳过,而且**不许**跑它 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("src"), mkNode("gone"), mkNode("dst")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}, EngineEdge{1, 2}};

        QVector<int> skipped;
        auto hook = [&](EnginePhase phase, const EngineStepInfo& info) {
            if (phase == EnginePhase::Skipped) skipped.append(info.step);
        };
        // 第 1 步(下标 1)的节点已经被删了
        auto alive = [](int idx) { return idx != 1; };

        const EngineFrame f = runFrame(nodes, edges, {}, 0, -1, {}, r, alive, hook);
        check(f.ok, "有节点被删 -> 整帧不算崩");
        check(skipped.size() == 1 && skipped.first() == 1,
              QString("被删的那个节点被跳过了(实际跳过了 %1 个)").arg(skipped.size()));
        check(f.executed.size() == 2,
              QString("被删的节点没跑,其余两个跑了(实际执行了 %1 步)")
                  .arg(f.executed.size()));
        check(f.outputs.at(1).isEmpty(), "被跳过的节点没有产出");
    }

    // ---- maxSteps:只跑前 N 个(单步执行用) ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("a"), mkNode("b"), mkNode("c")};
        QVector<EngineEdge> edges = {EngineEdge{0, 1}, EngineEdge{1, 2}};
        const EngineFrame f = runFrame(nodes, edges, {}, 0, 2, {}, r, {}, {});
        check(f.executed.size() == 2,
              QString("maxSteps=2 -> 只跑 2 个(实际 %1)").arg(f.executed.size()));
        check(f.outputs.at(2).isEmpty(), "范围外的节点没跑");
    }

    // ---- 顺序不对:必须报错,不能闷头跑 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        // 边说 1 -> 0,但 nodes 的执行序是 0 在前 —— 顺序自相矛盾
        QVector<EngineNode> nodes = {mkNode("a"), mkNode("b")};
        QVector<EngineEdge> edges = {EngineEdge{1, 0}};
        const EngineFrame f = runFrame(nodes, edges, {}, 0, -1, {}, r, {}, {});
        check(!f.ok, "顺序自相矛盾 -> 拒绝跑");
        check(!f.error.isEmpty(),
              QString("顺序错了要说清为什么(实际 \"%1\")").arg(f.error));
        check(rec->gotInputs.isEmpty(), "顺序错了就一步都不该跑");
    }

    // ---- hook 的调用序列:每个节点 Before 一次、After 一次 ----
    {
        auto rec = std::make_shared<Recorder>();
        FakeRunner r{rec};
        QVector<EngineNode> nodes = {mkNode("a"), mkNode("b")};
        QStringList seq;
        auto hook = [&](EnginePhase phase, const EngineStepInfo& info) {
            seq << (phase == EnginePhase::Before ? QStringLiteral("B%1").arg(info.step)
                                                 : QStringLiteral("A%1").arg(info.step));
        };
        const EngineFrame f = runFrame(nodes, {}, {}, 0, -1, {}, r, {}, hook);
        check(f.ok, "hook 序列 -> 跑通");
        check(seq == (QStringList{QStringLiteral("B0"), QStringLiteral("A0"),
                                  QStringLiteral("B1"), QStringLiteral("A1")}),
              QString("每个节点 Before 一次、After 一次(实际 %1)").arg(seq.join(QLatin1Char(','))));
    }
}

// ============================================================
// 用例 9:hook 拿到的信息 —— 每个字段都得对得上
// ============================================================
//
// ⚠ 为什么单独一条用例:
//   用例 7 验的是"引擎**做了什么"(跑了几个、跳过没、失败没),
//   但 hook 收到的**那一份**信息本身从没被验过 ——
//   nodeIndex / uuid / output / skipped 这几个字段,
//   改错了用例 7 一条都不会红。
//
//   而主程序恰恰全靠这些字段:
//     · nodeIndex -> 去 order 里取节点(取错就摸到别的节点)
//     · uuid      -> 每步缓存的键(错了就存串了)
//     · output    -> 状态点 + 日志(错了就显示错的数)
//     · skipped   -> 判断这是"被跳过"还是"跑失败"
//
//   "字段没人验"就是"字段没有保护"—— 那和没抽出来没区别。
// ============================================================
namespace {

// 把 hook 收到的东西按顺序存下来
struct StepRec {
    QVector<int> phase;        // 0=Before 1=After 2=Skipped
    QVector<int> nodeIndex;
    QVector<int> step;
    QVector<QString> opName;
    QVector<QString> uuid;
    QVector<bool> skipped;
    QVector<bool> failed;
    QVector<QJsonObject> output;

    void add(int p, const EngineStepInfo& i) {
        phase.append(p);
        nodeIndex.append(i.nodeIndex);
        step.append(i.step);
        opName.append(i.opName);
        uuid.append(i.uuid);
        skipped.append(i.skipped);
        failed.append(i.failed);
        output.append(i.output);
    }
    int indexOfPhase(int p) const {
        for (int i = 0; i < phase.size(); ++i) if (phase.at(i) == p) return i;
        return -1;
    }
};

} // namespace

static void test_step_info_fields() {
    beginCase("9. hook 拿到的信息:每个字段都得对得上");

    auto rec = std::make_shared<StepRec>();
    auto runnerRec = std::make_shared<Recorder>();
    FakeRunner r{runnerRec};

    // 三个节点,中间那个(b)会被"删掉" —— 于是它走 Skipped 那条路,
    // 前后两个走正常的那条路。一条用例覆盖三种阶段。
    QVector<EngineNode> nodes = {mkNode("a"), mkNode("b"), mkNode("c")};
    QVector<EngineEdge> edges = {EngineEdge{0, 1}, EngineEdge{1, 2}};
    auto alive = [](int idx) { return idx != 1; };

    auto hook = [&](EnginePhase phase, const EngineStepInfo& info) {
        const int p = (phase == EnginePhase::Before) ? 0
                     : (phase == EnginePhase::After) ? 1 : 2;
        rec->add(p, info);
    };

    const EngineFrame f = runFrame(nodes, edges, {}, 0, -1, {}, r, alive, hook);
    check(f.ok, "hook 信息 -> 跑通");

    // ---- Skipped 那一条 ----
    const int sk = rec->indexOfPhase(2);
    check(sk >= 0, "hook 收到了 Skipped 通知");
    if (sk >= 0) {
        check(rec->step.at(sk) == 1,
              QString("Skipped 说的是第 1 步(实际 %1)").arg(rec->step.at(sk)));
        check(rec->nodeIndex.at(sk) == 1,
              QString("Skipped 的 nodeIndex 是 1(实际 %1)—— 主程序靠它去 order 里取节点,"
                      "取错就摸到别的节点去了").arg(rec->nodeIndex.at(sk)));
        check(rec->opName.at(sk) == "b",
              QString("Skipped 的 opName 是 b(实际 \"%1\")").arg(rec->opName.at(sk)));
        check(rec->uuid.at(sk) == "u-b",
              QString("Skipped 的 uuid 是 u-b(实际 \"%1\")—— 它是每步缓存的键,错了就存串了")
                  .arg(rec->uuid.at(sk)));
        check(rec->skipped.at(sk), "Skipped 那条带着 skipped 标记");
        check(!rec->failed.at(sk), "Skipped 那条**不是** failed(跳过 ≠ 失败)");
    }

    // ---- Before(节点 a)----
    const int be = rec->indexOfPhase(0);
    check(be >= 0, "hook 收到了 Before 通知");
    if (be >= 0) {
        check(rec->nodeIndex.at(be) == 0 && rec->step.at(be) == 0,
              QString("Before 的 nodeIndex/step 都是 0(实际 %1/%2)")
                  .arg(rec->nodeIndex.at(be)).arg(rec->step.at(be)));
        check(rec->opName.at(be) == "a",
              QString("Before 的 opName 是 a(实际 \"%1\")").arg(rec->opName.at(be)));
        check(rec->uuid.at(be) == "u-a",
              QString("Before 的 uuid 是 u-a(实际 \"%1\")").arg(rec->uuid.at(be)));
        check(!rec->skipped.at(be), "Before 那条没有 skipped 标记");
        check(!rec->failed.at(be), "Before 那条还没有 failed 标记(还没跑呢)");
    }

    // ---- After(节点 a,跑完了)----
    const int af = rec->indexOfPhase(1);
    check(af >= 0, "hook 收到了 After 通知");
    if (af >= 0) {
        check(rec->nodeIndex.at(af) == 0,
              QString("After 的 nodeIndex 是 0(实际 %1)").arg(rec->nodeIndex.at(af)));
        check(!rec->failed.at(af), "跑成功的 After 那条没有 failed 标记");
        check(rec->output.at(af).value("op").toString() == "a",
              QString("After 带的是算子**真正的输出**(实际 \"%1\")—— "
                      "主程序的状态点和日志全靠它")
                  .arg(rec->output.at(af).value("op").toString()));
    }

    // ---- 每一阶段的 nodeIndex 都必须等于 step ----
    //
    // ⚠ 当前设计里这两者相等(节点数组**就是**执行序),
    //   但它们是**两件事**:nodeIndex 是"哪个节点",step 是"第几步"。
    //   哪天引擎改成不预先排序,这两个就分开了 ——
    //   那时候主程序用错一个就是摸错节点。
    //   所以在这里分别钉住,别等它们分家了才发现没人守。
    bool idxOk = true;
    for (int i = 0; i < rec->phase.size(); ++i) {
        if (rec->nodeIndex.at(i) != rec->step.at(i)) idxOk = false;
    }
    check(idxOk, "每一阶段的 nodeIndex 都等于 step");

    // ---- 被跳过的节点,不该进过 Before/After ----
    bool bTouched = false;
    for (int i = 0; i < rec->phase.size(); ++i) {
        if (rec->phase.at(i) != 2 && rec->step.at(i) == 1) bTouched = true;
    }
    check(!bTouched, "被跳过的节点从没进过 Before/After(只该有 Skipped 那一条)");
}

// ============================================================
// 用例 8:源码守卫 —— mainwindow 必须走引擎
// ============================================================
//
// 为什么这条是 lint 而不是行为测试:
//   mainwindow.cpp **不在任何测试目标里**(它要真窗口 + 整套算子),
//   所以"主程序有没有接上引擎"没法用行为测。
//
//   但lint 也能干这件事 —— 只要把判据从"禁止某种写法"
//   改成"**要求某个可测入口**"(见 run_policy 那条记录):
//   mainwindow 必须调 w7::runFrame(...),
//   而且不许再出现自己写的 inputs 合并循环。
//
//   后一条是关键:引擎抽出来了,主程序却照旧自己写一遍,
//   那就等于**两份拷贝** —— 抽了个寂寞,而且两份会漂。
// ============================================================
static void test_mainwindow_uses_engine() {
    beginCase("8. 源码守卫:mainwindow 必须走引擎,不许自己再写一遍");

    const QString path = QString(W7_SOURCE_DIR) + "/mainwindow.cpp";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        check(false, "能打开 mainwindow.cpp —— 打不开说明构建时没把源码目录传进来");
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());

    check(src.contains("w7::runFrame("),
          "mainwindow.cpp 走的是 w7::runFrame(...) —— "
          "自己再写一遍执行逻辑就等于抄副本");
    check(!src.contains("inputs[it.key()] = it.value()"),
          "mainwindow.cpp 里没有自己写的 inputs 合并循环");
    check(src.contains("w7::topoOrder("),
          "mainwindow.cpp 的拓扑序也走 w7::topoOrder(...)");
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 流水线执行引擎 回归测试";

    test_topological_order();
    test_cycle_detection();
    test_bad_edges();
    test_merge_outputs();
    test_merge_is_order_sensitive();
    test_not_vacuous();
    test_run_frame();
    test_step_info_fields();
    test_mainwindow_uses_engine();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
