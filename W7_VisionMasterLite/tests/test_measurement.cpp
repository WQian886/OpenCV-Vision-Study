// ============================================================
// W7 测量类算子(circle_fit + caliper_dist)— 无头回归测试
//
// 这两个和 line_fit 是一组的,共用一条设计原则:
//   **公式越简单,越要测"约定"和"退化情形",而不是测算术。**
//
// circle_fit 真正的坑:
//   · 圆度 4π·area/perim² —— 分子分母写反、漏 4π、忘记平方,
//     算出来是 0.25 或者 0.001,滤不掉任何东西还"看起来在工作"
//   · 周长为 0 的退化轮廓 → 除零
// caliper_dist 真正的坑:
//   · 距离公式是叉积/模,不是投影;分母是 baseLen
//   · A、B 重合 → 基准线无定义
//
// 运行:  w7_measure_test.exe   (退出码 0 = 全过)
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QFile>        // QFile::remove —— 删掉自造的临时 mask
#include <QJsonArray>   // 用 circles.toArray().first() 取第一个圆
#include <cmath>
#include <windows.h>   // SetConsoleOutputCP
#include <opencv2/imgproc.hpp>

#include "operators/circle_fit.h"
#include "operators/caliper_dist.h"

using namespace w7;

// ============================================================
// 极简断言(与其余 6 个测试同一套风格)
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

// 造一张"白底黑圆"的 mask,写到临时文件给算子读
// 参数:半径、圆心、画布边长
static QString makeCircleMask(int radius, int cx, int cy, int size,
                              const QString& file) {
    cv::Mat m(size, size, CV_8UC1, cv::Scalar(0));
    cv::circle(m, cv::Point(cx, cy), radius, cv::Scalar(255), -1);
    cv::imwrite(file.toStdString(), m);
    return file;
}

// 删临时文件。QFile::remove 收 QString,不收 std::string ——
// 直接把 .toStdString() 传进去会报"没有重载函数可以转换所有参数类型"
static void dropFile(const QString& f) { QFile::remove(f); }

static QJsonObject circleParams(int minArea, double minCirc) {
    return QJsonObject{{"min_area", minArea}, {"min_circularity", minCirc}};
}

// ------------------------------------------------------------
// 用例 1:量一个已知的圆
// ------------------------------------------------------------
static void test_circle_basic() {
    beginCase("圆形测量:已知圆");
    CircleFit op;

    // r=20 的正圆,圆心 (50,50)
    const QString f = makeCircleMask(20, 50, 50, 120, "images/_t_circle.png");
    QJsonObject r = op.run(circleParams(10, 0.3),
                           QJsonObject{{"mask_path", f}});

    check(!r.contains("error"), "正常 mask 不报错");
    check(r.value("count").toInt() == 1,
          QString("找到 1 个圆 (实际 %1)").arg(r.value("count").toInt()));

    const QJsonObject c = r.value("circles").toArray().first().toObject();
    checkNear(c.value("cx").toDouble(), 50.0, 1.5, "圆心 x ≈ 50");
    checkNear(c.value("cy").toDouble(), 50.0, 1.5, "圆心 y ≈ 50");
    checkNear(c.value("radius").toDouble(), 20.0, 1.5, "半径 ≈ 20");
    checkNear(c.value("diameter").toDouble(), 40.0, 3.0, "直径 ≈ 40");

    // 圆度:正圆理论值是 1.0,但离散像素画出来的圆会略低
    // (面积和周长都被光栅化"啃掉"一圈),0.8 以上就算很圆了
    const double circ = c.value("circularity").toDouble();
    check(circ > 0.8 && circ <= 1.0001,
          QString("正圆圆度落在 (0.8, 1.0] (实际 %1)").arg(circ));

    QFile::remove(f);
}

// ------------------------------------------------------------
// 用例 2:圆度阈值真的会滤东西吗
//
// 这是 circle_fit 最值钱的一条测试。
// 圆度公式写错(漏 4π / 忘记平方 / 分子分母写反)都不报错,
// 只是算出 0.25 或 0.001 这种值 —— 于是**滤不掉任何东西**,
// 方块也被当圆报出来。所以必须验证"阈值真的在起作用"。
// ------------------------------------------------------------
static void test_circle_circularity_filter() {
    beginCase("圆度阈值真的在滤东西");
    CircleFit op;

    // 画一个方块:r=20 的方块,面积和圆差不多,但圆度低
    cv::Mat m(120, 120, CV_8UC1, cv::Scalar(0));
    cv::rectangle(m, cv::Rect(35, 35, 40, 40), cv::Scalar(255), -1);
    const QString f = "images/_t_square.png";
    cv::imwrite(f.toStdString(), m);

    // 阈值放到 0.9:方块圆度远低于这个值,应该被滤掉
    QJsonObject r = op.run(circleParams(10, 0.9), QJsonObject{{"mask_path", f}});
    check(r.value("count").toInt() == 0,
          QString("方块被 0.9 阈值滤掉 (实际找到 %1 个)")
              .arg(r.value("count").toInt()));
    check(r.contains("warning"), "没找到圆时给 warning 而不是 error");
    check(!r.contains("error"),
          "滤掉全部不算 error —— 用户可能只是阈值调高了");

    // 阈值放到 0.1:方块圆度高于这个值,应该被留下
    r = op.run(circleParams(10, 0.1), QJsonObject{{"mask_path", f}});
    check(r.value("count").toInt() == 1,
          QString("阈值放到 0.1 时方块被留下 (实际 %1 个)")
              .arg(r.value("count").toInt()));

    QFile::remove(f);
}

// ------------------------------------------------------------
// 用例 3:退化输入 —— 面积太小 / 图读不到
// ------------------------------------------------------------
static void test_circle_degenerate() {
    beginCase("圆形测量:退化输入");
    CircleFit op;

    // 面积阈值卡死,什么都找不到
    const QString f = makeCircleMask(20, 50, 50, 120, "images/_t_c2.png");
    QJsonObject r = op.run(circleParams(999999, 0.3), QJsonObject{{"mask_path", f}});
    check(r.value("count").toInt() == 0, "面积阈值过大时找到 0 个");
    check(!r.contains("error"), "阈值过大不是 error");
    QFile::remove(f);

    // 路径读不到
    r = op.run(circleParams(10, 0.3),
               QJsonObject{{"mask_path", "images/_不存在.png"}});
    check(r.contains("error"), "路径读不到 → error");

    // 两个路径都没有
    r = op.run(circleParams(10, 0.3), QJsonObject());
    check(r.contains("error"), "mask_path 和 image_path 都没有 → error");

    // 退化轮廓不能除零:一个孤立像素点
    cv::Mat tiny(20, 20, CV_8UC1, cv::Scalar(0));
    tiny.at<uchar>(10, 10) = 255;
    const QString f2 = "images/_t_dot.png";
    cv::imwrite(f2.toStdString(), tiny);
    r = op.run(circleParams(1, 0.0), QJsonObject{{"mask_path", f2}});
    check(!r.contains("error"), "孤立像素点不导致除零崩溃");
    QFile::remove(f2);
}

// ============================================================
// === caliper_dist ===
// ============================================================

static QJsonObject cal(int ax, int ay, int bx, int by, int cx, int cy) {
    return QJsonObject{{"ax", ax}, {"ay", ay}, {"bx", bx}, {"by", by},
                       {"cx", cx}, {"cy", cy}};
}

// ------------------------------------------------------------
// 用例 4:点到线距离的基本几何
// ------------------------------------------------------------
static void test_caliper_basic() {
    beginCase("卡尺:基本几何");
    CaliperDist op;

    // 水平基准线,C 在正上方 3 格 → 距离 3
    QJsonObject r = op.run(cal(0, 0, 10, 0, 5, 3), QJsonObject());
    checkNear(r.value("distance").toDouble(), 3.0, 1e-9, "水平基准距离 = 3");
    checkNear(r.value("base_length").toDouble(), 10.0, 1e-9, "基准线长 = 10");
    checkNear(r.value("foot_x").toDouble(), 5.0, 1e-9, "垂足 x = 5");
    checkNear(r.value("foot_y").toDouble(), 0.0, 1e-9, "垂足落在基准线上");
    checkNear(r.value("ratio").toDouble(), 0.5, 1e-9, "垂足比例 = 0.5");

    // 垂直基准线,C 在右边 4 格 → 距离 4
    r = op.run(cal(0, 0, 0, 10, 4, 5), QJsonObject());
    checkNear(r.value("distance").toDouble(), 4.0, 1e-9, "垂直基准距离 = 4");

    // 斜基准线 3-4-5,C 沿垂线方向偏 6 → 距离 6
    // A=(0,0) B=(3,4) 方向 (3,4)/5,垂线方向 (-4,3)/5
    // C = (0,0) + 6*(-4,3)/5 = (-4.8, 3.6) → 存成整数 (-5,4) 有误差,
    // 所以用能整除的:基准线 (5,0),C=(5,6) 距离 6
    r = op.run(cal(0, 0, 5, 0, 5, 6), QJsonObject());
    checkNear(r.value("distance").toDouble(), 6.0, 1e-9, "C 在端点正上方距离 = 6");

    // **关键**:测的是垂直距离,不是到 A 或 B 的直线距离
    // A=(0,0) B=(10,0) C=(1,1) —— 到 A 的直线距离是 √2≈1.414,
    // 但垂直距离是 1。搞混这两个是最常见的错。
    r = op.run(cal(0, 0, 10, 0, 1, 1), QJsonObject());
    checkNear(r.value("distance").toDouble(), 1.0, 1e-9,
              "量的是垂直距离而非 √2 直线距离");

    // 负侧:C 在基准线下方,距离仍为正(距离没有方向)
    r = op.run(cal(0, 0, 10, 0, 5, -7), QJsonObject());
    checkNear(r.value("distance").toDouble(), 7.0, 1e-9, "C 在下方距离仍为正 7");
}

// ------------------------------------------------------------
// 用例 5:退化输入
// ------------------------------------------------------------
static void test_caliper_degenerate() {
    beginCase("卡尺:退化输入");
    CaliperDist op;

    // A、B 重合 → 基准线无定义
    // A=(10,10) C=(30,40) → A→C = √(20² + 30²) = √1300 ≈ 36.0555
    QJsonObject r = op.run(cal(10, 10, 10, 10, 30, 40), QJsonObject());
    checkNear(r.value("base_length").toDouble(), 0.0, 1e-9, "基准线长 = 0");
    check(r.contains("warning"), "A、B 重合给 warning");
    check(!r.contains("error"), "A、B 重合不是 error —— 用户还没拉开基准");
    checkNear(r.value("distance").toDouble(), std::sqrt(1300.0), 1e-9,
              "退化成直接量 A→C = √1300");

    // 没有图照样能测(纯几何)
    r = op.run(cal(0, 0, 10, 0, 5, 3), QJsonObject{{"image_path", ""}});
    checkNear(r.value("distance").toDouble(), 3.0, 1e-9, "没图也能量");

    // 给了图但读不到
    r = op.run(cal(0, 0, 10, 0, 5, 3),
               QJsonObject{{"image_path", "images/_不存在.png"}});
    check(r.contains("error"), "给了路径但读不到 → error");
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);

    qInfo().noquote() << "W7 测量类算子(circle_fit + caliper_dist) 回归测试";

    test_circle_basic();
    test_circle_circularity_filter();
    test_circle_degenerate();
    test_caliper_basic();
    test_caliper_degenerate();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
