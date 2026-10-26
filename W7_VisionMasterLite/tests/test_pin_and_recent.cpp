// ============================================================
// W7 pin 类型检查 + 最近文件列表 — 无头回归测试
//
// 这两块都不涉及界面,全是"错了不崩、只是行为不对"的逻辑:
//   · pin 检查放过一条不该连的线 → 用户搭完流水线 Run 才炸,
//     或者更糟:不炸,只是拿不到该拿的字段,静默算错
//   · 最近列表存了相对路径 → 下次从别的目录启动,工程"不见了"
// 所以单测的重点是"哪些该拦、哪些不该拦",而不是覆盖率。
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <windows.h>

#include "operators/pin_types.h"
#include "recent_files.h"

using namespace w7;

// ============================================================
// 极简断言(与其余测试同一套风格)
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

// 便捷包装:checkOk = 允许连, checkNo = 应被拒且提示里有指定关键词
static QString chk(PinType out, PinType in, bool toIsSource, bool self) {
    return checkPinCompatibility(out, in, toIsSource, "上游", "下游", self);
}
static void checkOk(PinType out, PinType in, bool toIsSource, const QString& what) {
    check(chk(out, in, toIsSource, false).isEmpty(),
          what + (chk(out, in, toIsSource, false).isEmpty()
                      ? QString()
                      : QString("  (被拒:'%1')").arg(chk(out, in, toIsSource, false))));
}

// ------------------------------------------------------------
// 用例 1:正常的流水线必须一路放行
// 拦错了比不拦更烦人 —— 用户明明连对了却被弹窗挡住
// ------------------------------------------------------------
static void test_pin_allows_normal_flow() {
    beginCase("正常流水线必须放行");
    const auto I = PinType::Image;
    const auto D = PinType::Data;
    const auto T = PinType::Text;

    checkOk(I, I, false, "图像源 → 颜色分割");
    checkOk(I, I, false, "颜色分割 → Blob 计数(吐 mask,也是图)");
    checkOk(D, D, false, "Blob 计数 → 数量判定");
    checkOk(I, I, false, "图像源 → 模板匹配");
    checkOk(I, I, false, "图像源 → QR 解码");
    checkOk(I, I, false, "图像源 → PCB 元件定位");
    checkOk(D, D, false, "PCB 元件定位 → PCB 缺陷判定");
    // 注意:Text 目前是终点类型 —— QR 解码 / 数量判定 / 缺陷判定
    // 都吐文本,但没有任何算子以文本为输入。所以 D→T 是被拒的,
    // 这符合设计(它们是流水线最后的判定环节)。
    // 真要加一个吃文本的算子,得同时想清楚它的输入类型。
}

// ------------------------------------------------------------
// 用例 2:该拦的必须拦
// 每条都对应一个新手真会犯的错
// ------------------------------------------------------------
static void test_pin_blocks_mistakes() {
    beginCase("明显错的连法必须拦下");

    // 自环
    const QString selfLoop = checkPinCompatibility(
        PinType::Image, PinType::Image, false, "颜色分割", "颜色分割", true);
    check(selfLoop.contains("自己"),
          QString("自环被拦 (实际 '%1')").arg(selfLoop));

    // 往图像源上连 —— 图像源没有输入口
    const QString intoSource = checkPinCompatibility(
        PinType::Image, PinType::Image, /*toIsSource=*/true,
        "颜色分割", "文件夹批量图像", false);
    check(intoSource.contains("图像源"),
          QString("往图像源连被拦 (实际 '%1')").arg(intoSource));

    // 类型不符
    const QString mismatch = checkPinCompatibility(
        PinType::Text, PinType::Image, false, "数量判定", "颜色分割", false);
    check(mismatch.contains("类型不匹配"),
          QString("文本→图像 被拦 (实际 '%1')").arg(mismatch));
    // 提示里要说清两边是什么类型,否则用户不知道怎么改
    check(mismatch.contains("文本") && mismatch.contains("图像"),
          "类型不符的提示里写清了双方类型");

    // 图像源 → 数量判定
    // 这条最值:count_check 只读 inputs["count"],接图像源时
    // count 拿不到 → actual=0 → 判 FAIL,而日志看不出任何异常。
    // 类型检查把这条静默算错的连法挡在搭流水线的时候。
    const QString srcToCheck = checkPinCompatibility(
        PinType::Image, PinType::Data, false, "文件夹批量图像", "数量判定", false);
    check(!srcToCheck.isEmpty(),
          QString("图像源 → 数量判定 被拦(否则会静默判 FAIL,"
                  "实际 '%1')").arg(srcToCheck));
}

// ------------------------------------------------------------
// 用例 3:Any 不能误伤
// 新算子没写类型声明时是 Any —— 这时必须放行。
// 默认给具体类型的话,每个新算子忘了声明就会被莫名拦住。
// ------------------------------------------------------------
static void test_pin_any_never_blocks() {
    beginCase("Any 类型不拦任何线");
    const auto A = PinType::Any;
    const auto I = PinType::Image;
    const auto D = PinType::Data;
    const auto T = PinType::Text;

    checkOk(A, I, false, "Any → 图像");
    checkOk(I, A, false, "图像 → Any");
    checkOk(A, D, false, "Any → 数据");
    checkOk(T, A, false, "文本 → Any");
    checkOk(A, A, false, "Any → Any");
    // Even Any→Any into a source is still blocked: "no input port"
    // is a structural fact, not a type question
    const QString anyIntoSource = checkPinCompatibility(
        A, A, true, "新算子", "文件夹批量图像", false);
    check(anyIntoSource.contains("图像源"),
          "Any→Any 接图像源仍被拦(没有输入口是结构事实,不是类型问题)");
}

// ------------------------------------------------------------
// 用例 4:类型名不能为空/不能重复
// 名字会画在节点上,重复或空白会让画布上出现一模一样的两行,
// 用户没法分辨
// ------------------------------------------------------------
static void test_pin_names() {
    beginCase("类型名");
    const QStringList names = {
        pinTypeName(PinType::Any), pinTypeName(PinType::Image),
        pinTypeName(PinType::Data),  pinTypeName(PinType::Text)};
    for (const QString& n : names) {
        check(!n.isEmpty(), QString("类型名非空: %1").arg(n));
    }
    QSet<QString> uniq(names.begin(), names.end());
    check(uniq.size() == names.size(),
          QString("4 个类型名互不相同 (实际去重后 %1 个)").arg(uniq.size()));
}

// ============================================================
// === 多前驱冲突检查 ===
//
// 这组测的是一个**已实测复现**的静默失效(2026-10-15 一次性探针,
// 照抄 mainwindow.cpp 的引擎合并逻辑跑出来的):
//
//   顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue / 5000
//   顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red  / 10000
//
// 两个上游状态点都是绿的、都不报错,只有下游那个数悄悄变了。
// 注意连"先连线的赢"都不成立 —— A 在前却被 B 覆盖了,
// 真正决定胜负的是 connections() 的遍历顺序。
// ============================================================

// 便捷封装:第一条线永远放行
static QString chkMulti(const QString& toDisp,
                        const QString& existingFrom,
                        const QString& newFrom) {
    return checkMultiplePredecessors(toDisp, existingFrom, newFrom);
}

static void test_multi_predecessor_allows_first_edge() {
    beginCase("多前驱:第一条线必须放行");
    // 没有前驱时,无论是谁来连都要放行
    check(chkMulti("Blob 分析", "", "颜色分割").isEmpty(),
          "空前驱 + 任意来源 = 放行(第一条线永远合法)");
    check(chkMulti("Blob 分析", QString(), QString()).isEmpty(),
          "两边都空 = 放行");
}

static void test_multi_predecessor_blocks_second_edge() {
    beginCase("多前驱:第二条线必须拦住");

    const QString why = chkMulti("Blob 分析", "颜色分割 A", "颜色分割 B");
    check(!why.isEmpty(), "已有前驱时新增第二条线被拒绝");

    // 提示必须**说清楚是哪两条**,否则用户不知道怎么改
    check(why.contains("颜色分割 A"), "提示点名了已有的那条线");
    check(why.contains("颜色分割 B"), "提示点名了新增的这条线");
    check(why.contains("Blob 分析"),  "提示点名了下游算子");

    // 必须解释**为什么**,不只是说"不行"
    check(why.contains("后写覆盖"), "提示说明了根因是「后写覆盖」");
    check(why.contains("后写"),     "提示包含「后写」这个关键词(解释覆盖方向)");

    // 必须告诉用户怎么办 —— 只报错不给出路,等于把问题推给用户猜
    check(why.contains("解决办法"), "提示给出了解决办法");
    check(why.contains("支路") || why.contains("分支"),
          "解决办法里提到了可行的布线方式");
}

static void test_multi_predecessor_same_op_name_still_blocked() {
    beginCase("多前驱:同名算子接两条也要拦");

    // 这是最真实的场合:抓红 + 抓蓝,两个都叫"颜色分割"。
    // 显示名相同不代表是同一个节点,冲突照样成立。
    const QString why = chkMulti("Blob 分析", "颜色分割", "颜色分割");
    check(!why.isEmpty(), "两个同名算子接到同一下游,仍然被拒绝");

    // 反向不变式:不能因为"看起来一样"就放行,
    // 也不能因为提示里两个名字相同就误判成"没有冲突"
    check(why.contains("颜色分割"), "提示仍点名了算子");
}

static void test_multi_predecessor_independent_nodes_never_conflict() {
    beginCase("多前驱:不同下游互不影响");

    // 每个下游各自记自己的前驱 —— 不能因为 A→B 连过,
    // 就把 C→D 也拦掉。这是最容易写错的地方:
    // 如果实现时不小心用了"全局已连线数"而不是"这个下游的前驱数",
    // 这条就会红。
    check(chkMulti("Blob 分析", "", "颜色分割").isEmpty(),
          "Blob 分析 还没接,第一条放行");
    check(chkMulti("数量判定", "", "Blob 计数").isEmpty(),
          "数量判定 还没接,第一条也放行(不受上一个节点影响)");
    check(!chkMulti("Blob 分析", "颜色分割", "模板匹配").isEmpty(),
          "只有 Blob 分析 自己被拦,数量判定 不受影响");
}

// ============================================================
// === 最近文件列表 ===
// ============================================================
static void test_recent_basics() {
    beginCase("最近列表:去重 / 提前 / 截断");
    const QString key = "test/recent_basic";
    RecentFiles rf(key, 5);
    rf.clear();

    check(rf.paths().isEmpty(), "新列表是空的");

    rf.add("D:/a.sol.json");
    rf.add("D:/b.sol.json");
    rf.add("D:/c.sol.json");
    check(rf.paths().size() == 3, "加了 3 条");
    // 最新的排最前 —— 最后加的是 c
    check(rf.paths().first() == "D:/c.sol.json",
          QString("最新的排最前 (实际 %1)").arg(rf.paths().first()));

    // 重开同一个文件:提到最前,而不是多出一条
    rf.add("D:/c.sol.json");
    check(rf.paths().size() == 3,
          QString("重开同一文件不产生第二条 (实际 %1)")
              .arg(rf.paths().size()));
    check(rf.paths().first() == "D:/c.sol.json", "重开的提到了最前");

    // 超过上限丢最旧的
    for (int i = 0; i < 8; ++i) rf.add(QString("D:/f%1.sol.json").arg(i));
    check(rf.paths().size() == 5,
          QString("上限 5 条,实际 %1 条").arg(rf.paths().size()));

    // 空路径不该进列表
    rf.add("");
    check(!rf.paths().contains(""), "空路径没进列表");

    rf.clear();
    check(rf.paths().isEmpty(), "clear() 之后是空的");
}

// ------------------------------------------------------------
// 用例 5:路径必须归一化成绝对
//
// 这是"最近列表"最容易犯且最难发现的错:
// 第一次从工程根启动,存下相对路径;下次从别处启动,
// 同一个字符串指向别的目录,文件"不见了"。
// ------------------------------------------------------------
static void test_recent_normalize() {
    beginCase("路径归一化");
    check(RecentFiles::normalize("").isEmpty(), "空路径归一化后仍为空");

    const QString cwd = QDir::currentPath();
    const QString rel = RecentFiles::normalize("demo.sol.json");
    check(QDir::isAbsolutePath(rel),
          QString("相对路径被转成绝对: %1").arg(rel));
    check(rel.endsWith("demo.sol.json"), "文件名没变");
    check(rel.startsWith(cwd), "绝对路径基于当前工作目录");

    // 不同写法的同一文件要归一到同一条,否则菜单里会出现两项
    const QString a = RecentFiles::normalize("demo.sol.json");
    const QString b = RecentFiles::normalize("./demo.sol.json");
    check(a == b, "'x' 和 './x' 归一到同一条");
    check(RecentFiles::normalize("sub/../demo.sol.json") == a,
          "'sub/../x' 折叠后与 'x' 相同");

    // 已经是绝对路径的不该被再套一层
    const QString abs = "D:/x/y.sol.json";
    check(RecentFiles::normalize(abs) == abs, "绝对路径原样保留");
}

// ------------------------------------------------------------
// 用例 6:pruneMissing 剔掉被删/被挪走的条目
// 用户把工程移走之后,列表里还挂着 → 点下去只得到"文件不存在",
// 试两次之后整个菜单都不敢用了。
// ------------------------------------------------------------
static void test_recent_prune() {
    beginCase("剔除已不存在的文件");
    QTemporaryDir dir;
    check(dir.isValid(), "临时目录可用");

    const QString f1 = dir.filePath("keep.sol.json");
    const QString f2 = dir.filePath("gone.sol.json");
    QFile a(f1); a.open(QIODevice::WriteOnly); a.write("{}"); a.close();
    QFile b(f2); b.open(QIODevice::WriteOnly); b.write("{}"); b.close();

    RecentFiles rf("test/recent_prune", 5);
    rf.clear();
    rf.add(f1);
    rf.add(f2);
    check(rf.paths().size() == 2, "先放 2 条");

    QFile::remove(f2);   // 模拟用户把文件删了

    const int dropped = rf.pruneMissing();
    check(dropped == 1, QString("剔掉 1 条 (实际 %1)").arg(dropped));
    check(rf.paths().size() == 1, "剩 1 条");
    check(rf.paths().first() == QDir::cleanPath(f1), "留下的是还在的那个");

    // 幂等:再剔一次不该有变化
    check(rf.pruneMissing() == 0, "再剔一次是 0 条(幂等)");

    rf.clear();
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);
    // 用独立的 ini 文件,别把用户真实的最近列表搅了
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QDir::temp().filePath("w7_test_settings"));

    qInfo().noquote() << "W7 pin 类型检查 + 最近文件列表 回归测试";

    test_pin_allows_normal_flow();
    test_pin_blocks_mistakes();
    test_pin_any_never_blocks();
    test_pin_names();

    test_multi_predecessor_allows_first_edge();
    test_multi_predecessor_blocks_second_edge();
    test_multi_predecessor_same_op_name_still_blocked();
    test_multi_predecessor_independent_nodes_never_conflict();

    test_recent_basics();
    test_recent_normalize();
    test_recent_prune();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
