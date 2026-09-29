// ============================================================
// W7 line_fit(直线测量)— 无头回归测试
//
// 为什么这个算子值得测:
//   它是纯几何,几行公式,看着"不可能错"。但真正容易错的不是公式,
//   是**约定**:
//     · 角度报 0~180 还是 0~360?(本项目定的是 0~180,无向直线)
//     · 两个点重合时角度该报多少?
//     · 没给图的时候该不该报错?
//   这些都不崩,只是结果悄悄变了 —— 而测量结果直接决定判废,
//   错了要到产线上才发现。
//
// 所以这个测试的主要作用不是验算术,是**把上面这些决策钉住**。
// 哪天"顺手优化"了角度归一化,这里会先炸。
//
// 运行:  w7_line_test.exe   (退出码 0 = 全过)
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <cmath>
#include <windows.h>   // SetConsoleOutputCP

#include "operators/line_fit.h"

using namespace w7;

// ============================================================
// 极简断言(与其余 5 个测试同一套风格)
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
static void checkNear(double actual, double expected, double tol,
                      const QString& what) {
    check(std::fabs(actual - expected) <= tol,
          QString("%1 (期望 %2 ±%3,实际 %4)")
              .arg(what).arg(expected).arg(tol).arg(actual));
}

// 造一组参数,免得每个用例都写 4 行
static QJsonObject P(int x1, int y1, int x2, int y2) {
    return QJsonObject{{"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}};
}

// ------------------------------------------------------------
// 用例 1:基本几何 —— 三条一定成立的关系
// 这一组不依赖任何图片文件,是纯数学
// ------------------------------------------------------------
static void test_basic_geometry() {
    beginCase("基本几何");
    LineFit op;

    // 水平线
    QJsonObject r = op.run(P(0, 0, 10, 0), QJsonObject());
    checkNear(r.value("length").toDouble(),    10.0, 1e-9, "水平线长度 = 10");
    checkNear(r.value("angle_deg").toDouble(),  0.0, 1e-9, "水平线角度 = 0°");
    check(!r.contains("error"), "没有图也没有 error(纯几何不该依赖图)");

    // 垂直线
    r = op.run(P(0, 0, 0, 10), QJsonObject());
    checkNear(r.value("length").toDouble(),     10.0, 1e-9, "垂直线长度 = 10");
    checkNear(r.value("angle_deg").toDouble(),  90.0, 1e-9, "垂直线角度 = 90°");

    // 斜边 3-4-5
    r = op.run(P(0, 0, 3, 4), QJsonObject());
    checkNear(r.value("length").toDouble(),      5.0, 1e-9, "3-4-5 斜边长度 = 5");
    checkNear(r.value("angle_deg").toDouble(), 53.13, 0.01, "3-4-5 斜边角度 ≈ 53.13°");

    // 端点回显:下游要用
    r = op.run(P(10, 20, 30, 40), QJsonObject());
    check(r.value("p1").toObject().value("x").toInt() == 10
          && r.value("p1").toObject().value("y").toInt() == 20,
          "p1 回显正确");
    checkNear(r.value("mid").toObject().value("x").toDouble(), 20.0, 1e-9,
              "中点 x = 20");
    checkNear(r.value("mid").toObject().value("y").toDouble(), 30.0, 1e-9,
              "中点 y = 30");
}

// ------------------------------------------------------------
// 用例 2:角度归一化 —— **这个用例钉的是设计决策**
//
// 本项目定的是 0~180(无向直线)。
// 如果哪天有人"优化"成报有向角(0~360),这里会先红,
// 提醒他这是有意为之,不是 bug。
// ------------------------------------------------------------
static void test_angle_normalization() {
    beginCase("角度归一化(0~180 无向)");
    LineFit op;

    // 同一条线的两个点序,角度必须一样 —— 这就是"无向"的定义
    const QJsonObject a = op.run(P(0, 0, 10, 0), QJsonObject());
    const QJsonObject b = op.run(P(10, 0, 0, 0), QJsonObject());
    checkNear(a.value("angle_deg").toDouble(), b.value("angle_deg").toDouble(),
              1e-9, "水平线正反点序角度一致");

    // 恰好 180°:atan2 会返回 180,必须归到 0(和 0° 是同一条线)
    checkNear(op.run(P(0, 0, -10, 0), QJsonObject())
                  .value("angle_deg").toDouble(),
              0.0, 1e-9, "atan2 返回 180 时归一到 0");

    // 负角度:atan2 的 (-180,0) 段要 +180
    const double downRight =
        op.run(P(0, 0, 10, -10), QJsonObject()).value("angle_deg").toDouble();
    checkNear(downRight, 135.0, 0.01, "右下斜线(-45°)报 135° 而不是 -45°");

    // 全部结果都必须在 [0, 180)
    const QJsonObject probes[] = {
        P(0, 0, 1, 1), P(0, 0, -1, 1), P(0, 0, 1, -1),
        P(0, 0, -1, -1), P(0, 0, -1, 0), P(0, 0, 0, -1),
    };
    bool allInRange = true;
    for (const QJsonObject& p : probes) {
        const double ang = op.run(p, QJsonObject()).value("angle_deg").toDouble();
        if (ang < 0.0 || ang >= 180.0) allInRange = false;
    }
    check(allInRange, "6 个方向的探测值全部落在 [0,180)");
}

// ------------------------------------------------------------
// 用例 3:退化输入
//
// 这一条是本项目的铁律:**算子不能让流水线崩**。
// 用户拖滑块时两个点一样多是常态,不是异常。
// ------------------------------------------------------------
static void test_degenerate() {
    beginCase("退化输入");
    LineFit op;

    // 两点重合
    QJsonObject r = op.run(P(50, 50, 50, 50), QJsonObject());
    checkNear(r.value("length").toDouble(), 0.0, 1e-9, "重合点长度 = 0");
    checkNear(r.value("angle_deg").toDouble(), 0.0, 1e-9, "重合点角度 = 0");
    check(r.contains("warning"), "重合点给 warning");
    check(!r.contains("error"),
          "重合点**不是** error —— 用户还没拉开距离,流水线该照常跑完");

    // 负坐标(相机装反了 / 图像没平移到位时会遇到)
    r = op.run(P(-10, -10, 10, 10), QJsonObject());
    check(!r.contains("error"), "负坐标不崩");
    checkNear(r.value("length").toDouble(), std::sqrt(800.0), 1e-9,
              "负坐标长度算对");
}

// ------------------------------------------------------------
// 用例 4:图的处理 —— "没给图"和"给了但读不到"是两回事
// ------------------------------------------------------------
static void test_image_handling() {
    beginCase("图的处理");
    LineFit op;

    // 没给图:照样量,只是没标注图
    QJsonObject r = op.run(P(0, 0, 10, 0), QJsonObject{{"image_path", ""}});
    check(!r.contains("error"), "image_path 为空不报错");
    check(r.value("length").toDouble() > 9.99, "没图也能量出长度");

    // 给了路径但读不到:这是真问题(路径写错/文件被删),该报 error
    r = op.run(P(0, 0, 10, 0),
               QJsonObject{{"image_path", "images/_不存在的图.png"}});
    check(r.contains("error"), "给了路径但读不到 → 报 error");
    check(r.value("error").toString().contains("imread"),
          "error 里说清了是 imread 的问题");
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);

    qInfo().noquote() << "W7 line_fit(直线测量) 回归测试";

    test_basic_geometry();
    test_angle_normalization();
    test_degenerate();
    test_image_handling();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
