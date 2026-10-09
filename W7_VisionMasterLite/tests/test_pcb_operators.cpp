// ============================================================
// W7 PCB 算子 — 无头回归测试
//
// 为什么需要它:
//   pcb_inspect / pcb_defect_judge 是从 W6 命令行项目搬上流水线的,
//   W6 那边是一堆 judgeXxx() 硬编码阈值,只在 main.cpp 里跑。
//   搬成算子后最大的风险是「阈值搬歪了」——
//   比如那个反直觉的金色 H 85-115,谁看都觉得该是 10-40,顺手一改就废。
//   而且真值坐标模式是写死 6 个 SITES 的,ROI 越界/漏框都不会报错,
//   只看"能编译能跑"根本发现不了。
//
//   这里用 W6 的 8 张样本图(baseline + 7 种缺陷)做端到端断言:
//     baseline 必须 PASS 且零缺陷
//     每张缺陷图必须 FAIL,且必须命中它该命中的那一类
//   —— 等于把 W6 当时的验收清单变成了可重复执行的测试。
//
// 依赖:本测试要真跑 OpenCV 算子,所以链接 OpenCV(与 undo 测试不同)。
// 运行:  w7_pcb_test.exe      (工作目录必须是工程根,里面有 images/)
// ============================================================

// windows.h 会把 min/max 定义成宏,不加 NOMINMAX 的话
// `std::max(...)` 会被展开成 `std::(max)(...)` -> C2589。
// 本文件为了 SetConsoleOutputCP 必须要 windows.h,所以在这里挡掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <opencv2/opencv.hpp>
#include <windows.h>   // SetConsoleOutputCP
#include <algorithm>
#include <cmath>

#include "operators/pcb_inspect.h"
#include "operators/pcb_defect_judge.h"
#include "operators/pcb_color_ref.h"

using namespace w7;

// ============================================================
// 极简断言(与 test_undo_redo.cpp 保持同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}

static void check(bool cond, const QString& what) {
    if (cond) {
        ++g_pass;
        qInfo().noquote() << "  [PASS]" << what;
    } else {
        ++g_fail;
        qWarning().noquote() << "  [FAIL]" << what << "  (case:" << g_case << ")";
    }
}

// ============================================================
// 跑一条完整流水线:image_source → pcb_inspect → pcb_defect_judge
// 跟 GUI 里 Run 点一下是同一条路径(算子的 run() 是纯函数,无 GUI 依赖)。
// ============================================================
struct RunResult {
    QJsonObject inspectOut;
    QJsonObject judgeOut;
    QSet<QString> defectTypes;   // 判定出的缺陷类型集合
};

static RunResult runPipeline(const QString& fileName) {
    RunResult r;
    const QString path = "images/" + fileName;

    PcbInspect inspect;
    r.inspectOut = inspect.run(inspect.defaultParams(),
                               QJsonObject{{"image_path", path}});

    PcbDefectJudge judge;
    r.judgeOut = judge.run(judge.defaultParams(), r.inspectOut);

    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    for (const auto& d : defects)
        r.defectTypes.insert(d.toObject().value("type").toString());
    return r;
}

// 同上,但判定算子的参数可以覆盖 —— 敏感性扫描要用。
// 覆盖走和产线完全一样的 params JSON 入口,不是在测试里另开一条路:
// 另开一条路的话,测的就不是产线真正跑的那条代码。
static RunResult runPipelineWith(const QString& fileName,
                                 const QJsonObject& overrides) {
    RunResult r;
    const QString path = "images/" + fileName;

    PcbInspect inspect;
    r.inspectOut = inspect.run(inspect.defaultParams(),
                               QJsonObject{{"image_path", path}});

    QJsonObject p = PcbDefectJudge().defaultParams();
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        p[it.key()] = it.value();

    PcbDefectJudge judge;
    r.judgeOut = judge.run(p, r.inspectOut);

    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    for (const auto& d : defects)
        r.defectTypes.insert(d.toObject().value("type").toString());
    return r;
}

// ------------------------------------------------------------
// 用例 1:baseline 必须零缺陷
// 这是最重要的回归锚 —— 只要有一类判定在正常板子上误报,
// 后面 7 个用例的 FAIL 就全都不足为凭了。
// ------------------------------------------------------------
static void test_baseline_clean() {
    beginCase("baseline.png 必须 PASS 且零缺陷");
    RunResult r = runPipeline("baseline.png");

    check(!r.inspectOut.contains("error"),
          QString("pcb_inspect 无错误 (%1)")
              .arg(r.inspectOut.value("error").toString("无")));
    check(r.inspectOut.value("count").toInt() == 6,
          QString("定位到 6 个元件 (实际 %1)")
              .arg(r.inspectOut.value("count").toInt()));
    check(r.judgeOut.value("defect_count").toInt() == 0,
          QString("零缺陷 (实际 %1)  命中:%2")
              .arg(r.judgeOut.value("defect_count").toInt())
              .arg(r.defectTypes.values().join(",")));
    check(r.judgeOut.value("result").toString() == "PASS",
          "结论 PASS");
}

// ------------------------------------------------------------
// 用例 2:7 张缺陷图,每张必须命中自己那类
// W6 当时是靠肉眼看 w6_inspect_*.png 判断的,
// 这里变成可重复的断言 —— 改阈值时能立刻知道踩没踩到别的图。
// ------------------------------------------------------------
static void test_each_defect_detected() {
    struct Case { const char* file; const char* type; };
    const Case cases[] = {
        {"missing.png",       "missing"},
        {"wrong.png",         "wrong"},
        {"polarity.png",      "polarity"},
        {"bridge.png",        "bridge"},
        {"insufficient.png",  "insufficient"},
        {"shifted.png",       "shifted"},
        {"short.png",         "short"},
    };
    for (const auto& c : cases) {
        beginCase(QString("%1 必须判出 %2").arg(c.file, c.type));
        RunResult r = runPipeline(c.file);
        check(r.judgeOut.value("result").toString() == "FAIL",
              "结论 FAIL");
        check(r.defectTypes.contains(c.type),
              QString("命中缺陷类型 %1  (实际:%2)")
                  .arg(c.type).arg(r.defectTypes.values().join(",")));
    }
}

// ------------------------------------------------------------
// 用例 3:短路不归属任何元件
// short 是全局缺陷,defect.component 留空。
// 如果哪天有人给它硬塞个元件名,这条会挂 —— 提醒他这类的语义变了。
// ------------------------------------------------------------
static void test_short_has_no_owner() {
    beginCase("short.png 的短路缺陷不挂元件");
    RunResult r = runPipeline("short.png");
    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    int shortCnt = 0;
    for (const auto& d : defects) {
        QJsonObject o = d.toObject();
        if (o.value("type").toString() != "short") continue;
        ++shortCnt;
        check(o.value("component").toString().isEmpty(),
              QString("短路 component 为空 (实际 '%1')")
                  .arg(o.value("component").toString()));
    }
    check(shortCnt == 1, QString("短路判出 1 处 (实际 %1)").arg(shortCnt));
}

// ------------------------------------------------------------
// 用例 4:输入缺失时给友好错误,而不是崩
// pcb_defect_judge 单独接在图像源后面(没接 pcb_inspect)是很容易犯的错,
// 这种情况必须报「需要先接 pcb_inspect」,而不是让用户对着空界面猜。
// ------------------------------------------------------------
static void test_missing_input_error() {
    beginCase("缺上游时给友好错误");
    PcbDefectJudge judge;
    QJsonObject out = judge.run(judge.defaultParams(),
                               QJsonObject{{"image_path", "images/baseline.png"}});
    check(out.value("error").toString().contains("pcb_inspect"),
          QString("提示需要先接 pcb_inspect (实际 '%1')")
              .arg(out.value("error").toString()));

    QJsonObject out2 = judge.run(judge.defaultParams(), QJsonObject{});
    check(out2.value("error").toString().contains("image_path"),
          QString("提示缺 image_path (实际 '%1')")
              .arg(out2.value("error").toString()));
}

// ------------------------------------------------------------
// 用例 5:阈值可调 —— 这正是搬成算子的意义
// 把 dark_min_pct 调到 0,missing 判定必须失效;baseline 依然零缺陷。
// 说明阈值真的在控制逻辑,不是摆设。
//
// (这里跟着阈值单位一起改过:原来是像素数 dark_thresh=0 / =100000,
//  现在是百分比 dark_min_pct=0 / =100。名字和量纲都变了 ——
//  沿用旧名字的话,老存档里的 50 会被读成 50%。)
// ------------------------------------------------------------
static void test_threshold_actually_controls() {
    beginCase("阈值真的在控制判定(不是摆设)");
    PcbInspect inspect;
    QJsonObject iOut = inspect.run(inspect.defaultParams(),
                                   QJsonObject{{"image_path", "images/missing.png"}});

    PcbDefectJudge judge;
    QJsonObject loose = judge.defaultParams();
    loose["dark_min_pct"] = 0;         // 放宽:任何图都不再判缺件
    QJsonObject lOut = judge.run(loose, iOut);
    QJsonArray lDefects = lOut.value("defects").toArray();
    bool stillMissing = false;
    for (const auto& d : lDefects)
        if (d.toObject().value("type").toString() == "missing") stillMissing = true;
    check(!stillMissing, "dark_min_pct=0 时不再判缺件");

    QJsonObject tight = judge.defaultParams();
    tight["dark_min_pct"] = 100;       // 收紧:正常板子也会被判缺件
    QJsonObject tOut = judge.run(tight,
        inspect.run(inspect.defaultParams(),
                    QJsonObject{{"image_path", "images/baseline.png"}}));
    check(tOut.value("result").toString() == "FAIL",
          "dark_min_pct=100 时 baseline 也 FAIL(证明阈值生效)");
}

// ------------------------------------------------------------
// 用例 6:auto_contour 模式不依赖 SITES 真值
// 这是"换板型也能用"这条承诺的验证:同一张图,自动模式照样能框出东西。
// ------------------------------------------------------------
static void test_auto_contour_mode() {
    beginCase("auto_contour 自动定位模式");
    PcbInspect inspect;
    QJsonObject p = inspect.defaultParams();
    p["mode"] = "auto_contour";
    QJsonObject out = inspect.run(p, QJsonObject{{"image_path", "images/baseline.png"}});

    check(!out.contains("error"), "无错误");
    check(out.value("mode").toString() == "auto_contour", "模式为 auto_contour");
    int n = out.value("count").toInt();
    check(n > 0, QString("自动框出 %1 个元件(应 > 0)").arg(n));
    check(n < 30, QString("框数 %1 未爆炸(应 < 30,说明聚类生效)").arg(n));

    // 画框图要真的落盘
    check(QFile::exists(out.value("marked_path").toString()),
          QString("画框图已写出 %1")
              .arg(out.value("marked_path").toString()));
}

// ------------------------------------------------------------
// 用例 7:已知的跨判定串扰 —— 特征化测试(characterization test)
//
// 现象:short.png 会同时报出 short 和 wrong 两种缺陷。
// 这不是搬算子时搬出来的 bug,W6 当时就是这样,原因在样本数据:
//   gen_pcb_dataset.py 里 GOLD = (30,180,220) BGR,
//   也就是**青色**(H≈90,正好在 W6 校准的金层范围里)。
//   而 judgeWrong 的 BGR 范围 (150,150,0)-(255,255,80)
//   要求的正是 B/G 高、R 低 —— 青色也满足。
//   defect_short 把金色短路桥画在 x 418..664,
//   R3 的 ROI 是 x 640..760 → 桥有 24px 落在 R3 的框里。
// 于是短路桥顺带触发了错件判定。
//
// 为什么专门写一条测试锁住它:
//   这是"当前真实行为"的记录,不是"期望行为"。
//   以后谁调了 wrong 阈值或改了 ROI,这条会立刻响,
//   那时候是接受(阈值改窄了)还是修 bug,是个有意识的选择,
//   而不用等某天误报飘上产线才发现。
//
// 如果判定逻辑改好了(短路桥不再串到 wrong),删掉本用例即可。
// ------------------------------------------------------------

// ============================================================
// 用例 7:判废算子不能对"没检查过"的东西报合格
//
// 这是本项目里最坏的一类错,比崩溃难查得多:
// 崩溃你会立刻知道,报错你会看见,而"什么都没检查却报 PASS"是**安静地错**。
//
// 成因:pcb_inspect 有两种模式 ——
//   真值模式      :按 SITES 构造 ROI,标签是 R1/C1/U1/R2/C2/R3(真站点)
//   自动轮廓模式  :聚类连通域,标签是 C1/C2/C3...,**编号来自轮廓扫描顺序**
// 而 pcb_defect_judge 的六条规则全部按 label 硬匹配
// (R2→missing、R3→wrong、C1→polarity、R1→bridge、U1→insufficient、R2→shifted)。
// 把自动模式的产物喂进去:只有 "C1" 会命中,而且命中的是随便一颗元件;
// R1/U1/R2/R3 四个分支一次都进不去。
//
// 于是 defects 永远是空的 → result 永远是 PASS,而 component_count 还显示 6。
// 判废算子在什么都没检查的情况下报"合格"。
//
// 注意 test_auto_contour_mode 只单独测了 inspect(框数>0、<30、落盘),
// 从来没跑过 inspect(auto) → judge 这一对 —— 断链就藏在这个缝里。
// ============================================================
static void test_judge_refuses_uninspectable_components() {
    beginCase("判废算子:没有判定依据时拒绝判定,而不是报 PASS");

    // --- 前置:先确认 auto 模式确实产出 kind="AUTO" 的组件 ---
    PcbInspect inspect;
    QJsonObject ip = inspect.defaultParams();
    ip["mode"] = "auto_contour";
    const QJsonObject iout =
        inspect.run(ip, QJsonObject{{"image_path", "images/baseline.png"}});
    const QJsonArray autoComps = iout.value("components").toArray();
    check(autoComps.size() > 0,
          QString("前置:auto 模式产出 %1 个元件").arg(autoComps.size()));
    bool allAuto = autoComps.size() > 0;
    for (const auto& v : autoComps) {
        if (v.toObject().value("kind").toString() != "AUTO") allAuto = false;
    }
    check(allAuto, "前置:auto 模式每个元件的 kind 都是 AUTO");

    // --- 核心:把它喂给判废算子 ---
    PcbDefectJudge judge;
    const QJsonObject jout = judge.run(
        QJsonObject{}, QJsonObject{{"image_path", "images/baseline.png"},
                                   {"components", autoComps}});

    check(jout.contains("error"),
          QString("自动标签 -> 报错而不是判定(实际 error=\"%1\")")
            .arg(jout.value("error").toString()));
    check(!jout.value("error").toString().isEmpty() &&
              jout.value("error").toString().contains("自动轮廓"),
          "报错要点明是自动轮廓模式的问题(用户不看代码也该知道怎么改)");
    check(jout.value("result").toString() != "PASS",
          QString("绝不能报 PASS(实际 result=\"%1\")")
            .arg(jout.value("result").toString()));
    check(!jout.contains("result"),
          "连 result 字段都不给 —— 没判定过就没有判定结果,"
          "空字符串会被下游当成\"判了但没值\"");

    // --- 兜底:手改的 JSON 可能压根没有 kind 字段 ---
    {
        QJsonArray bogus;
        QJsonObject c;
        c["label"] = "焊盘7";      // 本算子不认的标签
        c["x"] = 10; c["y"] = 10; c["w"] = 30; c["h"] = 30;
        bogus.append(c);
        const QJsonObject out = judge.run(
            QJsonObject{}, QJsonObject{{"image_path", "images/baseline.png"},
                                       {"components", bogus}});
        check(out.contains("error"), "标签一个都不认得 -> 报错");
        check(out.value("result").toString() != "PASS",
              "标签一个都不认得时也不报 PASS");
    }

    // --- 对照组:真值模式必须照常判定 ---
    // 没有这条,"永远报错"也能让上面全绿 —— 而那等于把判废功能整个废了。
    {
        RunResult r = runPipeline("baseline.png");
        check(r.judgeOut.value("result").toString() == "PASS",
              QString("对照组:真值模式 baseline 照常判 PASS(实际 \"%1\")")
                .arg(r.judgeOut.value("result").toString()));
        check(!r.judgeOut.contains("error"),
              QString("对照组:真值模式不报错(实际 error=\"%1\")")
                .arg(r.judgeOut.value("error").toString()));
        // 真值模式 6 个元件里 C2 本来就没有判定规则(W6 从没给 C2 定义过
        // 缺陷类型),所以应该有"覆盖不全"的提示,而不是假装全查过了。
        check(r.judgeOut.value("warning").toString().contains("未判定"),
              QString("只判定了部分元件时要说明覆盖范围(实际 warning=\"%1\")")
                .arg(r.judgeOut.value("warning").toString()));
    }
}
// ------------------------------------------------------------
// 短路圆环和邻位元件的串扰
// ------------------------------------------------------------
// 以前这条钉的是"串扰**存在**":short.png 的短路桥伸进 R3 的 ROI,
// 把 R3 的浅黄像素抬到 261px,而当时阈值是 200px —— 于是顺带报了个
// `wrong`。那个 wrong 是假的:R3 本身没问题,是邻居的桥伸过来的。
//
// 阈值改成占 ROI 面积的百分比之后,实测:
//   串扰抬起来的浅黄 = 4.53% ROI
//   R3 真的错件时    = 44.8% ROI   (wrong.png)
// 默认值取 12% —— 两边都留着余量,串扰不再触发,真错件照样抓得到。
// 所以这里的预期从"报出 wrong"翻转成"**不**报 wrong"。
//
// 仍然保留结构性的那一半:wrong 必须挂在 R3 身上。
// 用 wrong.png 来验 —— 那张图 R3 是真的错了。
// 万一以后是别的元件串了,光看"类型还在"会误以为没事。
static void test_short_crosstalk_and_wrong_owner() {
    beginCase("short.png 的串扰不再误报 wrong");
    RunResult r = runPipeline("short.png");

    check(r.defectTypes.contains("short"), "仍然报出 short");
    check(!r.defectTypes.contains("wrong"),
          QString("串扰不再触发假 wrong(实际命中:%1)")
              .arg(r.defectTypes.values().join(",")));

    RunResult w = runPipeline("wrong.png");
    check(w.defectTypes.contains("wrong"), "wrong.png 仍报出 wrong");
    const QJsonArray defects = w.judgeOut.value("defects").toArray();
    QString wrongOwner;
    for (const auto& d : defects) {
        QJsonObject o = d.toObject();
        if (o.value("type").toString() == "wrong")
            wrongOwner = o.value("component").toString();
    }
    check(wrongOwner == "R3",
          QString("wrong 挂在 R3 上(实际 '%1')").arg(wrongOwner));
}

// ------------------------------------------------------------
// 用例 9:HSV 阈值给成怪形状时报错,不崩
//
// 成因是抄副本:QJsonArray::operator[] **不检查越界**。
// 以前 pcb_inspect.cpp 的 run() 写的是裸的 `lo[0].toInt(85)`,
// hsv_low 传成空数组就 0xC0000005。同样的守卫 color_segment.cpp
// 早就修过了 —— 抄副本的结果是同一个坑踩两次。
//
// 这里钉两件事:
//   ① 崩 -> 变成一条明确的 error
//   ② 报错时**不产出任何元件** —— 不能给下游一个"0 个元件"的列表,
//      那和"什么都没检查却报 PASS"是同一类安静的错误。
// ------------------------------------------------------------
static void test_hsv_param_guarded() {
    beginCase("金层 HSV 参数形状不对时报错,不崩");

    PcbInspect inspect;
    const QJsonObject in{{"image_path", "images/baseline.png"}};

    struct Case {
        const char* what;
        QJsonValue low;
        const char* expect;         // 报错里应该点名的字段
    };
    const Case cases[] = {
        {"空数组",       QJsonArray{},                 "hsv_low"},
        {"只有两个元素", QJsonArray{85, 80},           "hsv_low"},
        {"第一个是字符串", QJsonArray{"85", 80, 80},   "hsv_low"},
        {"根本不是数组",  QJsonValue(85),              "hsv_low"},
    };

    for (const auto& c : cases) {
        QJsonObject p = inspect.defaultParams();
        p["hsv_low"] = c.low;
        QJsonObject out = inspect.run(p, in);

        check(out.contains("error"),
              QString("%s -> 有 error(实际 result=\"%1\")")
                  .arg(c.what, out.value("result").toString()));
        check(out.value("error").toString().contains(c.expect),
              QString("%s -> 报错点名字段 %2(实际 \"%3\")")
                  .arg(c.what, c.expect, out.value("error").toString()));
        check(!out.contains("count"),
              QString("%s -> 不产出 count(实际 %1)")
                  .arg(c.what).arg(out.value("count").toInt()));
    }

    // hsv_high 同样要查,别只测一半
    {
        QJsonObject p = inspect.defaultParams();
        p["hsv_high"] = QJsonArray{};
        QJsonObject out = inspect.run(p, in);
        check(out.value("error").toString().contains("hsv_high"),
              QString("hsv_high 空数组 -> 报错点名 hsv_high(实际 \"%1\")")
                  .arg(out.value("error").toString()));
    }

    // 对照组:没有这条,"永远报错"也能让上面全绿 —— 而那等于把定位废了
    QJsonObject ok = inspect.run(inspect.defaultParams(), in);
    check(!ok.contains("error"),
          QString("对照组:默认参数照常工作(实际 error=\"%1\")")
              .arg(ok.value("error").toString()));
    check(ok.value("count").toInt() == 6,
          QString("对照组:仍然定位到 6 个元件(实际 %1)")
              .arg(ok.value("count").toInt()));
}

// ------------------------------------------------------------
// 用例 10:相对颜色判据的**尺度不变性**(性质测试)
// ------------------------------------------------------------
// "去绝对化"不是"结果差不多",是**逐像素分类结果完全一样**。
// 这里直接钉性质:把图整体乘 k 之后,同一个像素在 isDark /
// isOffColor / isWhiteSilk 三条判据下的分类必须**一模一样**。
//
// 只检查**缩放后逐位精确**的像素(pa[c]*k 恰好是整数且不超 255)。
// 削波和 8 位舍入都是不可逆的信息丢失,乘回去补不回来 —— 削波那部分
// 正是 pcb_color_ref.h 里 ceilingBreached 要处理的情况;而中性度这种
// max/min 比值,在除数小的时候 ±0.5 的量化就能跨过阈值,那是噪声不是
// 判据的性质。要求"所有像素完全一致"等于要求一个做不到的性质。
//
// 这条断言的可证伪性:把 isOffColor 里任意一个系数换回绝对值
// (比如 double(p[1]) >= 150),k=0.6 那一档立刻大面积变红。
// 反过来,把 kOffColorMul 从 1.6 改成 2.0,k=0.55 档会因为
// 舍入差一个单位而翻车(实测青色 G/板面G ≈ 1.98)—— 也会被抓住。
// ------------------------------------------------------------
static void test_color_ref_scale_invariant() {
    beginCase("相对颜色判据对整体亮度缩放逐像素不变");

    cv::Mat src = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
    check(!src.empty(), "读得到 images/baseline.png");
    if (src.empty()) return;

    const cv::Vec3b base1 = pcbcolor::estimateBoardBase(src);
    check(!pcbcolor::ceilingBreached(base1),
          QString("原始亮度不撞上限 (板面 BGR %1,%2,%3,还能撑到 %4 倍)")
              .arg(base1[0]).arg(base1[1]).arg(base1[2])
              .arg(pcbcolor::ceilingHeadroomK(base1), 0, 'f', 2));

    for (double k : {0.40, 0.60, 0.80, 1.20, 1.40, 1.60}) {
        cv::Mat scaled;
        src.convertTo(scaled, CV_8U, k);
        const cv::Vec3b baseK = pcbcolor::estimateBoardBase(scaled);

        // 基色本身必须跟着缩放走。差 1 个量化单位是舍入,不算错。
        int maxDrift = 0;
        for (int c = 0; c < 3; ++c)
            maxDrift = std::max(maxDrift,
                std::abs(int(baseK[c]) - int(std::lround(base1[c] * k))));
        check(maxDrift <= 1,
              QString("k=%1 板面基色跟着缩放 (原 %2,%3,%4 -> %5,%6,%7,最大偏差 %8)")
                  .arg(k, 4, 'f', 2)
                  .arg(base1[0]).arg(base1[1]).arg(base1[2])
                  .arg(baseK[0]).arg(baseK[1]).arg(baseK[2]).arg(maxDrift));

        // 逐像素:分类必须一致。
        //
        // 只看**缩放后没有舍入误差**的像素(pa[c]*k 恰好是整数且不超 255)。
        // 理由:8 位量化本身有 ±0.5 的误差,isWhiteSilk 里的中性度是
        // max/min 这种比值,除数小的时候 ±0.5 能让比值跨过阈值 —— 那是
        // 量化噪声,不是判据不不变。要求"所有像素分类完全一致"就是在
        // 要求一个物理上做不到的性质,那样的测试只会被量化误差搞红,
        // 然后下一次就会被人把断言改松。**门槛只能往严里加,不能往松里调。**
        //
        // 逐位精确的那些像素上,不变性是可证的:三条判据都是像素和基色的
        // 0 次齐次式,两边同时乘 k 不等号方向不变。
        long nDark = 0, nOff = 0, nWhite = 0, nExact = 0;
        for (int y = 0; y < src.rows; ++y) {
            const cv::Vec3b* a = src.ptr<cv::Vec3b>(y);
            const cv::Vec3b* b = scaled.ptr<cv::Vec3b>(y);
            for (int x = 0; x < src.cols; ++x) {
                const cv::Vec3b pa = a[x], pb = b[x];
                bool exact = true;
                for (int c = 0; c < 3; ++c)
                    if (double(pb[c]) != double(pa[c]) * k) { exact = false; break; }
                if (!exact) continue;
                ++nExact;
                if (pcbcolor::isDark(pa, base1) !=
                    pcbcolor::isDark(pb, baseK)) ++nDark;
                if (pcbcolor::isOffColor(pa, base1) !=
                    pcbcolor::isOffColor(pb, baseK)) ++nOff;
                if (pcbcolor::isWhiteSilk(pa, base1) !=
                    pcbcolor::isWhiteSilk(pb, baseK)) ++nWhite;
            }
        }
        check(nExact > 1000,
              QString("k=%1 有足够的精确缩放像素可比(%2 个)")
                  .arg(k, 4, 'f', 2).arg(nExact));
        check(nDark == 0 && nOff == 0 && nWhite == 0,
              QString("k=%1 精确像素上分类逐位不变 (暗判 %3 处 / 错色判 %4 处 /"
                      "白判 %5 处,共比 %2 个)")
                  .arg(k, 4, 'f', 2).arg(nExact).arg(nDark).arg(nOff).arg(nWhite));
    }
}

// ------------------------------------------------------------
// 用例 11:端到端的亮度不变性 —— 整条流水线跨亮度档
// ------------------------------------------------------------
// 用例 10 钉的是判据本身,这条钉的是**整条链路**:定位 -> 判定。
// 定位那一步也可能有绝对量(SITES 已经归一化过),这里一起兜住。
//
// 亮度档取到 1.60 而不是更高:实测 k>1.77 时 1.6 x 板面绿 > 255,
// 判据在物理上就不成立,那是过曝,归到用例 12。
// ------------------------------------------------------------
static RunResult runPipelineAbs(const QString& absPath) {
    RunResult r;
    PcbInspect inspect;
    r.inspectOut = inspect.run(inspect.defaultParams(),
                               QJsonObject{{"image_path", absPath}});
    PcbDefectJudge judge;
    r.judgeOut = judge.run(judge.defaultParams(), r.inspectOut);
    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    for (const auto& d : defects)
        r.defectTypes.insert(d.toObject().value("type").toString());
    return r;
}

static void test_brightness_invariance_end_to_end() {
    beginCase("整条流水线跨亮度不变(0.40x ~ 1.60x)");

    struct Case { const char* file; const char* type; };
    const Case cases[] = {
        {"baseline.png",      nullptr},   // 必须 PASS
        {"missing.png",       "missing"},
        {"wrong.png",         "wrong"},
        {"polarity.png",      "polarity"},
        {"bridge.png",        "bridge"},
        {"insufficient.png",  "insufficient"},
        {"shifted.png",       "shifted"},
        {"short.png",         "short"},
    };
    const QString outDir = "build/_bright";
    QDir().mkpath(outDir);

    int passCells = 0, totalCells = 0;
    QStringList failed;

    for (double k : {0.40, 0.60, 0.80, 1.00, 1.20, 1.40, 1.60}) {
        for (const auto& c : cases) {
            const QString stem = QString(c.file).remove(".png");
            const QString path = QString("%1/%2_x%3.png")
                .arg(outDir, stem).arg(k, 4, 'f', 2);

            cv::Mat src = cv::imread(QString("images/%1").arg(c.file).toStdString(),
                                     cv::IMREAD_COLOR);
            if (src.empty()) continue;
            cv::Mat scaled;
            src.convertTo(scaled, CV_8U, k);
            if (!cv::imwrite(path.toStdString(), scaled)) continue;

            const RunResult r = runPipelineAbs(path);
            ++totalCells;
            const bool ok =
                !r.judgeOut.contains("error")
                && (c.type == nullptr
                      ? r.judgeOut.value("result").toString() == "PASS"
                      : r.judgeOut.value("result").toString() == "FAIL"
                        && r.defectTypes.contains(c.type));
            if (ok) ++passCells;
            else failed << QString("%1@%2x(err=%3,result=%4,命中=%5)")
                .arg(c.file).arg(k, 0, 'f', 2)
                .arg(r.judgeOut.value("error").toString("无"),
                     r.judgeOut.value("result").toString("无"),
                     r.defectTypes.values().join("+"));
        }
    }

    check(totalCells == 56, QString("格子数够(期望 56,实际 %1)").arg(totalCells));
    check(failed.isEmpty(),
          QString("7 档亮度 x 8 张图全部维持原结论 (%1/%2)%3")
              .arg(passCells).arg(totalCells)
              .arg(failed.isEmpty() ? QString()
                                    : "  失败的格子:\n      " + failed.join("\n      ")));
}

// ------------------------------------------------------------
// 用例 12:过曝时必须拒绝判定,不能报 PASS
// ------------------------------------------------------------
// 这是这次改动里最要紧的一条。错色判据要求绿通道 >= 1.6 x 板面绿,
// 板面亮到一定程度这个数超过 255,uint8 里根本没有这么亮的像素,
// 判据会**永远返回 false** —— 不报错、不告警,直接 PASS,废板判合格。
//
// 这和自动轮廓模式那个老问题是同一类:拿不到判定依据却输出合格。
// 产线上那种错最难查 —— 状态点是绿的,日志一片正常。
// ------------------------------------------------------------
static void test_overexposed_refuses_to_judge() {
    beginCase("曝光过强时拒绝判定,而不是报 PASS");

    cv::Mat src = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
    if (src.empty()) { check(false, "读得到 images/baseline.png"); return; }

    // k=1.90 -> 板面绿 171,1.6x = 273 > 255,越线
    cv::Mat hot;
    src.convertTo(hot, CV_8U, 1.90);
    const QString path = "build/_bright/overexposed_x1.90.png";
    QDir().mkpath("build/_bright");
    cv::imwrite(path.toStdString(), hot);

    PcbInspect inspect;
    const QJsonObject iOut = inspect.run(inspect.defaultParams(),
                                         QJsonObject{{"image_path", path}});
    PcbDefectJudge judge;
    const QJsonObject jOut = judge.run(judge.defaultParams(), iOut);

    check(!iOut.contains("error"),
          QString("定位这一步仍然成功(过曝不影响定位,实际 \"%1\")")
              .arg(iOut.value("error").toString()));
    check(jOut.value("error").toString().contains("曝光"),
          QString("判定给出「曝光过强」的错误(实际 \"%1\")")
              .arg(jOut.value("error").toString()));
    check(!jOut.contains("result"),
          QString("不给 result —— 没判定过就没有判定结果可给"
                  "(实际 \"%1\")")
              .arg(jOut.value("result").toString("没给")));
    check(!jOut.contains("defects"),
          QString("也不给 defects 列表(实际 %1 项)")
              .arg(jOut.value("defects").toArray().size()));
}

// ------------------------------------------------------------
// 用例 13:阈值不是刀尖上的平衡 —— 系数上下扫,结论纹丝不动
// ------------------------------------------------------------
// 这个项目吃过一次亏:`pad_min_pct` 取 0.44 是按 1x 的
// (噪声斑 0.38%, 焊少那颗 0.51%) 算的,贴着噪声斑上沿,
// 换个尺度就翻车。所以阈值不只要"此刻对",还得**离边界有距离**。
//
// 这条测试把每个系数**逐个**乘 0.8 / 1.2,再跑一遍 8 张样本,
// 结论必须和原值完全一样。变了一格就说明这个数贴在悬崖边上。
//
// 三件事是踩过之后才写对的,记在这里免得下次又写回去:
//
// ① **断言的是结论,不是像素数。** 第一版断言"系数一动,各判据
//    像素数完全不变"—— 那是错的:阈值一动,分到某一侧的像素当然会变,
//    而且实测 x1.2 时白色像素质数从 0 跳到 17552(大量像素的中性度
//    恰好压在 1.15 上)。像素数不是不变量,结论才是。
//
// ② **逐个扫,不一起扫。** 第一版把五个系数同时乘 f,测的是叠加效应,
//    测不出"哪个系数贴在悬崖边"。
//
// ③ **参数化才扫得动。** 这些系数本来是 constexpr,写死就只能靠
//    复制一份判据出来扫 —— 第四次抄副本。改成算子参数之后,
//    测试走的是和产线完全一样的入口。
//
// 变异验证证明这条不是摆设:把 kNeutralMax 1.15->1.02、
// 把 kDarkMul 0.6->0.9,在没有这条测试时都是**全绿**的。
// ------------------------------------------------------------
static void test_color_threshold_sensitivity() {
    beginCase("颜色系数逐个上下 20%,结论不变");

    // 每张图一个"结论"。用"每元件的缺陷类型串"而不是只看 PASS/FAIL:
    // PASS/FAIL 是二值的,一个系数把错件类型换掉但没翻转结论时看不出来。
    struct Case { const char* file; const char* type; };
    const Case cases[] = {
        {"baseline.png",      ""},
        {"missing.png",       "missing"},
        {"wrong.png",         "wrong"},
        {"polarity.png",      "polarity"},
        {"bridge.png",        "bridge"},
        {"insufficient.png",  "insufficient"},
        {"shifted.png",       "shifted"},
        {"short.png",         "short"},
    };

    auto verdictOf = [&](const RunResult& r) {
        QStringList v;
        v << r.judgeOut.value("result").toString();
        QStringList types = r.defectTypes.values();
        std::sort(types.begin(), types.end());
        v << types.join("+");
        if (r.judgeOut.contains("error")) v << "ERR:" + r.judgeOut.value("error").toString().left(20);
        return v.join("/");
    };

    QHash<QString, QString> base;
    for (const auto& c : cases) base[c.file] = verdictOf(runPipeline(c.file));

    // 逐个系数扫,一次只动一个 —— 之前一版把五个一起缩,那是测"叠加效应",
    // 测不出"哪个系数贴在悬崖边"。
    // neutral_max 不在这里:它往下有数学地板(恒 >= 1.0),
    // 单独在下面按非对称扫。把它混进对称扫只会得到一条
    // "±20% 结论不变"的假断言 —— 实测 x0.8 恰恰会变。
    struct Coef { const char* key; const char* label; double base; };
    const Coef coefs[] = {
        {"dark_v_mul",      "暗判 V 倍数",      pcbcolor::kDarkMul},
        {"bright_v_mul",    "白判 V 倍数",      pcbcolor::kBrightMul},
        {"off_color_mul",   "错色倍数",        pcbcolor::kOffColorMul},
        {"off_color_r_max", "错色红通道上限",  pcbcolor::kOffColorRMax},
        {"mask_v_mul",      "金层掩膜 V 下界", pcbcolor::kMaskVMul},
    };

    for (const auto& co : coefs) {
        for (double f : {0.8, 1.2}) {
            const double v = co.base * f;
            QStringList diff;
            for (const auto& c : cases) {
                const QString got = verdictOf(
                    runPipelineWith(c.file, QJsonObject{{co.key, v}}));
                if (got != base[c.file])
                    diff << QString("%1:%2 != %3").arg(c.file, got, base[c.file]);
            }
            check(diff.isEmpty(),
                  QString("%1 %2 -> x%3 结论不变%4")
                      .arg(co.label, co.key).arg(f, 4, 'f', 1)
                      .arg(diff.isEmpty() ? QString()
                                          : "  实测 " + diff.join("; ")));
        }
    }

    // ---- neutral_max 只能往上调,往下有数学地板 ----
    //
    // 上面那轮 ±20% 扫里,neutral_max x0.8 = 0.92 会让 polarity.png
    // 从 FAIL/polarity 变成 PASS。查下来不是阈值偏严,是判据被证伪:
    // neutralness = max/min 恒 >= 1,所以 <= 0.92 永远不成立。
    // 实测(白像素占 C1 ROI 面积):
    //     1.15 -> 4.21%   1.02 -> 4.10%   1.00 -> 4.05%
    //     0.98 -> 0.00%   0.92 -> 0.00%   0.80 -> 0.00%
    // 断崖 -> 掩膜全空 -> white_min_pct 把 polarity 规则挡掉 -> 报 PASS。
    // 极性反的板判合格,还不报错。所以这条不能对称扫,得单独钉。
    {
        for (double nm : {1.02, 1.00}) {
            const RunResult r = runPipelineWith("polarity.png",
                                                 QJsonObject{{"neutral_max", nm}});
            check(r.defectTypes.contains("polarity"),
                  QString("neutral_max=%1(polarity 地板之上),polarity 仍判出(实际 %2)")
                      .arg(nm, 0, 'f', 2).arg(r.defectTypes.values().join("+")));
        }
        // 地板之下必须被夹住 —— 夹住的表现是"照常判出",不是"照常报 PASS"
        for (double nm : {0.92, 0.50}) {
            const RunResult r = runPipelineWith("polarity.png",
                                                 QJsonObject{{"neutral_max", nm}});
            check(r.defectTypes.contains("polarity"),
                  QString("neutral_max=%1(低于 1.0 地板)被夹住,polarity 仍判出(实际 %2)")
                      .arg(nm, 0, 'f', 2).arg(r.defectTypes.values().join("+")));
        }

        // ⚠ 上面那两条只证明了"不越界不塌",证明不了**默认值本身够不够远**。
        //
        // 原因值得记下来:敏感性扫是**相对默认值**做的 ——
        // 把默认值从 1.15 改成 1.02,整条扫描会跟着一起挪到 1.02 附近,
        // 结论照样一致,全绿。变异验证 M8(kNeutralMax 1.15->1.02)就是这么
        // 活下来的。**结论不变的测试永远抓不到"默认值选错了"**。
        //
        // 而 1.02 确实是错的选法:它离实测断崖(1.00)只剩 2%,
        // 正是本项目吃过亏的那种"刀尖上的平衡"。所以默认值必须单独
        // 钉住离地板的余量。
        //
        // 为什么地板之上还要留 10%:白像素占比在 1.00~1.15 之间
        // 只从 4.05% 爬到 4.21%,曲线极平 —— 也就是说这段区间里
        // 阈值调高调低几乎没区别,唯独贴到 1.00 就断崖。
        // 平坦区意味着"这个参数在这里没什么用",而不是"这里很稳"。
        check(pcbcolor::kNeutralMax >= 1.10,
              QString("中性度默认值离 1.0 地板留 >=10%% 余量(实际 %1,离地板 %2%%)")
                  .arg(pcbcolor::kNeutralMax, 0, 'f', 2)
                  .arg(100.0 * (pcbcolor::kNeutralMax - pcbcolor::kNeutralFloor)
                           / pcbcolor::kNeutralFloor, 0, 'f', 1));
    }

    // 定向钉两条。上面是 ±20% 的"余量够不够",这条是"明知有问题时能不能被抓住":
    // kNeutralMax 收到 1.02 会削掉约 4% 的白色像素(实测 17230 -> 16563),
    // 这是**实测结论**,不是"应当不变" —— 所以这条断言的是"判定结果仍然正确",
    // 而不是"像素数一个不动"。像素数本来就会动,把不变当不变量是写错了断言。
    {
        RunResult a = runPipelineWith("polarity.png", QJsonObject{{"neutral_max", 1.02}});
        check(a.judgeOut.value("result").toString() == "FAIL"
                  && a.defectTypes.contains("polarity"),
              QString("kNeutralMax 收到 1.02,polarity.png 仍判出极性反(实际 %1:%2)")
                  .arg(a.judgeOut.value("result").toString(),
                       a.defectTypes.values().join("+")));

        RunResult b = runPipelineWith("missing.png", QJsonObject{{"dark_v_mul", 0.9}});
        check(b.judgeOut.value("result").toString() == "FAIL"
                  && b.defectTypes.contains("missing"),
              QString("kDarkMul 放宽到 0.9,missing.png 仍判出缺件(实际 %1:%2)")
                  .arg(b.judgeOut.value("result").toString(),
                       b.defectTypes.values().join("+")));

        // 反向:系数动到明显离谱时,判定必须跟着动。
        // 只测"变了一定没事"会退化成"这参数根本没人看"——
        // M8/M9 之所以活下来,就是因为没有任何断言要求它生效。
        RunResult c = runPipelineWith("missing.png", QJsonObject{{"dark_v_mul", 2.0}});
        check(!c.defectTypes.contains("missing"),
              QString("kDarkMul 放到 2.0(远高于板面 V),缺件不该再报(实际 %1)")
                  .arg(c.defectTypes.values().join("+")));
    }
}

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");   // headless
    // Windows 控制台默认 GBK,中文断言信息会乱码。强制 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 PCB 算子 回归测试(端到端跑 W6 样本)";

    test_baseline_clean();
    test_each_defect_detected();
    test_short_has_no_owner();
    test_missing_input_error();
    test_threshold_actually_controls();
    test_auto_contour_mode();
    test_judge_refuses_uninspectable_components();
    test_short_crosstalk_and_wrong_owner();
    test_hsv_param_guarded();
    test_color_ref_scale_invariant();
    test_brightness_invariance_end_to_end();
    test_overexposed_refuses_to_judge();
    test_color_threshold_sensitivity();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
