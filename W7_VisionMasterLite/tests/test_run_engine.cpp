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

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 流水线执行引擎 回归测试";

    test_topological_order();
    test_cycle_detection();
    test_bad_edges();
    test_merge_outputs();
    test_merge_is_order_sensitive();
    test_not_vacuous();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
