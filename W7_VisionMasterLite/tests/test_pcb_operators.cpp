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
// 运行:  w7_pcb_test.exe
//   工作目录 = build/test_work/pcb_operators/(ctest 给的),
//   里面有暂存进来的 images/ 和 recipes/。产物也只落在这里,
//   不再往源码树里写。
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
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <opencv2/opencv.hpp>
#include <windows.h>   // SetConsoleOutputCP
#include <algorithm>
#include <cmath>
#include <limits>

#include "operators/pcb_inspect.h"
#include "operators/pcb_defect_judge.h"

// ⚠ 颜色判据搬到共享核心了(common/pcb_core/pcb_color_ref.h),
//   命名空间 w7::pcbcolor -> pcbcore::pcbcolor。
//   这里 include 的是**核心里那一份**,路径靠 w7_pcb_core 的 PUBLIC
//   include 目录解析 —— 不要再 include "operators/pcb_color_ref.h",
//   那个文件已经不存在了。
//
//   ⚠ 下面那行 using 是**为了让下面的 pcbcolor:: 写法一个字都不用改** ——
//   抄副本的另一种形式是"连调用点一起抄一遍",那才是真的漂开。
#include "pcb_color_ref.h"

using namespace w7;
using namespace pcbcore;

// ============================================================
// 复杂板样本目录
//
// ⚠ 这里原来写的是绝对路径 "E:/qianwie/00study/OpenCV/W6_PCB_DefectProject/
//   images/complex/",6 处。写死盘符的问题不是"难看",是**仓库一挪位置就断** ——
//   而且断的方式很阴:路径拼不出来,断言照样报"通过"(读不到图不等于
//   判据不成立),整套复杂板用例变成空转,测试全绿。
//
// 这个仓库其它测试一律用相对路径("images/xxx.png"),基准是 ctest 给的
// WORKING_DIRECTORY = build/test_work/<套件名>/。这里跟着同一套约定:
//   images/complex/
// 仓库整体搬到哪,这个相对关系都成立。
//
// ⚠ 复杂板的数据在 W6 那边(那边才是生成器所在地),所以 CMake 的
//   w7_add_test() 会在**构建时**把 W6_PCB_DefectProject/images/complex
//   拷进测试工作目录的 images/complex/。
//   用构建时拷而不是运行时找仓库根,是因为运行时找路径 = 路径一断
//   就"读不到图",而读不到图的断言照样可能报"通过"(见上面那段话)。
//   拷进工作目录后,路径写死成相对路径反而是最稳的 —— 因为拷不动会
//   在构建阶段就炸,不会拖到测试阶段变成一句"找不到文件"。
// ============================================================
static const char* kComplexDir = "images/complex/";

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
    // 每一类缺陷具体点到了哪些元件。单独记一份,是因为"报了 missing"
    // 和"报的是**那个** missing"是两回事 —— 前者一个瞎子也能做到。
    QMap<QString, QStringList> defectSites;
};

static void collectDefects(RunResult* r) {
    r->defectTypes.clear();
    r->defectSites.clear();
    const QJsonArray defects = r->judgeOut.value("defects").toArray();
    for (const auto& d : defects) {
        const QJsonObject o = d.toObject();
        const QString t = o.value("type").toString();
        r->defectTypes.insert(t);
        r->defectSites[t].push_back(o.value("component").toString());
    }
}

static RunResult runPipeline(const QString& fileName) {
    RunResult r;
    const QString path = "images/" + fileName;

    PcbInspect inspect;
    r.inspectOut = inspect.run(inspect.defaultParams(),
                               QJsonObject{{"image_path", path}});

    PcbDefectJudge judge;
    r.judgeOut = judge.run(judge.defaultParams(), r.inspectOut);

    collectDefects(&r);
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

    collectDefects(&r);
    return r;
}

// ------------------------------------------------------------
// 用例清单从 images/manifest.csv 读,不再在代码里抄一份
// ------------------------------------------------------------
// 为什么:这份 CSV **原来一个代码都不读**(2026-10-14 之前),
// 用例列表是 tests 里手抄的一份 C++ 数组。两份会漂,而且漂了没人知道 ——
// 往 images/ 里加一张新缺陷图、改了 CSV,测试照样全绿。
//
// 这就是本项目记过两笔的"抄副本",只是这次抄的是**用例清单**而不是阈值。
// 现在 CSV 成了唯一入口,用例列表从它生成。
//
// 顺带钉一条反向不变式:images/ 里每一张图都必须在 CSV 里登记过。
// 只做"从 CSV 生成用例"是不够的 —— 那只能保证 CSV 里的都跑了,
// 仍然漏掉"多了一张图、没人知道它该判什么"。两张方向都要钉。
struct ManifestRow {
    QString file;
    QString defect;    // "none" 表示好板
    QString expected;  // PASS / FAIL
    QString site;      // 缺陷画在哪个元件上(真值表登记,空 = 好板)
    QString kind;      // 板型(只有 complex 清单有这一列)
    QString note;
};

// images/ 下这些是别的用例跑出来的中间产物,不是测试图,
// 不该要求它们登记在册。
//
// ⚠ 这个豁免是**必须留着的**,不是遗留:
//   算子算完会把画框图写回 images/(artifact_path.h 的"测试路径"分支),
//   而本文件下面那条"images/ 下没有漏登记的图"是在同一趟运行里查的。
//   没有这个豁免,算子刚画出来的框就会被当成"多了一张没人登记的图"。
//
// 但豁免名单不能随便放宽 —— 它白名单化的是**产物**,不是**样本**。
// 样本(8 张 demo 板)一张都不许豁免。
static bool isScratchImage(const QString& name) {
    return name.startsWith("w7_") || name.startsWith("_srcframe_");
}

// 读一张"用例清单"CSV —— **按列名**读,不按位置。
//
// 按列名是必须的:demo 板和 complex 板的清单列根本不一样
// (demo 是 file/defect/expected/site/note;
//  complex 是 file/kind/components/size/filesize_kb/expected/site)。
// 按位置读就得给两张表各写一套解析,加一列就错位 ——
// 而"加一列就错位"正是抄副本最常见的死法。
//
// 顺带把 expected 的写法统一了:complex 表里写的是 "FAIL:short",
// 这里统一拆成 expected=FAIL / defect=short,
// 免得下面每处都要自己判断前缀。
static QList<ManifestRow> readManifest(const QString& path, QString* err) {
    QList<ManifestRow> rows;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *err = "打不开 " + path;
        return rows;
    }
    const QString text = QString::fromUtf8(f.readAll());
    const QStringList lines = text.split(QRegularExpression("\r?\n"),
                                         Qt::SkipEmptyParts);

    // 表头 -> 列号
    QHash<QString, int> col;
    for (const QString& line : lines) {
        const QStringList cols = line.split(',');
        if (cols.isEmpty()) continue;
        if (cols[0].trimmed() == "file") {
            for (int i = 0; i < cols.size(); ++i) col[cols[i].trimmed()] = i;
            break;
        }
    }
    if (!col.contains("file") || !col.contains("expected")) {
        *err = path + " 里没有 file/expected 两列";
        return rows;
    }
    // 按列名取一格(列不存在 -> 空串,不当成"这格是空的")
    const auto cell = [](const QStringList& cols,
                         const QHash<QString, int>& c,
                         const char* name) -> QString {
        if (!c.contains(name)) return QString();
        const int i = c.value(name);
        if (i >= cols.size()) return QString();
        return cols[i].trimmed();
    };

    for (const QString& line : lines) {
        const QStringList cols = line.split(',');
        if (cols.size() < 3) continue;
        const QString c0 = cols[0].trimmed();
        if (c0 == "file" || c0.startsWith('#')) continue;   // 表头/注释
        ManifestRow r;
        r.file     = c0;
        r.expected = cell(cols, col, "expected");
        if (r.expected.startsWith("FAIL:")) {
            r.defect   = r.expected.mid(5).trimmed();
            r.expected = "FAIL";
        } else if (r.expected == "PASS") {
            r.defect = "none";
        } else {
            r.defect = cell(cols, col, "defect");
        }
        r.site = cell(cols, col, "site");
        r.kind = cell(cols, col, "kind");
        r.note = cell(cols, col, "note");
        rows << r;
    }
    if (rows.isEmpty()) *err = path + " 里没读到任何一行";
    return rows;
}

static void test_manifest_is_the_single_source() {
    beginCase("images/manifest.csv 是用例清单的唯一来源,且覆盖 images/ 下每一张图");
    QString err;
    const QList<ManifestRow> rows =
        readManifest("images/manifest.csv", &err);
    check(err.isEmpty(), QString("能读出用例清单(%1)").arg(err));
    if (rows.isEmpty()) return;

    // ① CSV 里的每一行都必须真的跑得起来(不是死条目)
    for (const auto& r : rows) {
        check(QFile::exists("images/" + r.file),
              QString("清单里的 %1 真的存在").arg(r.file));
    }

    // ② 反向:images/ 下没登记过的图要报出来。
    //    只有 ① 的话,"多了一张图没人知道该判什么"这条路是漏的 ——
    //    而那恰好是加新缺陷样本时最容易发生的事。
    QSet<QString> listed;
    for (const auto& r : rows) listed << r.file;
    QStringList unlisted;
    QDir d("images");
    for (const QString& name : d.entryList(QStringList{"*.png"}, QDir::Files)) {
        if (isScratchImage(name)) continue;
        if (!listed.contains(name)) unlisted << name;
    }
    check(unlisted.isEmpty(),
          QString("images/ 下没有漏登记的图(漏了:%1)")
              .arg(unlisted.isEmpty() ? QString("(无)") : unlisted.join(" ")));

    // ③ 好板/废板两类都得有,否则这份清单学不到东西
    int pass = 0, fail = 0;
    for (const auto& r : rows) {
        if (r.expected == "PASS") ++pass;
        else if (r.expected == "FAIL") ++fail;
    }
    check(pass >= 1 && fail >= 1,
          QString("清单里两类都要有(好板 %1 张,废板 %2 张)")
              .arg(pass).arg(fail));
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
    QString err;
    const QList<ManifestRow> rows = readManifest("images/manifest.csv", &err);
    if (rows.isEmpty()) {
        check(false, QString("用例清单读不出来(%1)").arg(err));
        return;
    }
    // 清单里 expected=FAIL 的行才是缺陷样本。
    // 原来这里是手抄的一份 7 行数组 —— 和 CSV 内容一样,但没有任何机制
    // 保证它们一直一样。
    int cases = 0;
    for (const auto& c : rows) {
        if (c.expected != "FAIL") continue;
        ++cases;
        beginCase(QString("%1 必须判出 %2").arg(c.file, c.defect));
        RunResult r = runPipeline(c.file);
        check(r.judgeOut.value("result").toString() == "FAIL",
              "结论 FAIL");
        check(r.defectTypes.contains(c.defect),
              QString("命中缺陷类型 %1  (实际:%2)")
                  .arg(c.defect).arg(r.defectTypes.values().join(",")));
        // 点名到**真值表说的那个元件**才算抓到。
        //
        // 只看类型不够:别的站点报一条同类型的缺陷,也能把结论撑成 FAIL。
        // 期望的元件从 manifest.csv 读(c.site)—— 不在代码里再抄一份。
        if (!c.site.isEmpty()) {
            check(r.defectSites.value(c.defect).contains(c.site),
                  QString("%1 的 %2 应点名 %3 (实际点名:%4)")
                      .arg(c.file, c.defect, c.site,
                           r.defectSites.value(c.defect).join("/")));
        }
    }
    // 少一行用例,这个函数就少跑一张图,而上面还是全绿。
    check(cases >= 7,
          QString("缺陷样本不能少于 7 张(实际 %1 张 —— 是清单被改小了,还是用例漏了?)")
              .arg(cases));
}

// ------------------------------------------------------------
// 用例 3:短路必须点名到具体元件
//
// ⚠ 2026-10-20 之前这条是反的:那时 short 是**全局**规则,
//   缺陷.component 留空,这条断言就钉"它必须留空"。
//   短路判据重做之后 short 变成**按元件**的规则
//   (同一个元件的两个端子之间有金属 = 短路),
//   缺陷必须挂在那个元件上 ——
//   不然产线上只知道"这板废了",不知道"哪个元件废了",
//   还得人拿块表挨个量。
//
// 期望的元件从 **manifest.csv 读**(真值表里那一列),
// 不在代码里写死 "R2" —— 写死就成第二份真值表了。
// ------------------------------------------------------------
static void test_short_names_the_component() {
    beginCase("short.png 的短路缺陷必须点名到元件");
    QString err;
    const QList<ManifestRow> rows = readManifest("images/manifest.csv", &err);
    if (rows.isEmpty()) {
        check(false, QString("用例清单读不出来(%1)").arg(err));
        return;
    }
    const ManifestRow* shortRow = nullptr;
    for (const auto& m : rows) {
        if (m.defect == "short") { shortRow = &m; break; }
    }
    check(shortRow != nullptr, "真值表里有 short 那一行");
    if (!shortRow) return;
    check(!shortRow->site.isEmpty(),
          "真值表登记了 short 的元件(不能是空的)");
    if (shortRow->site.isEmpty()) return;

    RunResult r = runPipeline(shortRow->file);
    const QJsonArray defects = r.judgeOut.value("defects").toArray();
    int shortCnt = 0;
    for (const auto& d : defects) {
        QJsonObject o = d.toObject();
        if (o.value("type").toString() != "short") continue;
        ++shortCnt;
        check(o.value("component").toString() == shortRow->site,
              QString("短路点名到 %1 (实际 '%2')")
                  .arg(shortRow->site, o.value("component").toString()));
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
// 用例 9:阈值给成怪形状时报错,不崩
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
//
// ⚠ 2026-10-09 参数形状变了(hsv_low/hsv_high 两个数组 -> 四个标量),
//    所以这一版的用例也跟着换。**换掉不等于可以不测** ——
//    守卫还在,只是挡的东西从"数组太短"变成"标量类型不对 + 取值越界"。
//    变异验证 M11/M12 就是改坏这两道新守卫。
// ------------------------------------------------------------
static void test_hsv_param_guarded() {
    beginCase("金层阈值形状不对时报错,不崩");

    PcbInspect inspect;
    const QJsonObject in{{"image_path", "images/baseline.png"}};

    struct Case {
        const char* what;
        const char* key;
        QJsonValue bad;
        const char* expect;         // 报错里应该点名的字段
    };
    const Case cases[] = {
        {"H 下界是数组",     "hsv_h_low",  QJsonArray{85},       "hsv_h_low"},
        {"H 下界是字符串",   "hsv_h_low",  QJsonValue("85"),     "hsv_h_low"},
        {"S 下界不是数字",   "hsv_s_low",  QJsonValue("80"),     "hsv_s_low"},
        {"H 上界缺失",       "hsv_h_high", QJsonValue(),         "hsv_h_high"},
        {"V 倍数是字符串",   "mask_v_mul", QJsonValue("0.8"),    "mask_v_mul"},
        // 取值越界:类型对但数不对。以前没有这一层,现在有。
        {"H 下界超过 179",   "hsv_h_low",  QJsonValue(300.0),    "H 区间不合法"},
        {"H 下界不小于上界", "hsv_h_low",  QJsonValue(120.0),    "H 区间不合法"},
        {"S 下界越界",       "hsv_s_low",  QJsonValue(999.0),    "S 下界"},
        {"V 倍数为负",       "mask_v_mul", QJsonValue(-1.0),     "倍数"},
        {"V 倍数过大",       "mask_v_mul", QJsonValue(10.0),     "倍数"},
    };

    for (const auto& c : cases) {
        QJsonObject p = inspect.defaultParams();
        p[c.key] = c.bad;
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

    // 老存档:以前是 hsv_low/hsv_high 两个数组。现在**故意不读它们** ——
    // 沿用旧名字的话,里面的 80 会被当成 80 倍板面色,金层恒空。
    // 宁可退回默认让人看出"没生效",也不能让用户以为自己的数在起作用。
    {
        QJsonObject p = inspect.defaultParams();
        p["hsv_low"]  = QJsonArray{85, 80, 80};
        p["hsv_high"] = QJsonArray{115, 255, 255};
        QJsonObject out = inspect.run(p, in);
        check(!out.contains("error"),
              QString("老存档的 hsv_low/hsv_high 被忽略而不是报错"
                      "(实际 error=\"%1\")").arg(out.value("error").toString()));
        check(out.value("count").toInt() == 6,
              QString("忽略旧参数后仍定位到 6 个元件(实际 %1)")
                  .arg(out.value("count").toInt()));
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

// gold_pct 在 1.0x 上的基准值。声明放在用例前面:它要跨多个 case 比对,
// 而每处重新算一遍等于把这个量抄第二份。
static QString refGoldPct;

// ------------------------------------------------------------
// 用例 14:两个算子的金层掩膜必须是同一份
//
// 这条是 2026-10-09 实测出来的抄副本第四例,而且是**最隐蔽的一例**:
// 上一轮把判定的金层 V 下界改成相对式(0.8 x 板面V),定位那边还写着
// 绝对 V>=80。两份 H/S 完全相同、只有 V 下界不同。
//
// 为什么所有测试都是绿的:
//   · 唯一能看见差异的字段是 gold_px,而它**全仓库没有任何消费者**
//   · 端到端 56 格(8 张 x 7 档亮度)照跑照绿
//   · 亮度测试量的是 0.40~1.60,只在 k=0.4 才切掉 24 个像素,
//     而 k=0.4 那格的判定结论没变
//
// 所以"两份定义漂开了"这件事,没有任何一条已有测试能发现。
// 唯一能发现的是**直接断言两个算子用同一份定义**。
// ------------------------------------------------------------
static void test_gold_mask_shared_between_operators() {
    beginCase("定位与判定的金层掩膜是同一份定义");
    QDir().mkpath("build/_gm");

    // ① 共用实现本身:两个算子调的是同一个函数,不是各自拼 Scalar。
    //    变异验证 M13 把 judge 的 bandMask 换成手写 inRange -> 这里红。
    PcbInspect inspect;
    const QJsonObject io = inspect.run(inspect.defaultParams(),
                                       QJsonObject{{"image_path", "images/baseline.png"}});
    const QJsonArray cs = io.value("components").toArray();
    check(cs.size() == 6, QString("定位到 6 个元件(实际 %1)").arg(cs.size()));

    // gold_pct 存在,且**量纲是占 ROI 面积的百分比**。
    //
    // ⚠ 变异验证 M17 连着栽了两次,两次都是**我把尺子拿错了**:
    //
    //   第一版:只检查"字段存在且在 0~100 之间"。裸像素数也落在那个范围里,
    //           于是全绿。
    //   第二版:加了一条"p/100*ROI面积 <= ROI面积"。可 M17 的真实变异是
    //           把 `100.0 *` 去掉,/ roiArea 那半截还在下一行,
    //           于是它产出的是**分数**(0~1),不是像素数 —— 而分数当然
    //           也过 0~100,也过面积上限。**我按错误的假设写检查,
    //           等于没检查。**
    //
    // 正确的做法:别拿算子自己的输出去验证算子自己的输出。
    // 这里**独立重算**一遍 —— 测试自己调 pcbcolor::estimateBoardBase +
    // bandMask,拿自己算出的金像素数除以 ROI 面积,再和算子报的 gold_pct 比。
    // 基准来自另一条计算路径,单位错、系数错、面积错,三样都会红。
    {
        cv::Mat img = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
        const cv::Vec3b base = pcbcolor::estimateBoardBase(img);
        cv::Mat hsv;
        cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
        const cv::Mat gold = pcbcolor::opened(
            pcbcolor::bandMask(hsv, base, pcbcolor::goldBand()),
            pcbcolor::kernelScale(img));

        bool ok = true;
        QStringList got;
        for (const auto& v : cs) {
            const QJsonObject c = v.toObject();
            if (!c.contains("gold_pct")) { ok = false; got << "缺字段"; break; }
            const cv::Rect roi(c.value("x").toInt(), c.value("y").toInt(),
                               c.value("w").toInt(), c.value("h").toInt());
            const double area = double(roi.width) * double(roi.height);
            if (area <= 0) { ok = false; got << "ROI 面积 0"; break; }
            const double want = 100.0 * cv::countNonZero(gold(roi)) / area;
            const double gotv = c.value("gold_pct").toDouble(-1);
            const double diff = std::fabs(gotv - want);
            got << QString("%1 算子%2/独立算%3")
                       .arg(c.value("label").toString())
                       .arg(gotv, 0, 'f', 4).arg(want, 0, 'f', 4);
            // 1e-6 的容差只够吸收浮点末位差,容不下任何单位错误。
            if (diff > 1e-6) { ok = false; }
        }
        check(ok,
              QString("gold_pct 的量纲是占 ROI 面积的百分比(独立重算比对:%1)")
                  .arg(got.join(" ")));
    }

    // ② 跨亮度:同一张图改亮度,gold_pct 必须不动。
    //    实测依据:绝对 V>=80 在 k=0.4 时 R1 的 gold_px 168 -> 144,
    //    相对式在 0.4~1.6 全档稳在 168。这条钉的就是"别退回绝对"。
    //
    // 基准取**未改动的原图**(上面那次 inspect.run 已经量过了),
    // 不能在循环里等 k=1.0 那一档再赋值 —— 循环顺序一变,
    // 前面的档就会拿去比一个空串,然后无论实现对不对都红。
    {
        QStringList base;
        for (const auto& v : cs)
            base << QString::fromUtf8("%1").arg(
                v.toObject().value("gold_pct").toDouble(), 0, 'f', 4);
        refGoldPct = base.join(" ");
    }
    for (double k : {0.4, 0.8, 1.4, 1.6}) {
        const QString dst = QString("build/_gm/baseline_k%1.png").arg(k, 0, 'f', 2);
        cv::Mat img = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
        cv::Mat s;
        img.convertTo(s, -1, k, 0.0);      // 只改亮度,尺寸不变
        cv::imwrite(dst.toStdString(), s);

        const QJsonObject o2 = PcbInspect().run(PcbInspect().defaultParams(),
                                                QJsonObject{{"image_path", dst}});
        const QJsonArray c2 = o2.value("components").toArray();
        QStringList sig;
        for (const auto& v : c2)
            sig << QString::fromUtf8("%1").arg(v.toObject().value("gold_pct").toDouble(), 0, 'f', 4);
        const QString now = sig.join(" ");
        check(now == refGoldPct,
              QString("亮度 %1x 的 gold_pct 不变(实际 %2 != %3)")
                  .arg(k, 4, 'f', 1).arg(now, refGoldPct));
    }

    // ③ 极暗曝光下 gold_pct 仍然不变 —— 这条才是钉住"板面基色逐图估计"的。
    //
    // ⚠ 变异验证 M14(把 estimateBoardBase 换成写死的 cv::Vec3b(50,90,40))
    //   第一版是全绿的。第一版我写的是"把图换个板色,gold_pct 必须变" ——
    //   实测**没变**(2.9167 49.5602 ... 一模一样)。原因量出来了:
    //   那个换色把板面从 (50,90,40) 变成 (80,45,52),V 下界只从 72 挪到 64;
    //   而金层像素的 V 在 180~255,72→64 这个位移**跨不过任何一个像素**。
    //   换句话说:我挑的刺激够不到那道门。判据没写错,是尺子不够长。
    //
    // 为什么 0.4~1.6 那一档也抓不到 M14:写死的 50/90/40 给出 V_lo=72,
    // 而真实基色在 k 档给出 0.8*90*k。k=0.4 时两者都是 72(50/90/40 里
    // G=90,0.8*90=72;写死也是 72)—— 正好撞平,所以看起来"碰巧对"。
    // k 再往下,写死的 72 就开始高于真实基色给出的下界,金层被切掉。
    //
    // 实测:k=0.25 时金层 V 落到 45~64,真实 V_lo = 0.8*90*0.25 = 18
    // (全留);写死常量仍是 72(全被切掉 -> gold_pct 全 0)。
    // 判据是 0.25 档 gold_pct 必须和原图逐位相同。
    {
        const double k = 0.25;
        const QString dst = QString("build/_gm/baseline_k%1.png").arg(k, 0, 'f', 2);
        cv::Mat img = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
        cv::Mat s;
        img.convertTo(s, -1, k, 0.0);
        cv::imwrite(dst.toStdString(), s);

        const QJsonObject o2 = PcbInspect().run(PcbInspect().defaultParams(),
                                                QJsonObject{{"image_path", dst}});
        QStringList sig;
        const QJsonArray c2 = o2.value("components").toArray();
        for (const auto& v : c2)
            sig << QString::fromUtf8("%1").arg(
                v.toObject().value("gold_pct").toDouble(), 0, 'f', 4);
        check(sig.join(" ") == refGoldPct,
              QString("0.25x 极暗曝光下 gold_pct 仍与原图逐位相同 —— "
                      "证明 V 下界是跟着这张图的板面基色走的,不是写死的 72"
                      "(实际 %1 != %2)").arg(sig.join(" "), refGoldPct));
    }
}

// ------------------------------------------------------------
// 用例 15:min_area / mergeDist 不得退回绝对像素
// 实测(只改分辨率):绝对 80px 下 auto_contour 元件数
//   0.5x->6  0.75x->9  1.0x->14  1.25x->18  1.5x->18  2.0x->18
// 同一块板在不同分辨率上被切成 6 个或 18 个元件。
// 改成占全图 0.05% 后:6 / 7 / 10 / 10 / 10 / 10。
// ------------------------------------------------------------
static void test_auto_contour_scale_stable() {
    beginCase("auto_contour 换分辨率时元件数趋于稳定");

    PcbInspect inspect;
    QJsonObject p = inspect.defaultParams();
    p["mode"] = "auto_contour";

    const double scales[] = {0.5, 1.0, 1.5, 2.0};
    QVector<int> counts;
    for (double s : scales) {
        const QString dst = QString("build/_gm/ac_x%1.png").arg(s, 0, 'f', 2);
        cv::Mat img = cv::imread("images/baseline.png", cv::IMREAD_COLOR);
        cv::Mat r;
        cv::resize(img, r, cv::Size(), s, s,
                   s < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
        cv::imwrite(dst.toStdString(), r);
        counts << inspect.run(p, QJsonObject{{"image_path", dst}}).value("count").toInt();
    }
    const int n05 = counts.at(0), n10 = counts.at(1),
              n15 = counts.at(2), n20 = counts.at(3);

    // 把实测值打出来 —— 这几个数本身就是结论的一部分,
    // 而且下面每条断言红了的时候,得靠它才看得见差在哪。
    qInfo().noquote() << "    auto_contour 元件数 0.5x/1.0x/1.5x/2.0x ="
                      << n05 << n10 << n15 << n20;

    // ⚠ 变异验证教训(四条,前三条都是我自己踩的):
    //
    // ① 第一版写的是"2.0x 不超过 0.5x 的两倍"。M16(mergeDist 退回 90.0f)
    //   和 M18(核写死 3x3)**都活下来了** —— 基准跟着分子一起漂,
    //   比值不变。这叫"拿会动的尺子量东西"。
    // ② 第二版改成"1.5x 与 2.0x 必须相等",这次是**没量就写**:实测是
    //   10 vs 11,不相等,于是这条自己先红了。探针那张表的档位是
    //   0.4/0.8/1.4/1.6,和这里的 0.5/1.0/1.5/2.0 不是同一组数。
    // ③ 第三版钉死上限 12 —— M16/M18 还是活着,因为它们各自只动**一档**
    //   (M16 把 0.5x 从 7 变成 6,M18 把 2.0x 从 11 变成 10),
    //   而"不超过 12"对 6 和 10 一视同仁地放行。
    // ④ 第四版逐档钉死 —— 这才是站得住的写法。
    //
    // ⚠ 2026-10-21:数据重做之后,这四个数**全变了**。
    //   demo 板的 IC 焊盘搬到了本体外面(生成器改动,见
    //   W6_PCB_DefectProject/gen_pcb_dataset.py),
    //   基线从 7/10/10/11 变成 6/10/10/10。
    //   黄金值跟着数据走 —— 这不是"改数迁就实现",是
    //   "数据变了,基准必须重量,并且**重新验证变异还杀不杀得动**"。
    //
    //   重量的结果(同一张 baseline.png,同一套探针):
    //     基线                6  10  10  10
    //     M16 mergeDist=90.0f 5  10  10  10   <- 0.5x 过度聚类,糊成 5 团
    //     M19 mergeDist x3    3   4   4   4   <- 聚类过头,整板糊成几团
    //     M18 核写死 3x3      6  10  10  10   <- **活下来了**
    //
    //   M18 为什么活下来(这次是想清楚了才写的,不是"先记一笔"):
    //     demo 板上只有**大焊盘**,没有细走线。核从 3x3 变成 5x5/7x7
    //     只会把焊盘边缘多削掉一圈,焊盘还是焊盘,连通域个数不变 ——
    //     "元件数"这个读数**对核尺寸不敏感**。
    //     所以这条断言**从一开始就抓不住** M18;老注释里那句
    //     "2.0x 掉到 10 = 开运算核写死 3x3"在**新数据下已经不成立**。
    //     不改它,它就成了一条会骗人的注释 —— 而注释骗人比测试红更贵,
    //     因为红会有人来修,骗不会。
    //
    //   那 M18 归谁抓:**目前没有人抓**。
    //   本轮试过"换一个读数去守":complex 三块板有 3px 宽的走线,
    //   核写死 -> 走线在金层掩膜里活下来 -> 走线把焊盘的铜面积撑大
    //   -> 焊少读数不再是 0% -> "好板读数恰好 0%"就能抓住。
    //   **实测不成立**:那些走线没有落进登记过的端子框里
    //   (走线在底层、被顶层元件压住),焊少读数仍然是 0.0%。
    //   三个"核写死"的变异(核心路径 / 算子路径 / 判定算子路径)
    //   实测**全都活着** —— 记在 notes 的「登记的欠账」里。
    //
    //   教训:一个读数对什么不敏感,就别指望它管另一件事;
    //   而"换个读数来管"这句话,**先量了再写进注释**
    //   (我一度把没验证过的说法写在这里,实测发现是错的,已订正)。
    const int want05 = 6, want10 = 10, want15 = 10, want20 = 10;
    check(n05 == want05 && n10 == want10 && n15 == want15 && n20 == want20,
          QString("auto_contour 逐档元件数钉死 6/10/10/10(实测 %1/%2/%3/%4)。"
                  "0.5x 掉到 5 = mergeDist 退回绝对 90(过度聚类);"
                  "整体塌到 4 附近 = 聚类过头;"
                  "核尺寸写死 3x3 这条**抓不住**(demo 板没有细走线,读数不敏感)")
              .arg(n05).arg(n10).arg(n15).arg(n20));
}

// ================================================================
// 用例 15:换板型 —— 另外三块板走配方跑
//
// 为什么必须有这条:
//   前面 14 个用例全部只跑 images/ 里那 8 张 1000x700 的 demo 板。
//   而"板型可切换"这个功能真正的验收对象是 W6 那三块 1200x900 的复杂板
//   (power 16 元件 / mcu 22 / signal 18)。它们以前只能靠一个临时探针验,
//   探针跑完就删 —— 也就是说**仓库里没有任何东西守着它们**:
//   配方哪天漂了、阈值哪天又调坏了,ctest 一声不响,只有产线会发现。
//
// 参照物是 W6 自带的真值表 W6_PCB_DefectProject/images/complex/
// complex_manifest.csv,不另抄一份期望出来。
// ================================================================

// 跑一块板:用板型配方定位,再判定。
static RunResult runBoard(const QString& recipeName, const QString& absPath) {
    RunResult r;
    PcbInspect inspect;
    QJsonObject p = inspect.defaultParams();
    p["recipe"] = QString("recipes/") + recipeName;   // 换板型只改这一个字段
    r.inspectOut = inspect.run(p, QJsonObject{{"image_path", absPath}});
    PcbDefectJudge judge;
    r.judgeOut = judge.run(judge.defaultParams(), r.inspectOut);
    collectDefects(&r);
    return r;
}

static void test_board_switch_good_boards_clean() {
    beginCase("三块别的好板换配方后必须 PASS 且零缺陷");
    // 漏报的代价不对称:好板被误判成废板只是停线,
    // 废板被判成好板是**流到下工序**,所以这两条都得钉死。
    struct Good { const char* recipe; const char* file; int count; };
    const Good goods[] = {
        {"power.json",  "power_baseline.png",  16},
        {"mcu.json",    "mcu_baseline.png",    22},
        {"signal.json", "signal_baseline.png", 18},
    };
    for (const auto& g : goods) {
        const QString dir = kComplexDir;
        RunResult r = runBoard(QString::fromUtf8(g.recipe), dir + QString::fromUtf8(g.file));
        const QString who = QString::fromUtf8(g.file);
        check(!r.inspectOut.contains("error"),
              QString("%1 定位无错误 (%2)")
                  .arg(who, r.inspectOut.value("error").toString("无")));
        check(r.inspectOut.value("count").toInt() == g.count,
              QString("%1 框出 %2 个元件 (实际 %3)")
                  .arg(who).arg(g.count).arg(r.inspectOut.value("count").toInt()));
        check(r.judgeOut.value("defect_count").toInt() == 0,
              QString("%1 零缺陷 (实际 %2) 命中:%3")
                  .arg(who).arg(r.judgeOut.value("defect_count").toInt())
                  .arg(r.defectTypes.values().join(",")));
        // 好板的焊少读数必须**恰好 0%**
        //
        // ⚠ 2026-10-21 加的。同一个元件的焊盘一样大,读数就该是 0。
        //   钉"恰好 0"而不是"小于阈值"(阈值 33):中间那一大截余量看着安全,
        //   其实会盖住一类 bug —— 采样框多掐进 1px 金属,读数变成 4%,
        //   判定不变,但"这个焊盘到底有多少铜"这个数已经不诚实了。
        //   读数不诚实,拿它标出来的阈值就不可信。
        //
        //   变异验证:把端子框半径翻倍,读数变成 power 6.3% / mcu 23.8%,
        //   **判定不变**(都还在 33 以下)—— 所以只断言"没报缺陷"是抓不住的,
        //   必须钉读数。
        {
            int n = 0;
            double worst = 0.0;
            for (const auto& m : r.judgeOut.value("metrics").toArray()) {
                const QJsonObject mo = m.toObject();
                if (mo.value("rule").toString() != "insufficient") continue;
                ++n;
                worst = qMax(worst, mo.value("value").toDouble());
            }
            check(n > 0,
                  QString("%1 的焊少规则必须真的跑了(实测 %2 条读数 —— "
                          "0 条就是\"没查\",不能算好板)")
                      .arg(who).arg(n));
            check(worst == 0.0,
                  QString("%1 是好板,焊少读数必须恰好 0%%(实际最大 %2)")
                      .arg(who, QString::number(worst, 'f', 4)));
        }
    }
}

static void test_board_switch_defect_boards() {
    beginCase("三块板的 10 张废板:能抓的必须抓,抓不到的必须写明");
    // 期望值全部来自 W6 的真值表 complex_manifest.csv:
    //   文件名、缺陷类型、点位元件、连哪张配方(kind + ".json")
    // —— 这里一个都不抄。
    //
    // ⚠ 2026-10-20 之前这里有一张手抄的表(recipe/file/type/site 四列),
    //   内容跟真值表一样,但**没有任何机制**保证它们一直一样。
    //   真值表补上 site 那一列之后,这张表就退化成了纯抄副本 ——
    //   抄的东西迟早会漂,而且漂了没人知道。所以删掉它,直接读真值表。
    //
    // 唯一**不在**真值表里的是 knownMiss:已知漏检(废板被判成合格)。
    // 那张表现在是空的(短路、极性判据重做之后 6 张废板全都抓得到),
    // 但**机制留着** —— 它是"过期检测器":
    // 哪天判据又退化,就有条目的"期望 PASS"自己先红,提醒你把问题当回事。
    //
    // 各条为什么调阈值解决不了,实测数据记在 pcb_defect_judge.cpp
    // 的 polarity / short 两段,不在这儿重复。
    struct KnownMiss {
        const char* file;    // complex_manifest.csv 里的文件名
        const char* why;     // 抓不到的结构性原因
    };
    const QList<KnownMiss> knownMiss = {
        // (空 —— 2026-10-20:短路判据重做 + 极性判据重做之后,
        //  6 张废板全部抓得到,而且都点名到真值表说的那个元件)
    };

    QString err;
    const QString complexDir = QString::fromUtf8(kComplexDir);
    const QList<ManifestRow> rows =
        readManifest(complexDir + "complex_manifest.csv", &err);
    check(err.isEmpty(), QString("读得到 complex_manifest.csv(%1)").arg(err));
    if (rows.isEmpty()) return;

    int defectBoards = 0;
    for (const auto& m : rows) {
        if (m.expected != "FAIL") continue;      // 好板另有一组用例
        ++defectBoards;
        const QString who  = m.file;
        const QString type = m.defect;
        const QString recipe = m.kind + ".json";

        const KnownMiss* km = nullptr;
        for (const auto& k : knownMiss) {
            if (who == QString::fromUtf8(k.file)) { km = &k; break; }
        }

        RunResult r = runBoard(recipe, complexDir + who);
        const QString got = r.judgeOut.value("result").toString("无结论");
        if (km) {
            check(got == "PASS",
                  QString("【已知漏检】%1 期望 PASS(实测 %2)。原因:%3")
                      .arg(who, got, QString::fromUtf8(km->why)));
            continue;
        }
        check(got == "FAIL", QString("%1 判废 (实际 %2)").arg(who, got));
        check(r.defectTypes.contains(type),
              QString("%1 命中 %2 (实际命中:%3)")
                  .arg(who, type, r.defectTypes.values().join(",")));
        // 点名到具体元件才算抓到。只看类型的话,一个瞎子也能通过 ——
        // 2026-10-20 之前就瞒过去过一次:整张板 FAIL:missing,
        // 但报的根本不是 C2。
        if (!m.site.isEmpty()) {
            check(r.defectSites.value(type).contains(m.site),
                  QString("%1 的 %2 应点名 %3 (实际点名:%4)")
                      .arg(who, type, m.site,
                           r.defectSites.value(type).join("/")));
        }
    }
    // 少一行用例,这个函数就少跑一块板,而上面还是全绿。
    //
    // ⚠ 2026-10-21:6 -> 9 —— complex 三块板各补了一张「焊少」真值图
    //   (power_insufficient / mcu_insufficient / signal_insufficient)。
    //   这三张图正是本轮的目标:insufficient 判据重做之前,这三块板上
    //   这条规则**整条没跑**,自然也造不出"焊少"的样本。
    //   9 -> 10 —— 再补一张 mcu_insufficient_xtal(晶振 Y1 焊少):
    //   晶振只有 2 个脚,而"中位数怎么取"这种差别**只在脚数为偶数时**
    //   才有分歧(取平均 86.1% / 取大的 92.6% / 取小的 0%)。
    //   没有这张图,中位数的定义就没人守。
    check(defectBoards == 10,
          QString("complex 板的废板不能少于 10 张(实际 %1 张 —— 是清单被改小了?)")
              .arg(defectBoards));
}

// ============================================================
// 彩色本体(CAP*)的 missing 必须问 body_pct,不能问暗像素
//
// 背景在 operators/pcb_defect_judge.cpp 的 missingAsksBodyPresence()。
// 这里钉的不是"某个阈值是多少"(那是标定值,会随板子变),而是
// **电容走的是哪条判据** —— 这条一旦被改回去,症状是好板在
// σ=图宽千分之一就集体误报缺件,而现有 12 套件全绿,因为
// k=0 时两条判据的结论恰好一样。
//
// 所以本用例必须做两件事:
//   1. 断言 CAP 站点的 missing 读数**等于 body_pct**(量纲能对上,
//      说明问的确实是"框里有没有东西");
//   2. 把 body_min_pct 推到两个极端,证明这个旋钮是活的 ——
//      只测"默认值下结论对"会退化成"这个参数根本没人看",
//      gold_px 和 body_pct 都是这么潜伏了很久的。
// ============================================================
static void test_cap_missing_uses_body_pct() {
    const QString dir = kComplexDir;

    // 从 metrics 里取出某个站点某条规则的读数;找不到返回 NaN。
    auto metricOf = [](const QJsonObject& judgeOut, const QString& label,
                       const QString& rule) {
        for (const auto& m : judgeOut.value("metrics").toArray()) {
            const QJsonObject mo = m.toObject();
            if (mo.value("component").toString() == label
                && mo.value("rule").toString() == rule)
                return mo.value("value").toDouble();
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    auto bodyOf = [](const QJsonObject& inspectOut, const QString& label) {
        for (const auto& c : inspectOut.value("components").toArray()) {
            const QJsonObject co = c.toObject();
            if (co.value("label").toString() == label)
                return co.value("body_pct").toDouble();
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    // 从 inspect 的输出**自己重算一遍中位数**,别拿算子的输出去验证算子的输出
    // —— 变异会两边一起变,恒等,断言永远绿(见变异教训)。
    auto medianBodyOf = [](const QJsonObject& inspectOut) {
        std::vector<double> v;
        for (const auto& c : inspectOut.value("components").toArray()) {
            const QJsonObject co = c.toObject();
            if (co.contains("body_pct")) v.push_back(co.value("body_pct").toDouble());
        }
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    auto unitOf = [](const QJsonObject& judgeOut, const QString& label,
                     const QString& rule) {
        for (const auto& m : judgeOut.value("metrics").toArray()) {
            const QJsonObject mo = m.toObject();
            if (mo.value("component").toString() == label
                && mo.value("rule").toString() == rule)
                return mo.value("unit").toString();
        }
        return QString();
    };

    RunResult good = runBoard("power.json", dir + "power_baseline.png");
    check(good.judgeOut.value("result").toString() == "PASS",
          QString("power 好板零缺陷 (实际 %1,命中:%2)")
              .arg(good.judgeOut.value("result").toString(),
                   good.defectTypes.values().join(",")));

    // C2 是 CAP_BIG。判据必须问"占同板中位数多少",不是绝对百分比 ——
    // 绝对阈值要求 T ∈ (55.9, 49.2),空集,k>=4 必漏(实测见 .cpp)。
    const double bodyC2   = bodyOf(good.inspectOut, "C2");
    const double medBoard = medianBodyOf(good.inspectOut);
    const double missC2   = metricOf(good.judgeOut, "C2", "missing");
    check(qAbs(missC2 - bodyC2 / medBoard) < 0.005,
          QString("C2 的 missing 读数应等于 body_pct/同板中位数"
                  "(实测 missing=%1, body_pct=%2, 中位数=%3, 应为 %4)")
              .arg(missC2, 0, 'f', 3).arg(bodyC2, 0, 'f', 2)
              .arg(medBoard, 0, 'f', 2).arg(bodyC2 / medBoard, 0, 'f', 3));
    check(unitOf(good.judgeOut, "C2", "missing") == QString::fromUtf8("比同板中位数"),
          QString("C2 的 missing 单位应标明走的是比值(实际 \"%1\")")
              .arg(unitOf(good.judgeOut, "C2", "missing")));

    // 暗像素那条读数仍然要单独记着 —— 换判据不等于把诊断信息扔了。
    const double darkC2 = metricOf(good.judgeOut, "C2", "dark_pct");
    check(darkC2 > 0.0 && qAbs(darkC2 - missC2) > 0.01,
          QString("C2 的暗像素读数应另记一条且与 missing 不同"
                  "(实测 dark_pct=%1, missing=%2)")
              .arg(darkC2, 0, 'f', 2).arg(missC2, 0, 'f', 2));

    // 黑本体那边必须**不受影响**:R1 是 RES,still 走暗像素。
    const double missR1 = metricOf(good.judgeOut, "R1", "missing");
    const double bodyR1 = bodyOf(good.inspectOut, "R1");
    check(qAbs(missR1 - bodyR1) > 0.01,
          QString("R1(RES)仍应走暗像素判据,不该等于 body_pct"
                  "(实测 missing=%1, body_pct=%2)")
              .arg(missR1, 0, 'f', 2).arg(bodyR1, 0, 'f', 2));

    // 旋钮是活的吗?两个方向都要,否则这参数删掉测试照样绿。
    {
        PcbInspect inspect;
        QJsonObject ip = inspect.defaultParams();
        ip["recipe"] = "recipes/power.json";
        PcbDefectJudge judge;

        auto hasMissing = [&](const QJsonObject& jo) {
            for (const auto& d : jo.value("defects").toArray())
                if (d.toObject().value("type").toString() == "missing")
                    return true;
            return false;
        };
        const QJsonObject ioBad = inspect.run(
            ip, QJsonObject{{"image_path", dir + "power_missing.png"}});
        const QJsonObject ioGood = inspect.run(
            ip, QJsonObject{{"image_path", dir + "power_baseline.png"}});

        // 放宽:比值下限 0 -> 什么都不可能低于 0。
        QJsonObject jp = judge.defaultParams();
        jp["body_ratio_min"] = 0;
        check(!hasMissing(judge.run(jp, ioBad)),
              "body_ratio_min=0 时不再判缺件");

        // 收紧:下限 1.0 -> 任何不在中位数之上的站点都会被报出来。
        QJsonObject jp2 = judge.defaultParams();
        jp2["body_ratio_min"] = 1.0;
        check(judge.run(jp2, ioGood).value("result").toString() == "FAIL",
              QString("body_ratio_min=1.0 时好板也应 FAIL(证明阈值生效,实际 %1)")
                  .arg(judge.run(jp2, ioGood).value("result").toString()));

        // 站点不足时必须退回绝对判据 —— 并且说清走了哪条。
        // 只测正常路径的话,这个兜底分支永远没人走过。
        QJsonObject jp3 = judge.defaultParams();
        jp3["body_min_sites"] = 999;
        jp3["body_min_pct"] = 0;
        const QJsonObject jo3 = judge.run(jp3, ioBad);
        check(!hasMissing(jo3),
              "站点不足退回绝对判据后,body_min_pct=0 应能压住缺件(兜底分支可达)");
        const QString u3 = unitOf(jo3, "C2", "missing");
        check(u3.contains(QString::fromUtf8("绝对")),
              QString("站点不足时单位应标明走了绝对兜底(实际 \"%1\")").arg(u3));
    }
}

// ============================================================
// 被守卫跳过的规则必须报出来(2026-10-14)
// ============================================================
//
// 起因是一个量不出来的问题:`insufficient` 在 complex 三块板上到底跑没跑。
// 量出来的结论是**一次都没跑** ——
//
//     板型/站点          金层连通域   insufficient
//     demo  U1  IC             13        跑
//     mcu   U1  QFP64           3        没跑
//     mcu   Y1/Y2 XTAL        3 / 3      没跑
//     power U1  TO220           2        没跑
//     signal U1/U2 SOIC8       0 / 0      没跑
//
// 连通域数是把开运算后的金层掩膜重数一遍得到的,并与算子自己的输出
// 交叉验证过(7 个站点 7 个一致);"跑没跑"则是直接读算子吐的 metric。
// 根因是 `areas.size() >= 4` 这道守卫,不是阈值。
//
// 而它一直没被发现,是因为**报告在撒谎**:覆盖度 withRules 是**按配方里
// 写了什么**算的,不是按真的跑了什么。于是配方点名要查 insufficient、
// 实际一次没跑,覆盖度报告还写"已覆盖",界面上一点异常都没有。
//
// 所以这里不只钉 insufficient 的具体数字,更钉一条**不变式**:
//     配方为某站点要的每一条规则,必须出现在 metrics 或 skipped 里。
// 这条不变式管的是将来 —— 以后再加一道守卫,忘了报就会红。
static void test_skipped_rules_are_reported() {
    beginCase("被守卫跳过的规则必须报出来,不能默默不跑");

    struct Img { const char* recipe; const char* file; bool complex; };
    const Img imgs[] = {
        {"demo.json",   "baseline.png",        false},
        {"demo.json",   "insufficient.png",    false},
        {"mcu.json",    "mcu_baseline.png",    true },
        {"power.json",  "power_baseline.png",  true },
        {"signal.json", "signal_baseline.png", true },
        {"power.json",  "power_missing.png",   true },
    };

    // 这几条是有归属元件的规则。short 是全局缺陷、没有 ROI,
    // 不在这条不变式里(配方也从来不给任何站点写 short)。
    const QStringList owned = {"missing", "wrong", "polarity",
                               "bridge", "insufficient", "shifted"};

    for (const auto& im : imgs) {
        const QString dir = im.complex
            ? QString::fromUtf8(kComplexDir)
            : QString("images/");
        RunResult r = runBoard(QString::fromUtf8(im.recipe),
                               dir + QString::fromUtf8(im.file));
        const QString who = QString::fromUtf8(im.file);
        check(!r.judgeOut.contains("error"),
              QString("%1 判定无错误(%2)").arg(who, r.judgeOut.value("error").toString("无")));

        // 跑过的(在 metrics 里)和跳过的(在 skipped 里)各收一份
        QSet<QString> ran, skipped;
        for (const auto& m : r.judgeOut.value("metrics").toArray()) {
            const QJsonObject mo = m.toObject();
            ran << mo.value("component").toString() + "/" + mo.value("rule").toString();
        }
        for (const auto& s : r.judgeOut.value("skipped").toArray()) {
            const QJsonObject so = s.toObject();
            check(!so.value("reason").toString().isEmpty(),
                  QString("%1 %2 的跳过原因不能是空串")
                      .arg(who, so.value("component").toString()));
            // 原因里必须带**实测数**,不能只是一句"没查"。
            // 只查"非空"的话,把原因换成任何一句别的照样绿 —— 实测过:
            // 故意把 insufficient 的原因改成一句无关的话,测试照样全过。
            // 而"为什么没跑"正是调参的人唯一用得上的信息。
            //
            // ⚠ 2026-10-21:insufficient 的跳过原因换了口径 ——
            //   以前是"金层连通域只有 N 个(< 4)",量的是 ROI 里的连通域;
            //   现在量的是**登记过的端子**,所以原因是这两种:
            //     "这个元件没有登记端子位置,量不了焊盘面积"
            //     "这个元件只登记了 N 个端子,没有同侪可比"
            //   两种都得写清"缺的是什么",不然还是一句"没查"。
            if (so.value("rule").toString() == "insufficient") {
                const QString reason = so.value("reason").toString();
                const bool hasCount =
                    QRegularExpression(QString::fromUtf8("(\\d+) 个"))
                        .match(reason).hasMatch();
                const bool saysNoPads =
                    reason.contains(QString::fromUtf8("没有登记端子"));
                check(hasCount || saysNoPads,
                      QString("%1 %2/insufficient 的跳过原因要写明缺了什么"
                              "(实测数,或\"没有登记端子\")(实际 \"%1\")")
                          .arg(who, so.value("component").toString(), reason));
            }
            skipped << so.value("component").toString() + "/"
                     + so.value("rule").toString();
        }
        // skipped_count 必须和 skipped 数组对得上(报告本身也要自洽)
        check(r.judgeOut.value("skipped_count").toInt()
                  == r.judgeOut.value("skipped").toArray().size(),
              QString("%1 skipped_count 与 skipped 数组一致(实际 %2 vs %3)")
                  .arg(who)
                  .arg(r.judgeOut.value("skipped_count").toInt())
                  .arg(r.judgeOut.value("skipped").toArray().size()));

        // ★ 不变式:配方要的每条规则,要么跑过,要么被报为跳过
        QStringList missing_;
        for (const auto& c : r.inspectOut.value("components").toArray()) {
            const QJsonObject co = c.toObject();
            const QString label = co.value("label").toString();
            for (const auto& ru : co.value("checks").toArray()) {
                const QString rule = ru.toString();
                if (!owned.contains(rule)) continue;
                const QString key = label + "/" + rule;
                if (!ran.contains(key) && !skipped.contains(key))
                    missing_ << key;
            }
        }
        check(missing_.isEmpty(),
              QString("%1 配方要的规则全都跑过或被报为跳过"
                      "(悄悄没跑的有:%1)")
                  .arg(who, missing_.isEmpty() ? QString("(无)")
                                               : missing_.join(" ")));

        // 有跳过就必须有告警 —— 否则"没查"这件事还是看不见
        if (!skipped.isEmpty()) {
            check(r.judgeOut.value("warning").toString().contains(QString::fromUtf8("没查")),
                  QString("%1 有规则被跳过时,告警里必须写明\"没查\"(实际 \"%1\")")
                      .arg(who, r.judgeOut.value("warning").toString()));
        }
    }

    // ---- 具体数字:哪块板、哪个站点的哪条规则没跑(量出来的,不是推的) ----
    //
    // ⚠ 2026-10-21:三块 complex 板的这三行**从非空变成了空** ——
    //   原来 mcu 是 "U1/insufficient Y1/insufficient Y2/insufficient",
    //   power 是 "U1/insufficient",signal 是 "U1/insufficient U2/insufficient"。
    //   原因是 insufficient 判据重做了(见 common/pcb_core/pcb_core.cpp):
    //   它现在逐个**登记过的端子**量金属面积,不再需要"凑够 4 个连通域"。
    //   于是 TO-220(3 脚)/ SOIC-8(8 脚)/ QFP-64(64 脚)/ 晶振(2 脚)
    //   全都真的跑起来了 —— **"没查"变成了"查过了"**。
    //
    //   这张表现在**全空**:凡是配了规则的,都真的跑了。
    //   以后哪条规则又跳过,这张表会当场红 —— 那是它的用处。
    {
        const QString dir = kComplexDir;
        struct Expect { const char* recipe; const char* file; const char* keys; };
        const Expect exp[] = {
            {"demo.json",   "baseline.png",        ""},
            {"mcu.json",    "mcu_baseline.png",    ""},
            {"power.json",  "power_baseline.png",  ""},
            {"signal.json", "signal_baseline.png", ""},
        };
        for (const auto& e : exp) {
            RunResult r = runBoard(QString::fromUtf8(e.recipe),
                                   dir + QString::fromUtf8(e.file));
            QStringList got;
            for (const auto& s : r.judgeOut.value("skipped").toArray()) {
                const QJsonObject so = s.toObject();
                got << so.value("component").toString() + "/"
                     + so.value("rule").toString();
            }
            QStringList want;
            const QString ws = QString::fromUtf8(e.keys);
            if (!ws.isEmpty())
                for (const auto& k : ws.split(' ', Qt::SkipEmptyParts))
                    want << k;
            want.sort();
            got.sort();
            check(got == want,
                  QString("%1 被跳过的规则 = {%2}(实际 {%3})")
                      .arg(QString::fromUtf8(e.file),
                           want.isEmpty() ? QString("(无)") : want.join(" "),
                           got.isEmpty() ? QString("(无)") : got.join(" ")));
        }
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
    test_short_names_the_component();
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
    test_gold_mask_shared_between_operators();
    test_auto_contour_scale_stable();
    test_board_switch_good_boards_clean();
    test_board_switch_defect_boards();
    test_cap_missing_uses_body_pct();
    test_skipped_rules_are_reported();
    test_manifest_is_the_single_source();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
