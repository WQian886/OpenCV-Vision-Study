// ============================================================
// 算子库分组 回归测试(无头,纯 QtCore)
//
// ------------------------------------------------------------
// 为什么这层值得测
// ------------------------------------------------------------
// 它以前是硬编码的一串 if/else,把算子的 category 映射到树节点:
//
//     if      (category == "acquisition") parent = acquisition;
//     else if (category == "positioning") parent = positioning;
//     ... 一共 5 个分支
//     if (parent) { 建树项 }
//
// 而注册表里写的是 category = "dl"。**6 个分类只认 5 个**,
// parent 停在 nullptr,`if (parent)` 不成立 ——
// dl_infer 凭空消失,左边算子库里根本看不到,**连日志都不打一句**。
//
// 这个形态的特点是:新增一个分类不会报错、不会崩,
// 只会让那一类算子**无声无息地不见了**。用户看到的是
// "怎么没有 DL 推理",不会想到是建树的 if 少了一个分支。
//
// ------------------------------------------------------------
// 关键断言:不是"dl_infer 出现了",而是"**没有算子会消失**" ——
// 对任何输入(含没见过的分类)都成立。后者才钉得住这个 bug 的形态。
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>
#include <QSet>

#include "operator_library_groups.h"

#include <windows.h>   // SetConsoleOutputCP

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
static void checkEq(int actual, int expected, const QString& what) {
    check(actual == expected, QString("%1 (期望 %2,实际 %3)").arg(what).arg(expected).arg(actual));
}

// ------------------------------------------------------------
// 照抄 MainWindow::registerOperators 里的真实注册表。
// 这里必须**手抄一份而不是 include mainwindow.h** ——
// mainwindow.h 要 QtWidgets,会把整个 GUI 拖进来;
// 而且抄下来才看得出"表里有几个分类、分支里有几个"这种对不上的地方。
// 以后 registerOperators() 加了算子,这里要跟着补 ——
// 下面有一条断言专门盯这个。
// ------------------------------------------------------------
static QList<OperatorInfoLite> realRegistry() {
    return {
        {"image_source_dir",  "文件夹批量图像",   "acquisition"},
        {"image_source_camera","工业相机实时流",  "acquisition"},
        {"image_source_video","视频文件图像源",   "acquisition"},
        {"color_segment",     "颜色分割",         "detection"},
        {"blob_count",        "Blob 计数",        "detection"},
        {"count_check",       "数量判定",         "detection"},
        {"template_match",    "模板匹配",         "positioning"},
        {"qr_decode",         "QR 解码",          "identification"},
        {"pcb_inspect",       "PCB 元件定位",     "detection"},
        {"pcb_defect_judge",  "PCB 缺陷判定",     "detection"},
        {"dl_infer",          "DL 推理 (ONNX)",   "dl"},
        {"line_fit",          "直线测量",         "measurement"},
        {"circle_fit",        "圆形测量",         "measurement"},
        {"caliper_dist",      "卡尺距离",         "measurement"},
        {"point_point_dist",  "两点距离",         "measurement"},
        {"point_line_dist",   "点到线距离",       "measurement"},
        {"angle",             "两线夹角",         "measurement"},
    };
}

// 修复前的分支表:只有 5 个,没有 dl
static QList<QPair<QString, QString>> oldBranches() {
    return {
        {"acquisition",    "采集 Acquisition"},
        {"positioning",    "定位 Positioning"},
        {"measurement",    "测量 Measurement"},
        {"identification", "识别 Identification"},
        {"detection",      "检测 Detection"},
    };
}

// 修复后的分组表:补上 dl
static QList<QPair<QString, QString>> newBranches() {
    auto b = oldBranches();
    b.append(qMakePair(QStringLiteral("dl"), QStringLiteral("深度学习 Deep Learning")));
    return b;
}

static QSet<QString> placedOperators(const QList<LibraryGroup>& groups) {
    QSet<QString> all;
    for (const auto& g : groups) all.unite(QSet<QString>(g.operators.begin(), g.operators.end()));
    return all;
}

// ------------------------------------------------------------
// 用例 1:注册表里每个算子都能找到归属(这条就是 bug 的本体)
// ------------------------------------------------------------
static void test_no_operator_disappears() {
    beginCase("1. 没有算子会从算子库里消失");
    const auto ops = realRegistry();

    QStringList unknown;
    const auto groups = buildOperatorLibraryGroups(ops, newBranches(), &unknown);

    const QSet<QString> placed = placedOperators(groups);
    checkEq(placed.size(), ops.size(),
            QString("落地的算子数 = 注册数(%1)").arg(ops.size()));
    for (const auto& op : ops) {
        check(placed.contains(op.name),
              QString("%1(%2)出现在算子库里").arg(op.display, op.name));
    }

    checkEq(unknown.size(), 0,
            QString("没有用到未知分类(实际未知:%1)").arg(unknown.join(',')));
}

// ------------------------------------------------------------
// 用例 2:复刻旧实现,确认前提是真的
//
// 这里**不能**拿新函数去模拟旧行为 —— 新函数本来就不丢算子,
// 那样写出来的对照组是假的(第一版就栽在这:断言失败才发现想错了)。
// 所以把旧代码的 if/else + `if (parent)` 原样搬过来。
//
// ------------------------------------------------------------
static QSet<QString> placeLikeOldImplementation(const QList<OperatorInfoLite>& ops) {
    const auto branches = oldBranches();
    QSet<QString> placed;
    for (const auto& op : ops) {
        bool parentFound = false;                 // ← 旧代码里的 QTreeWidgetItem* parent
        for (const auto& b : branches)
            if (b.first == op.category) { parentFound = true; break; }
        if (!parentFound) continue;               // ← 旧代码的 `if (parent)`
        placed.insert(op.name);
    }
    return placed;
}

static void test_old_branches_dropped_dl() {
    beginCase("2. 复现:只认 5 个分类时,dl_infer 被丢掉");
    const auto ops = realRegistry();

    // --- 旧实现的真实行为 ---
    const QSet<QString> oldPlaced = placeLikeOldImplementation(ops);
    check(!oldPlaced.contains("dl_infer"),
          "对照:旧实现(5 个分支)下 dl_infer 确实不见了 —— 前提属实");
    checkEq(ops.size() - oldPlaced.size(), 1,
            "对照:旧实现恰好丢了 1 个算子");

    // --- 新实现,即使只喂旧的 5 个分支 ---
    QStringList unknown;
    const auto groups = buildOperatorLibraryGroups(ops, oldBranches(), &unknown);
    checkEq(placedOperators(groups).size(), ops.size(),
            "喂旧的 5 个分支表,算子一个都不会少(兜底建组救回来的)");
    check(unknown.contains("dl"),
          "dl 被报告成未知分类 —— 修复前这条信息根本不会产生");
    check(unknown.size() == 1, "未知分类恰好报一个");
}

// ------------------------------------------------------------
// 用例 3:核心不变式 —— 传什么分类都不会丢算子
// ------------------------------------------------------------
static void test_unknown_category_still_lands() {
    beginCase("3. 核心不变式:没见过的分类也不会丢算子");

    QList<OperatorInfoLite> ops = realRegistry();
    ops.append({"brand_new_op", "全新算子", "brand_new_category"});

    QStringList unknown;
    const auto groups = buildOperatorLibraryGroups(ops, newBranches(), &unknown);
    const QSet<QString> placed = placedOperators(groups);

    check(placed.contains("brand_new_op"),
          "分组表里没有的分类,算子照样出现在库里(不会被静默丢弃)");
    check(unknown.contains("brand_new_category"),
          "未知分类被回报出来 —— 这条信息以前是不存在的");
    checkEq(unknown.size(), 1, QString("未知分类恰好被报告一次(实际:%1)").arg(unknown.join(',')));

    // 现建的分组要真的有这个算子,而不是只有一个空壳分组
    bool found = false;
    for (const auto& g : groups)
        if (g.key == "brand_new_category") {
            found = g.operators.contains("brand_new_op");
            break;
        }
    check(found, "现建的分组里确实挂着那个算子");
}

// ------------------------------------------------------------
// 用例 4:分组结构本身
// ------------------------------------------------------------
static void test_group_structure() {
    beginCase("4. 分组结构:顺序、不重复、空组保留");

    // 只用一个算子,其余 5 个组应该是空的但仍然存在
    QStringList unknown;
    const auto groups = buildOperatorLibraryGroups({{"a", "A", "detection"}},
                                                   newBranches(), &unknown);
    checkEq(groups.size(), 6, "6 个已知分组全部保留(空组也留着,对齐 VM 固定布局)");

    QStringList keys;
    for (const auto& g : groups) keys.append(g.key);
    check(keys == QStringList({"acquisition", "positioning", "measurement",
                               "identification", "detection", "dl"}),
          "分组顺序与配置表一致");
    checkEq(QSet<QString>(keys.begin(), keys.end()).size(), keys.size(),
            "分组没有重复项");

    // dl_infer 单独验一次:它必须落在 dl 组,不是别的组
    const auto ops = realRegistry();
    const auto g2 = buildOperatorLibraryGroups(ops, newBranches(), &unknown);
    QString dlMembers, detectionMembers;
    for (const auto& g : g2) {
        if (g.key == "dl")          dlMembers = g.operators.join(',');
        if (g.key == "detection")   detectionMembers = g.operators.join(',');
    }
    check(dlMembers == "dl_infer", "dl 组里只有 dl_infer(实际:" + dlMembers + ")");
    check(detectionMembers.contains("color_segment") && !detectionMembers.contains("dl_infer"),
          "dl_infer 没有混进检测组");
}

// ------------------------------------------------------------
// 用例 5:退化输入
// ------------------------------------------------------------
static void test_degenerate() {
    beginCase("5. 退化输入");
    QStringList unknown("残留值");

    const auto empty = buildOperatorLibraryGroups({}, newBranches(), &unknown);
    checkEq(empty.size(), 6, "空注册表 → 只剩 6 个空分组");
    checkEq(unknown.size(), 0, "空注册表不该报未知分类");

    const auto g = buildOperatorLibraryGroups({{"", "", "detection"}}, newBranches(), &unknown);
    checkEq(placedOperators(g).size(), 0,
            "空算子名不计入(否则树上会出现一个点不出来的空条目)");

    // nullptr 出参也要能用 —— MainWindow 现在传的正是 nullptr
    const auto g2 = buildOperatorLibraryGroups({{"x", "X", "brandnew"}},
                                               newBranches(), nullptr);
    checkEq(placedOperators(g2).size(), 1, "unknownCategories 传 nullptr 也不崩");
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);

    qInfo().noquote() << "W7 算子库分组 回归测试";

    test_no_operator_disappears();
    test_old_branches_dropped_dl();
    test_unknown_category_still_lands();
    test_group_structure();
    test_degenerate();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}