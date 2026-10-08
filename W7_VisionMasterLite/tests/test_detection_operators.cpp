// ============================================================
// W7 检测类算子 回归测试 (color_segment / blob_count / count_check)
//
// ============================================================
// 为什么先补这三个:
//
// 它们是**启动时你看到的那条默认流水线**
// (image_source_dir → color_segment → blob_count → count_check,
//  见 MainWindow::loadDefaultPipeline)上的三个算子,
// 而 9 个套件里没有一个测过它们 —— 前 8 个套件测的是画布、
// 撤销重做、几何测量、图像源,恰好绕开了这条链。
//
// 这三个算子有两个共同点,决定了这层的 bug 特别难靠手点发现:
//
// 1) **参数是从 JSON 来的,形状不保证**。工程文件是手可编辑的、
//    也可能是旧版本存的(字段名变了/字段没了)。
//    run() 里直接 lo[0].toInt() 这种写法,拿到短数组就是越界。
//    而同一个类的表单里却写了 `if (lo.size() >= 3)` —— 说明作者
//    知道数组可能短,只是漏了 run() 这一处。
//    越界在 Release 下不报错,可能直接给出一个"看起来正常"的假阈值。
//
// 2) **中间结果靠落盘的文件名传递**。Run 引擎只传字符串路径,
//    所以每个算子把结果写进一个 png,下游再 imread 回来。
//    代价是:文件名必须每个实例唯一,否则两个同类型算子互相覆盖,
//    下游读到的是**别人的结果** —— 不崩、不报错,数出来的数就是错的。
//    这类 bug 在产线上的后果比崩溃严重得多。
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <windows.h>
#include <opencv2/opencv.hpp>

#include "operators/param_form.h"
#include "operators/color_segment.h"
#include "operators/blob_count.h"
#include "operators/count_check.h"

using namespace w7;

// ============================================================
// 极简断言(与其余 9 个测试同一套风格)
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
// 造一张纯色方块图,当输入。不用 W6 的样本图:
// 这个套件要验的是"参数形状不对时怎么办",和图内容无关,
// 自己造图才能保证每次跑的输入完全一致。
// ------------------------------------------------------------
static QString makeSolidImage(const QString& path, cv::Scalar bgr, int size = 100) {
    cv::Mat img(size, size, CV_8UC3, bgr);
    cv::imwrite(path.toStdString(), img);
    return path;
}

// 半个图是给定颜色、另一半是黑的。造 mask 用:
// 两个 mask 的**像素数必须不同**,否则"文件被覆盖"这种错看不出来 ——
// 内容一样的文件被互相覆盖,结果照样是对的。
static QString makeHalfImage(const QString& path, cv::Scalar bgr, int size = 100) {
    cv::Mat img(size, size, CV_8UC3, cv::Scalar(0, 0, 0));
    img(cv::Rect(0, 0, size, size / 2)).setTo(bgr);
    cv::imwrite(path.toStdString(), img);
    return path;
}

// ============================================================
// 用例 1:color_segment 的 HSV 数组残缺时不能崩
//
// run() 原来是这么写的:
//     QJsonArray lo = params.value("hsv_low").toArray();
//     cv::Scalar low(lo[0].toInt(), lo[1].toInt(), lo[2].toInt());
// lo 是空的/只有 1 个元素时,lo[2] 就是越界。
// 触发路径真实存在:旧版本存的 .sol.json、手改过的工程文件、
// 或者 node->params() 非空但缺 hsv_low(引擎只在 params **完全为空**
// 时才替换默认值,非空但缺字段照样直接进 run)。
// ============================================================
static void test_color_segment_bad_params() {
    beginCase("color_segment:HSV 数组残缺不崩,报人话");

    const QString img = "images/w7_test_solid.png";
    makeSolidImage(img, cv::Scalar(0, 0, 255));   // 红色(BGR)

    ColorSegment op;

    // (a) 完全没有 hsv_low / hsv_high
    {
        QJsonObject out = op.run(QJsonObject{}, QJsonObject{{"image_path", img}});
        check(out.contains("error"),
              QString("参数完全为空 -> 报 error(实际 error=\"%1\")")
                .arg(out.value("error").toString()));
        check(!out.contains("mask_path"), "参数不对时不产出 mask_path(别拿垃圾阈值去分割)");
    }

    // (b) hsv_low 是空数组
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{};
        p["hsv_high"] = QJsonArray{40, 255, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        check(out.contains("error"), "hsv_low 是空数组 -> 报 error");
    }

    // (c) hsv_low 只有 1 个元素(lo[1] / lo[2] 越界)
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{10};
        p["hsv_high"] = QJsonArray{40, 255, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        check(out.contains("error"),
              QString("hsv_low 只有 1 个元素 -> 报 error 而不是越界(实际 error=\"%1\")")
                .arg(out.value("error").toString()));
    }

    // (d) 两个数组长度不一致
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{10, 80, 80};
        p["hsv_high"] = QJsonArray{40, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        check(out.contains("error"), "两个数组长度不一致 -> 报 error");
    }

    // (e) 元素类型不对(该是数字,存成了字符串)
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{10, 80, "abc"};
        p["hsv_high"] = QJsonArray{40, 255, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        check(out.contains("error"),
              QString("元素是字符串 -> 报 error(实际 error=\"%1\")")
                .arg(out.value("error").toString()));
    }

    // (f) 下限 > 上限(阈值反了)。不崩,但阈值无意义 —— 分出来的必然是空
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{100, 200, 200};
        p["hsv_high"] = QJsonArray{50, 100, 100};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        // 不要求它报错(这属于"参数合法但没意义"),但必须能跑完,
        // 而且 pixel_count 必须是 0 —— 反向区间不该凭空分出像素
        check(!out.contains("error"),
              QString("下限>上限不报错,能跑完(pixel_count=%1)")
                .arg(out.value("pixel_count").toInt()));
        check(out.value("pixel_count").toInt() == 0,
              "下限>上限时分出 0 像素(不是靠越界读凑出来的数)");
    }

    // (g) 对照组:参数完全正常时必须真的能分割。
    // 没有这条,"(a)~(e) 都返回 error"可能只是因为它**什么都算不出来**,
    // 那样的测试等于自己骗自己。
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{0, 100, 100};    // 红
        p["hsv_high"] = QJsonArray{10, 255, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", img}});
        check(!out.contains("error"),
              QString("正常参数不报错(实际 error=\"%1\")")
                .arg(out.value("error").toString()));
        check(out.value("pixel_count").toInt() == 100 * 100,
              QString("整张红图应被全部分割(pixel_count=%1,期望 10000)")
                .arg(out.value("pixel_count").toInt()));
        check(!out.value("mask_path").toString().isEmpty(), "正常时产出 mask_path");
    }

    // (h) 图读不出来也要报 error,而不是拿空 Mat 继续算
    {
        QJsonObject p;
        p["hsv_low"]  = QJsonArray{0, 100, 100};
        p["hsv_high"] = QJsonArray{10, 255, 255};
        QJsonObject out = op.run(p, QJsonObject{{"image_path", "images/不存在.png"}});
        check(out.contains("error"), "图不存在 -> 报 error");
    }
}

// ============================================================
// 用例 2:两个 color_segment 节点不能互相覆盖 mask
//
// 成因:mask 落盘的文件名写死成
//     images/w7_color_segment_mask.png
// 一条流水线里放两个颜色分割(很常见:一个抓红一个抓蓝),
// 第二个把第一个的文件覆盖了,而下游 blob_count 拿着
// "images/w7_color_segment_mask.png" 这个路径 imread,
// 读到的是**第二个算子**的 mask。
//
// 结果:不崩、不报错、数出来的数就是错的。
// 对照组:同一个节点(同一个 node_uuid)重跑,路径可以相同 ——
// 那本来就该复用同一个文件。
// ============================================================
static void test_color_segment_no_clobber() {
    beginCase("color_segment:两个实例的 mask 不互相覆盖");

    const QString imgRed   = "images/w7_test_red.png";
    const QString imgBlue  = "images/w7_test_blue.png";
    makeSolidImage(imgRed,  cv::Scalar(0, 0, 255));     // 全红  -> mask 10000 px
    makeHalfImage (imgBlue, cv::Scalar(255, 0, 0));     // 半蓝  -> mask  5000 px

    ColorSegment op;

    QJsonObject pRed, pBlue;
    pRed["hsv_low"]  = QJsonArray{0, 100, 100};
    pRed["hsv_high"] = QJsonArray{10, 255, 255};
    pBlue["hsv_low"]  = QJsonArray{100, 100, 100};
    pBlue["hsv_high"] = QJsonArray{130, 255, 255};

    // 节点 A:抓红
    const QJsonObject outA = op.run(pRed, QJsonObject{
        {"image_path", imgRed}, {"node_uuid", "aaaaaaaa-0000-0000-0000-000000000001"}});
    // 节点 B:抓蓝
    const QJsonObject outB = op.run(pBlue, QJsonObject{
        {"image_path", imgBlue}, {"node_uuid", "bbbbbbbb-0000-0000-0000-000000000002"}});

    check(!outA.contains("error") && !outB.contains("error"), "两个实例都跑成功");
    check(outA.value("mask_path").toString() != outB.value("mask_path").toString(),
          QString("两个节点拿到不同的 mask 路径(A=%1 B=%2)")
            .arg(outA.value("mask_path").toString(), outB.value("mask_path").toString()));

    // 关键:A 的文件必须还在,而且内容还是 **A 自己的**(全红 -> 10000 px)。
    // B 的 mask 是 5000 px,所以这条断言验的是"数据没被串",不只是"路径不同"。
    //
    // 只断言"路径不同"是不够的:文件名一样但内容恰好相同时,危害一样存在
    // 却测不出来。这里特意让两张图的 mask 像素数不同。
    const QString pathA = outA.value("mask_path").toString();
    const int expectA = outA.value("pixel_count").toInt();
    const int expectB = outB.value("pixel_count").toInt();
    check(expectA == 100 * 100 && expectB == 100 * 100 / 2,
          QString("对照组自检:两个 mask 像素数必须不同(A=%1 B=%2)")
            .arg(expectA).arg(expectB));
    cv::Mat maskA = cv::imread(pathA.toStdString(), cv::IMREAD_GRAYSCALE);
    check(!maskA.empty(), "A 的 mask 文件没被 B 覆盖掉");
    if (!maskA.empty()) {
        check(cv::countNonZero(maskA) == expectA,
              QString("A 的 mask 内容还是 A 自己的(pixel=%1,期望 %2 —— B 是 %3)")
                .arg(cv::countNonZero(maskA)).arg(expectA).arg(expectB));
    }

    // 最后一层:让下游真的去读一次。上面验的是"文件对不对",
    // 这条验的是"下游拿到的数对不对" —— 覆盖的真实后果发生在这一层,
    // 因为出错的不是像素,是数出来的数量。
    {
        BlobCount counter;
        const QJsonObject down = counter.run(
            QJsonObject{{"min_area", 20}}, QJsonObject{{"mask_path", pathA}});
        check(down.value("total_area").toInt() == expectA,
              QString("下游 blob_count 沿 A 的路径读回来的面积 = A 的(pixel=%1,期望 %2)")
                .arg(down.value("total_area").toInt()).arg(expectA));
    }

    // 对照组:同一个节点重跑,路径相同是对的 —— 同节点本来就该复用
    const QJsonObject outA2 = op.run(pRed, QJsonObject{
        {"image_path", imgRed}, {"node_uuid", "aaaaaaaa-0000-0000-0000-000000000001"}});
    check(outA2.value("mask_path").toString() == pathA,
          "同一 node_uuid 重跑 -> 路径相同(该复用,不该每次新建文件)");

    // 清理
    QFile::remove(pathA);
    QFile::remove(outB.value("mask_path").toString());
}

// ============================================================
// 用例 3:blob_count 的面积边界
//
// `area >= minArea` 这个 >= 是有讲究的:
// 换成 > 的话,面积正好等于 min_area 的目标会被漏掉,
// 而这在产线上是"刚好卡在阈值上的小件",最该被抓出来。
// 这种错不崩、只少数一个,手点很难发现。
// ============================================================
static void test_blob_count_area_boundary() {
    beginCase("blob_count:面积阈值边界用 >= 而不是 >");

    // 造一张 mask:左边一个 10x10=100 的方块,右边一个 20x20=400 的方块
    cv::Mat mask = cv::Mat::zeros(100, 100, CV_8UC1);
    mask(cv::Rect(0, 0, 10, 10)).setTo(255);      // 面积 100
    mask(cv::Rect(50, 50, 20, 20)).setTo(255);    // 面积 400
    const QString maskPath = "images/w7_test_mask.png";
    cv::imwrite(maskPath.toStdString(), mask);

    BlobCount op;

    // min_area = 100:两个都该算上(100 >= 100 成立)
    {
        QJsonObject out = op.run(QJsonObject{{"min_area", 100}},
                                 QJsonObject{{"mask_path", maskPath}});
        check(out.value("count").toInt() == 2,
              QString("min_area=100 -> 2 个(100 和 400 都算上,实际 %1)")
                .arg(out.value("count").toInt()));
        check(out.value("total_area").toInt() == 500,
              QString("total_area=500(实际 %1)").arg(out.value("total_area").toInt()));
    }

    // min_area = 101:小的那个被滤掉,只该剩 1 个
    // 这条同时也是"过滤器真的在滤"的证据 —— 只测 100 那条的话,
    // 一个把 min_area 完全忽略的实现也能过
    {
        QJsonObject out = op.run(QJsonObject{{"min_area", 101}},
                                 QJsonObject{{"mask_path", maskPath}});
        check(out.value("count").toInt() == 1,
              QString("min_area=101 -> 1 个(100 被滤掉,400 留下,实际 %1)")
                .arg(out.value("count").toInt()));
    }

    // 阈值比最大的还大:全滤掉,count=0,不能崩
    {
        QJsonObject out = op.run(QJsonObject{{"min_area", 100000}},
                                 QJsonObject{{"mask_path", maskPath}});
        check(out.value("count").toInt() == 0,
              QString("min_area=100000 -> 0 个(实际 %1)").arg(out.value("count").toInt()));
        check(!out.contains("error"), "全滤光不算错误(count=0 是合法结果)");
    }

    // 全黑 mask:一个连通域都没有
    {
        const QString emptyPath = "images/w7_test_mask_empty.png";
        cv::imwrite(emptyPath.toStdString(), cv::Mat::zeros(100, 100, CV_8UC1));
        QJsonObject out = op.run(QJsonObject{{"min_area", 20}},
                                 QJsonObject{{"mask_path", emptyPath}});
        check(out.value("count").toInt() == 0, "全黑 mask -> count=0");
        check(!out.contains("error"), "全黑 mask 不算错误");
        QFile::remove(emptyPath);
    }

    // mask 文件不存在
    {
        QJsonObject out = op.run(QJsonObject{{"min_area", 20}},
                                 QJsonObject{{"mask_path", "images/不存在.png"}});
        check(out.contains("error"), "mask 不存在 -> 报 error");
        check(!out.contains("count"), "读不到 mask 时不产出 count(别让下游拿到 0 当真值)");
    }

    // min_area 缺失:用默认值,不能崩
    {
        QJsonObject out = op.run(QJsonObject{}, QJsonObject{{"mask_path", maskPath}});
        check(!out.contains("error"), "params 为空 -> 用默认值,不崩");
    }

    QFile::remove(maskPath);
}

// ============================================================
// 用例 4:count_check 的 min > max
//
// 判定式是 (actual >= min) && (actual <= max)。
// 用户把最小值填成 100、最大值填成 50(QSpinBox 两个都是 0~99999,
// 互相之间没有任何约束,填得进去),那么:
//     任何 actual 都不可能同时 >= 100 且 <= 50
// -> 流水线**永远 FAIL**,而且日志里写的是
//     "数量 60 不在 [100, 50] 内"
// 这句话自己就自相矛盾,用户看不出是自己的阈值填反了。
//
// 产线上的后果:整批判废,却查不出配置问题在哪。
// 这里要求它明确报 error,把"阈值填反了"这件事说出来。
// 注意不能偷偷把两个值交换 —— 那样 [100,50] 变成 [50,100],
// 一批本来会被判废的产品就变成合格了。填错配置时保守(判废)是对的。
// ============================================================
static void test_count_check_inverted_range() {
    beginCase("count_check:阈值填反了要说人话,不能静默全判废");

    CountCheck op;

    // (a) 正常区间
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 1}, {"expected_max", 100}},
                                 QJsonObject{{"count", 5}});
        check(out.value("result").toString() == "PASS", "count=5 在 [1,100] 内 -> PASS");
        check(out.value("actual").toInt() == 5, "actual 字段回显 5");
    }

    // (b) 刚好在下界 / 上界上 —— 闭区间,两端都算内
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 5}, {"expected_max", 10}},
                                 QJsonObject{{"count", 5}});
        check(out.value("result").toString() == "PASS", "count=5 等于下界 -> PASS(闭区间)");
    }
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 5}, {"expected_max", 10}},
                                 QJsonObject{{"count", 10}});
        check(out.value("result").toString() == "PASS", "count=10 等于上界 -> PASS(闭区间)");
    }
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 5}, {"expected_max", 10}},
                                 QJsonObject{{"count", 4}});
        check(out.value("result").toString() == "FAIL", "count=4 小于下界 -> FAIL");
    }
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 5}, {"expected_max", 10}},
                                 QJsonObject{{"count", 11}});
        check(out.value("result").toString() == "FAIL", "count=11 大于上界 -> FAIL");
    }

    // (c) 单点区间:min == max,只有刚好那个数算合格
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 7}, {"expected_max", 7}},
                                 QJsonObject{{"count", 7}});
        check(out.value("result").toString() == "PASS", "min==max==7,count=7 -> PASS");
    }
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 7}, {"expected_max", 7}},
                                 QJsonObject{{"count", 6}});
        check(out.value("result").toString() != "PASS" || out.contains("error"),
              "min==max 但 count 对不上 -> 判 FAIL 或报 error(反正不能是 PASS)");
    }

    // (d) 核心用例:min > max
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 100}, {"expected_max", 50}},
                                 QJsonObject{{"count", 60}});
        check(out.contains("error"),
              QString("min=100 > max=50 -> 报 error(实际 \"%1\")")
                .arg(out.value("error").toString()));
        check(out.value("error").toString().contains("50") &&
              out.value("error").toString().contains("100"),
              QString("报错里带上填反的两个数,用户能直接看出填错了(实际 \"%1\")")
                .arg(out.value("error").toString()));
        // 关键:不能是 PASS。填反了绝不能放行
        check(out.value("result").toString() != "PASS",
              "阈值填反时绝不判 PASS(保守方向)");
    }

    // (e) 上游没给 count:inputs 里没有这个键。
    // toInt() 会给 0,而默认区间是 [0,999] —— 于是"什么都没检测到"
    // 会被判成 PASS。对一个判定算子来说这是危险的默认值。
    {
        QJsonObject out = op.run(QJsonObject{{"expected_min", 1}, {"expected_max", 100}},
                                 QJsonObject{});
        check(out.value("result").toString() != "PASS",
              QString("上游没给 count 时不判 PASS(实际 %1)")
                .arg(out.value("result").toString()));
        check(out.contains("error") || out.value("actual").toInt() == 0,
              "要么明确报错,要么 actual 老实报 0(不能悄悄当合格)");
    }
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);
    QDir().mkpath("images");

    qInfo().noquote() << "=== W7 检测类算子 回归测试 ===";

    test_color_segment_bad_params();
    test_color_segment_no_clobber();
    test_blob_count_area_boundary();
    test_count_check_inverted_range();

    // 清理
    QFile::remove("images/w7_test_solid.png");
    QFile::remove("images/w7_test_red.png");
    QFile::remove("images/w7_test_blue.png");

    qInfo().noquote() << QString("\n========================================");
    qInfo().noquote() << QString("pass=%1  fail=%2").arg(g_pass).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}
