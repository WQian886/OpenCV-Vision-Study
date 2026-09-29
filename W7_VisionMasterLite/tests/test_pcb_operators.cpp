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

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <windows.h>   // SetConsoleOutputCP

#include "operators/pcb_inspect.h"
#include "operators/pcb_defect_judge.h"

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
// 把 dark_thresh 调到 0,missing 判定必须失效;baseline 依然零缺陷。
// 说明阈值真的在控制逻辑,不是摆设。
// ------------------------------------------------------------
static void test_threshold_actually_controls() {
    beginCase("阈值真的在控制判定(不是摆设)");
    PcbInspect inspect;
    QJsonObject iOut = inspect.run(inspect.defaultParams(),
                                   QJsonObject{{"image_path", "images/missing.png"}});

    PcbDefectJudge judge;
    QJsonObject loose = judge.defaultParams();
    loose["dark_thresh"] = 0;          // 放宽:任何图都不再判缺件
    QJsonObject lOut = judge.run(loose, iOut);
    QJsonArray lDefects = lOut.value("defects").toArray();
    bool stillMissing = false;
    for (const auto& d : lDefects)
        if (d.toObject().value("type").toString() == "missing") stillMissing = true;
    check(!stillMissing, "dark_thresh=0 时不再判缺件");

    QJsonObject tight = judge.defaultParams();
    tight["dark_thresh"] = 100000;     // 收紧:正常板子也会被判缺件
    QJsonObject tOut = judge.run(tight,
        inspect.run(inspect.defaultParams(),
                    QJsonObject{{"image_path", "images/baseline.png"}}));
    check(tOut.value("result").toString() == "FAIL",
          "dark_thresh=100000 时 baseline 也 FAIL(证明阈值生效)");
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
static void test_known_cross_talk() {
    beginCase("已知串扰:short.png 的短路桥会顺带触发 wrong");
    RunResult r = runPipeline("short.png");

    check(r.defectTypes.contains("short"), "仍然报出 short");
    check(r.defectTypes.contains("wrong"),
          "仍然报出 wrong(短路桥伸进了 R3 的 ROI)");

    // 确认这个 wrong 确实挂在 R3 上 —— 万一以后是别的元件串了,
    // 光看"类型还在"会误以为没事
    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    QString wrongOwner;
    for (const auto& d : defects) {
        QJsonObject o = d.toObject();
        if (o.value("type").toString() == "wrong")
            wrongOwner = o.value("component").toString();
    }
    check(wrongOwner == "R3",
          QString("串扰挂在 R3 上(实际 '%1')").arg(wrongOwner));
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
    test_known_cross_talk();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
