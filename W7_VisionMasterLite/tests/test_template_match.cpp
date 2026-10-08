// ============================================================
// W7 template_match 回归测试
//
// 这个算子此前**一个测试都没有**,而它是判断类算子里最容易"安静地算错"的一个:
// 它不崩、不报错,只是给你一个错的 pass/FAIL。
//
// 三个缺陷共同的形状是"把某种方法的语义当成了所有方法的语义":
//
//  1) TM_SQDIFF_NORMED 的值方向和另外两种相反(越小越像,不是越大越像),
//     而代码对它取 -minVal,再和 [0,1] 区间的 threshold(默认 0.9)比 ——
//     于是恒判 FAIL。产线表现:"模板明明在图里,却一直判 NG"。
//
//  2) 模板比原图大时,cv::matchTemplate 内部 CV_Assert 失败会
//     **throw cv::Exception**。Run 引擎的调用点没有 try/catch,
//     异常穿出槽函数 = 整个进程终止。这是最常见的误操作。
//
//  3) CCOEFF 系列要求模板自身有方差。纯色模板方差为 0,
//     OpenCV 返回 NaN,而和 NaN 的任何比较恒为 false ——
//     pass 恒假、score 显示成 nan,同样查不出原因。
//
// 三条都"跑完了",日志也正常,状态点是绿的 —— 只有结论是错的。
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <windows.h>   // SetConsoleOutputCP
#include <opencv2/opencv.hpp>

#include "operators/param_form.h"
#include "operators/template_match.h"

using namespace w7;

// ============================================================
// 极简断言(与其余 10 个测试同一套风格)
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

// ------------------------------------------------------------
// 造测试图。
//
// 关键:模板图**必须有纹理**(不能是纯色),否则用 CCOEFF 会得到 NaN,
// 测试就变成在测另一个 bug 了。这里用有噪声的方块图 ——
// 噪声保证每个像素都有变化,方差非 0。
// ------------------------------------------------------------
static cv::Mat texturedPatch(int w, int h, cv::Scalar base, unsigned seed) {
    cv::Mat m(h, w, CV_8UC3);
    cv::RNG rng(seed);
    rng.fill(m, cv::RNG::UNIFORM, 0, 256);
    // 混一点底色,免得全是纯噪声导致各通道都接近均值
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            m.at<cv::Vec3b>(y, x) =
                cv::Vec3b(cv::saturate_cast<uchar>(m.at<cv::Vec3b>(y, x)[0] * 0.4 + base[0] * 0.6),
                          cv::saturate_cast<uchar>(m.at<cv::Vec3b>(y, x)[1] * 0.4 + base[1] * 0.6),
                          cv::saturate_cast<uchar>(m.at<cv::Vec3b>(y, x)[2] * 0.4 + base[2] * 0.6));
    return m;
}

// 把一张有纹理的 patch 贴到大图背景上,再把大图和 patch 各自落盘
static void makeScene(const QString& imgPath, const QString& tplPath,
                      cv::Point at, int sceneW = 200, int sceneH = 200) {
    cv::RNG rng(12345);
    cv::Mat scene(sceneH, sceneW, CV_8UC3);
    rng.fill(scene, cv::RNG::UNIFORM, 20, 90);          // 灰底
    const cv::Mat patch = texturedPatch(30, 30, cv::Scalar(200, 60, 60), 777);
    patch.copyTo(scene(cv::Rect(at.x, at.y, patch.cols, patch.rows)));
    cv::imwrite(imgPath.toStdString(), scene);
    cv::imwrite(tplPath.toStdString(), patch);
}

// ============================================================
// 用例 1:三种方法的方向必须统一成"1 = 最像"
//
// 这是整个套件的核心。同一个"模板确实在图里"的场景,
// 分别用下拉框里的三种方法跑,pass 必须都是 true。
//
// 只测一种方法的话,一个把方向写反的实现照样全绿 ——
// 而写反的代价是整条流水线恒判 NG。
// ============================================================
static void test_method_polarity_is_normalized() {
    beginCase("三种匹配方法的方向统一(越像越大)");

    const QString img = "images/w7_tm_scene.png";
    const QString tpl = "images/w7_tm_tpl.png";
    makeScene(img, tpl, cv::Point(70, 50));

    TemplateMatch op;
    struct { int method; const char* name; } methods[] = {
        {cv::TM_CCOEFF_NORMED, "TM_CCOEFF_NORMED"},
        {cv::TM_CCORR_NORMED,  "TM_CCORR_NORMED"},
        {cv::TM_SQDIFF_NORMED, "TM_SQDIFF_NORMED"},
    };

    for (const auto& m : methods) {
        const QJsonObject out = op.run(
            QJsonObject{{"template_path", tpl}, {"method", m.method},
                        {"threshold", 0.9}},
            QJsonObject{{"image_path", img}});
        check(!out.contains("error"),
              QString("%1 不报错(实际 \"%2\")")
                .arg(m.name, out.value("error").toString()));
        check(out.value("score").toDouble() > 0.0,
              QString("%1 的 score 为正(实际 %2)")
                .arg(m.name).arg(out.value("score").toDouble()));
        check(out.value("pass").toBool(),
              QString("%1 模板确实在图里 -> 判 pass(实际 score=%2 pass=%3)")
                .arg(m.name).arg(out.value("score").toDouble())
                .arg(out.value("pass").toBool()));
    }
}

// ============================================================
// 用例 2:三种方法必须真的都在起作用
//
// 上面那条只验"都对"。这条验"不是碰巧都对":每种方法报的位置
// 都要落在真实的贴图位置上(±2px)。
//
// 少了这条,一个"不管什么方法都返回同一个写死结果"的实现
// 也能把用例 1 骗过去 —— 而那正是静默算错的极端形态。
// ============================================================
static void test_position_is_correct() {
    beginCase("匹配位置落在真实贴图位置");

    const QString img = "images/w7_tm_scene.png";
    const QString tpl = "images/w7_tm_tpl.png";
    const cv::Point truth(70, 50);

    TemplateMatch op;
    struct { int method; const char* name; } methods[] = {
        {cv::TM_CCOEFF_NORMED, "TM_CCOEFF_NORMED"},
        {cv::TM_CCORR_NORMED,  "TM_CCORR_NORMED"},
        {cv::TM_SQDIFF_NORMED, "TM_SQDIFF_NORMED"},
    };
    for (const auto& m : methods) {
        const QJsonObject out = op.run(
            QJsonObject{{"template_path", tpl}, {"method", m.method},
                        {"threshold", 0.5}},
            QJsonObject{{"image_path", img}});
        const int dx = out.value("position_x").toInt() - truth.x;
        const int dy = out.value("position_y").toInt() - truth.y;
        check(std::abs(dx) <= 2 && std::abs(dy) <= 2,
              QString("%1 位置=(%2,%3),真值=(%4,%5),偏差应 ≤2")
                .arg(m.name).arg(out.value("position_x").toInt())
                .arg(out.value("position_y").toInt())
                .arg(truth.x).arg(truth.y));
    }
}

// ============================================================
// 用例 3:阈值真的在控(反向对照)
//
// 把阈值调到 0.99 以上,一个"完美匹配也不该过"的阈值,
// 用来确认 pass 不是恒真的。
//
// 只验"该过的时候过"的话,一个写死 pass=true 的实现能骗过用例 1。
// ============================================================
static void test_threshold_actually_controls() {
    beginCase("阈值真的在控(不是恒真)");

    const QString img = "images/w7_tm_scene.png";
    const QString tpl = "images/w7_tm_tpl.png";
    TemplateMatch op;

    const QJsonObject out = op.run(
        QJsonObject{{"template_path", tpl},
                    {"method", cv::TM_SQDIFF_NORMED}, {"threshold", 1.01}},
        QJsonObject{{"image_path", img}});
    check(!out.value("pass").toBool(),
          QString("阈值 1.01 高于任何归一化相似度 -> 判不通过(实际 score=%1)")
            .arg(out.value("score").toDouble()));
}

// ============================================================
// 用例 4:模板比原图大 -> 报错,而不是把程序打崩
//
// cv::matchTemplate 内部 CV_Assert(image.size() >= templ.size())
// 不满足就 throw cv::Exception。Run 引擎调用点没有 try/catch,
// 异常穿出槽函数 = 进程终止。
//
// 这条曾经直接 0xC0000005 / "cv::Exception 未能捕获" ——
// 而且"选一张比图大的模板图"是最顺手的误操作。
// ============================================================
static void test_template_bigger_than_scene() {
    beginCase("模板比原图大 -> 报错而不是崩");

    const QString img = "images/w7_tm_scene.png";
    const QString smallTpl = "images/w7_tm_tpl_small.png";
    makeScene(img, smallTpl, cv::Point(0, 0), 40, 40);   // 原图 40x40

    // 造一张 80x80 的模板
    cv::imwrite(smallTpl.toStdString(),
                texturedPatch(80, 80, cv::Scalar(200, 60, 60), 42));

    TemplateMatch op;
    const QJsonObject out = op.run(
        QJsonObject{{"template_path", smallTpl},
                    {"method", cv::TM_CCOEFF_NORMED}, {"threshold", 0.9}},
        QJsonObject{{"image_path", img}});

    check(out.contains("error"),
          QString("模板 80x80 > 原图 40x40 -> 报 error(实际 error=\"%1\")")
            .arg(out.value("error").toString()));
    check(!out.contains("pass"),
          "报错时不产出 pass(下游不能把缺字段当成\"判了但没过\")");

    // 反过来:模板比原图小是正常用法,必须照常工作。
    // 没有这条,"永远报错"也能骗过上面两条。
    const QString bigImg = "images/w7_tm_scene_big.png";
    const QString small  = "images/w7_tm_tpl_ok.png";
    cv::imwrite(small.toStdString(), texturedPatch(20, 20, cv::Scalar(10, 200, 10), 9));
    cv::Mat scene = cv::Mat::zeros(100, 100, CV_8UC3);
    cv::imwrite(bigImg.toStdString(), scene);
    const QJsonObject ok = op.run(
        QJsonObject{{"template_path", small},
                    {"method", cv::TM_CCOEFF_NORMED}, {"threshold", 0.5}},
        QJsonObject{{"image_path", bigImg}});
    check(!ok.contains("error"),
          QString("对照组:模板比原图小 -> 正常跑(实际 \"%1\")")
            .arg(ok.value("error").toString()));

    QFile::remove(smallTpl);
    QFile::remove(bigImg);
    QFile::remove(small);
}

// ============================================================
// 用例 5:非法 method 值 -> 报错,而不是抛 cv::Exception
//
// method 是从 JSON 来的(工程文件手可编辑),6 以外的值
// (比如有人按 TM_SQDIFF=0 / TM_CCOEFF=4 的顺序猜错了编号)
// 会让 matchTemplate 的 switch 落到 default 然后 assert 失败。
// ============================================================
static void test_invalid_method_rejected() {
    beginCase("非法 method 值被拦下");

    const QString img = "images/w7_tm_scene.png";
    const QString tpl = "images/w7_tm_tpl.png";
    TemplateMatch op;

    for (int bad : {-1, 6, 99}) {
        const QJsonObject out = op.run(
            QJsonObject{{"template_path", tpl}, {"method", bad},
                        {"threshold", 0.9}},
            QJsonObject{{"image_path", img}});
        check(out.contains("error"),
              QString("method=%1 -> 报 error 而不是抛异常").arg(bad));
    }
}

// ============================================================
// 用例 6:纯色模板 -> 报错,而不是恒判合格
//
// CCOEFF 系列要求模板自身方差非 0。
//
// 原本我以为纯色模板会给 NaN、于是 pass 恒假 —— **实测不是**。
// 在本机的 OpenCV 4.x 上,一整块纯色模板给出的是:
//     score = 1.000000   pass = true
// 也就是**恒判合格**:换任何一张图、任何位置,都"匹配成功"。
//
// 对检测算子来说这是最坏的方向:判不合格会有人去查参数,
// 判合格没人查 —— 板子再差也能过,直接流到下一道工序。
//
// 而且 OpenCV 在退化输入上到底返回 NaN、1.0 还是别的,依版本而异
// (就是同一个 0/0,不同实现路径处理不一样)。所以修法不能是
// "跑完检查一下返回值",必须**在调用之前自己算模板方差**。
//
// 界面上拿一张纯色图当模板是很常见的操作(截图没对齐、选错了文件)。
// ============================================================
static void test_uniform_template_reported() {
    beginCase("纯色模板 -> 报错,而不是恒判合格");

    const QString img   = "images/w7_tm_scene.png";
    const QString blank = "images/w7_tm_flat.png";
    cv::imwrite(blank.toStdString(),
                cv::Mat(30, 30, CV_8UC3, cv::Scalar(128, 128, 128)));

    TemplateMatch op;
    const QJsonObject out = op.run(
        QJsonObject{{"template_path", blank},
                    {"method", cv::TM_CCOEFF_NORMED}, {"threshold", 0.9}},
        QJsonObject{{"image_path", img}});

    check(out.contains("error"),
          QString("纯色模板 + CCOEFF -> 报 error(实际 error=\"%1\")")
            .arg(out.value("error").toString()));
    check(out.value("error").toString().contains("纯色") ||
              out.value("error").toString().contains("标准差"),
          "报错要说清是模板没纹理(用户不看代码也知道该换模板)");
    check(!out.contains("pass"),
          QString("纯色模板不产出 pass(实际 pass=%1)")
            .arg(out.value("pass").toBool()));

    // 再钉一次反向:固色模板**必须**恒判合格,否则这条断言是虚的。
    // 这条也是整个用例 6 的"对照组" —— 一个总是报错的实现也能骗过上面三条。
    {
        // 一张和模板毫无关系的纯色场景图:匹配不了任何东西
        const QString other = "images/w7_tm_other.png";
        cv::imwrite(other.toStdString(),
                    cv::Mat(120, 120, CV_8UC3, cv::Scalar(10, 200, 10)));
        const QJsonObject junk = op.run(
            QJsonObject{{"template_path", blank},
                        {"method", cv::TM_CCOEFF_NORMED}, {"threshold", 0.9}},
            QJsonObject{{"image_path", other}});
        // 关键:绝不能是 pass。改动前这里正是 pass=true。
        check(!junk.value("pass").toBool(),
              QString("纯色模板对任何图都不能判合格(实际 score=%1 pass=%2)")
                .arg(junk.value("score").toDouble())
                .arg(junk.value("pass").toBool()));
        QFile::remove(other);
    }

    // 换一种不挑方差的方法就该能跑 —— 证明是模板的问题,不是环境的问题
    const QJsonObject ok = op.run(
        QJsonObject{{"template_path", blank},
                    {"method", cv::TM_CCORR_NORMED}, {"threshold", 0.5}},
        QJsonObject{{"image_path", img}});
    check(!ok.contains("error"),
          QString("对照组:同一张纯色模板换 TM_CCORR_NORMED 就能跑"
                  "(实际 \"%1\")").arg(ok.value("error").toString()));

    QFile::remove(blank);
}

// ============================================================
// 用例 7:图 / 模板读不出来
// ============================================================
static void test_missing_files() {
    beginCase("图或模板不存在 -> 报错");
    TemplateMatch op;

    {
        const QJsonObject out = op.run(
            QJsonObject{{"template_path", "images/w7_tm_tpl.png"},
                        {"method", cv::TM_CCOEFF_NORMED}},
            QJsonObject{{"image_path", "images/不存在.png"}});
        check(out.contains("error"), "原图不存在 -> 报 error");
        check(!out.contains("pass"), "报错时不产出 pass");
    }
    {
        const QJsonObject out = op.run(
            QJsonObject{{"template_path", "images/不存在.png"},
                        {"method", cv::TM_CCOEFF_NORMED}},
            QJsonObject{{"image_path", "images/w7_tm_scene.png"}});
        check(out.contains("error"), "模板不存在 -> 报 error");
    }
    {
        // template_path 优先取 inputs(引擎往下传的),再回落 params
        const QJsonObject out = op.run(QJsonObject{},
            QJsonObject{{"image_path", "images/w7_tm_scene.png"},
                        {"template_path", "images/w7_tm_tpl.png"}});
        check(!out.contains("error"),
              QString("inputs 里给 template_path 时可用(实际 \"%1\")")
                .arg(out.value("error").toString()));
    }
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);
    QDir().mkpath("images");

    qInfo().noquote() << "=== W7 template_match 回归测试 ===";

    test_method_polarity_is_normalized();
    test_position_is_correct();
    test_threshold_actually_controls();
    test_template_bigger_than_scene();
    test_invalid_method_rejected();
    test_uniform_template_reported();
    test_missing_files();

    QFile::remove("images/w7_tm_scene.png");
    QFile::remove("images/w7_tm_scene_big.png");
    QFile::remove("images/w7_tm_tpl.png");
    QFile::remove("images/w7_tm_tpl_small.png");

    qInfo().noquote() << "\n========================================";
    qInfo().noquote() << QString("pass=%1  fail=%2").arg(g_pass).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}
