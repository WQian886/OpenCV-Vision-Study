// ============================================================
// W7 纯层(零 Widgets / 零 OpenCV)的回归测试
// ============================================================
//
// 测的是哪件事:
//
//   W7 的「层」有一条边界:**纯逻辑不许拖 widget**。
//   边界以前**只写在注释里** ——
//   run_limits.h 自己写着「纯算术」,run_policy.h 写着「纯逻辑」,
//   pipeline_graph.h 写着「零 OpenCV 零 QtWidgets」——
//   然后 run_policy.h 自己就拖了个 <QPlainTextEdit>,
//   而**没有任何东西拦着**。
//
// 为什么这一套是**编译期**的约束
// ============================================================
//
// 这个目标**只链 Qt6::Core**,而且 include 了全部纯头文件:
//
//     run_limits.h / run_policy.h / frame_verdict.h / run_engine.h
//     pipeline_graph.h / result_render_plan.h / step_frame_cache.h
//     frame_record.h / headless_runner.h
//
// 后果:哪天有人往任何一个纯头文件里塞一个 QWidget / QPlainTextEdit
// / opencv2,**这个目标编不过** —— CI 在构建那一步就红。
//
// ⚠ 这比源码 lint 强一个量级:
//
//     lint 查的是**文本** —— 而注释也是文本,
//     而且能被等价改写绕过(本项目栽过:`contains("w7::planResultRender")`
//     被一句注释喂饱,见 notes/atomic/at-the-guard-can-be-fed-by-a-comment.md)。
//     编译器查的是**依赖** —— 注释骗不了它,等价改写也骗不了它。
//
// 所以这一套是本项目**第一条不用 lint 守的边界**。
//
// ⚠ 代价:破坏它的时候,失败发生在**构建**那一步,不是测试那一步。
//   所以下面还有一条**读得懂**的守卫(用例 4)把这件事用中文讲出来 ——
//   免得下一个人的第一反应是"环境坏了"。
//
// ============================================================

#include <QJsonObject>
#include <QDebug>
#include <QString>
#include <QStringList>
#include <QVector>

// ⚠ 下面这些 include **就是**这个套件的守卫。
//   任何一处拖了 widget/OpenCV,这个文件就编不过。
#include "run_limits.h"          // 帧数封顶 + 帧协议
#include "run_policy.h"           // 重绘限频 / 取消 / 重入
#include "frame_verdict.h"        // 判定口径
#include "run_engine.h"           // 拓扑序 + 逐节点执行
#include "pipeline_graph.h"       // 读盘的纯解析层
#include "result_render_plan.h"   // 结果面板「显示哪张图」
#include "step_frame_cache.h"     // 多帧缓存
#include "frame_record.h"         // 帧接线
#include "headless_runner.h"      // 无头跑一条流水线

#include "source_scan.h"          // 剥注释 / 读源码(共享的测试工具)

using namespace w7;
using namespace w7test;

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

// ============================================================
// 用例 1:帧协议 —— 「源怎么报帧数」只有一份
// ============================================================
//
// ⚠ 2026-10-25:这条规则以前在 **两个文件里各写一遍**:
//     mainwindow.cpp      帧循环首帧之后
//     headless_runner.cpp 同一件事
//   两边碰巧一致,但没有任何东西保证它们一致 ——
//   哪天源算子改了 frame_count 的语义,两边会各按各的理解算,
//   而**两边都是绿的**。
//   这是本项目栽过**九遍**的「抄副本」的**第十遍**。
// ============================================================
static void test_resolve_frame_count() {
    beginCase("1. 帧协议:第一个报 frame_count 的说了算");

    // ---- 什么都没有 -> 1 帧 ----
    {
        const QVector<QJsonObject> none;
        int reported = -1;
        bool trunc = true;
        check(resolveFrameCount(none, &reported, &trunc) == 1,
              "一个输出都没有 -> 1 帧");
        check(!trunc, "没截断");
    }

    // ---- 有输出但都没报 frame_count -> 还是 1 帧 ----
    {
        QVector<QJsonObject> outs;
        QJsonObject a; a["image_path"] = "images/a.png";
        QJsonObject b; b["count"] = 3;
        outs << a << b;
        check(resolveFrameCount(outs) == 1, "都没报 frame_count -> 1 帧");
    }

    // ---- 第一个说了算 ----
    {
        QVector<QJsonObject> outs;
        QJsonObject a; a["frame_count"] = 7;
        QJsonObject b; b["frame_count"] = 99;   // ← 更大的,但**不是第一个**
        outs << a << b;
        int reported = -1;
        check(resolveFrameCount(outs, &reported) == 7,
              QString("取**第一个**(实际 %1,不是 99)").arg(reported));
        check(reported == 7, "reported 报出来的是 7");
    }

    // ---- 0 和负数 -> 当 1 帧(循环至少进一次) ----
    {
        QVector<QJsonObject> outs;
        QJsonObject z; z["frame_count"] = 0;
        outs << z;
        check(resolveFrameCount(outs) == 1, "报 0 -> 当 1 帧(否则循环一次都不进)");

        QVector<QJsonObject> negs;
        QJsonObject n; n["frame_count"] = -5;
        negs << n;
        check(resolveFrameCount(negs) == 1, "报负数 -> 当 1 帧");
    }

    // ---- 值不是数字 -> 不猜,当 1 帧 ----
    {
        QVector<QJsonObject> outs;
        QJsonObject s; s["frame_count"] = "abc";
        outs << s;
        check(resolveFrameCount(outs) == 1,
              "frame_count 不是数字 -> 当 1 帧(**不猜**)");
    }

    // ---- 超过上限 -> 截断,而且要报出来 ----
    {
        QVector<QJsonObject> outs;
        QJsonObject big; big["frame_count"] = kMaxRunFrames + 500;
        outs << big;
        int reported = -1;
        bool trunc = false;
        const int n = resolveFrameCount(outs, &reported, &trunc);
        check(n == kMaxRunFrames, "超过上限 -> 截到上限");
        check(trunc, "truncated 置上了(调用方必须报出来)");
        check(reported == kMaxRunFrames + 500,
              QString("reported 给的是**原始**报出来的数 %1(截断提示要说这个)")
                  .arg(reported));
    }

    // ---- 刚好等于上限:不算截断 ----
    {
        QVector<QJsonObject> outs;
        QJsonObject e; e["frame_count"] = kMaxRunFrames;
        outs << e;
        bool trunc = true;
        check(resolveFrameCount(outs, nullptr, &trunc) == kMaxRunFrames,
              "刚好等于上限 -> 原样");
        check(!trunc, "刚好等于上限 -> 不算截断");
    }
}

// ============================================================
// 用例 2:两个调用方都必须走这一份
// ============================================================
static void test_both_callers_use_the_shared_rule() {
    beginCase("2. GUI 和 headless 都走同一份帧协议");

    const QString base = QString(W7_SOURCE_DIR);

    // ---- mainwindow.cpp ----
    {
        const QString code = stripComments(readSource(base + "/mainwindow.cpp"));
        check(code.contains("w7::resolveFrameCount"),
              "mainwindow.cpp 走的是 w7::resolveFrameCount(...)");
        // ⚠ 旧的内联实现不许再出现 —— 那是抄副本的本体。
        check(!code.contains("contains(\"frame_count\")"),
              "mainwindow.cpp 里没有自己写的 \"frame_count\" 判据");
    }

    // ---- headless_runner.cpp ----
    {
        const QString code = stripComments(readSource(base + "/headless_runner.cpp"));
        check(code.contains("resolveFrameCount"),
              "headless_runner.cpp 走的是 resolveFrameCount(...)");
        check(!code.contains("contains(\"frame_count\")"),
              "headless_runner.cpp 里没有自己写的 \"frame_count\" 判据");
    }
}

// ============================================================
// 用例 3:日志控件的配置 —— widget 依赖只在一个文件里
// ============================================================
static void test_widget_dependency_is_isolated() {
    beginCase("3. widget 依赖被隔离到一个文件");

    const QString base = QString(W7_SOURCE_DIR);

    // ---- mainwindow 必须调那个共享函数 ----
    {
        const QString code = stripComments(readSource(base + "/mainwindow.cpp"));
        check(code.contains("w7::configureLogView"),
              "mainwindow.cpp 调的是 w7::configureLogView(...)");
    }

    // ---- log_view.h 是**唯一**和 QPlainTextEdit 有关系的地方 ----
    {
        const QStringList inc =
            includeDirectives(readSource(base + "/log_view.h"));
        bool hasIt = false;
        for (const QString& i : inc)
            if (i == "QPlainTextEdit") hasIt = true;
        check(hasIt, "log_view.h 确实 include 了 QPlainTextEdit(它就是那一个地方)");
    }
}

// ============================================================
// 用例 4:纯层不许拖 widget —— **读得懂**的那道闸
// ============================================================
//
// ⚠ 真正的闸是**编译期**的(这个目标只链 Qt6::Core)。
//   但破坏它的时候失败在**构建**那一步,第一反应容易是"环境坏了"。
//   所以这里把同一件事用中文再说一遍 ——
//   让下一个人的第一反应变成"哦,是我把 widget 拖进纯层了"。
//
// ⚠ 判据是**指令行**,不是文本:
//   includeDirectives 只认真正开头的 "#include",
//   注释里写一行 "#include <QWidget>" 不算数。
// ============================================================
static void test_pure_headers_drag_no_widgets() {
    beginCase("4. 纯层不许拖 widget / OpenCV");

    const QString base = QString(W7_SOURCE_DIR);

    // 这些是本项目实际用到的 widget 头。
    // ⚠ 不是"凡是 Q 开头的都不许"—— QJsonObject / QString 也是 Q 开头。
    const QStringList widgetHeaders = {
        "QPlainTextEdit", "QTextEdit", "QWidget", "QMainWindow", "QDockWidget",
        "QGraphicsView", "QGraphicsItem", "QGraphicsScene", "QLabel",
        "QPushButton", "QTreeWidget", "QStackedWidget", "QDialog",
        "QMessageBox", "QSpinBox", "QLineEdit", "QComboBox", "QMenu",
        "QAction", "QSlider", "QCheckBox", "QTableWidget",
    };

    // ⚠ 引号形式的也要查:
    //   #include "mainwindow.h" 拖的 widget 比 <QWidget> 还多 ——
    //   它把整个画布、算子库、信号槽都拖进来。
    const QStringList guiOwningHeaders = {
        "mainwindow.h", "operator_node.h", "connection_line.h",
        "pipeline_view.h", "pipeline_serialize.h", "select_delete_plan.h",
        "param_form.h", "operator_base.h", "operator_defaults.h",
        "dl_postprocess.h", "log_view.h",
    };

    const QStringList pureHeaders = {
        "run_limits.h", "run_policy.h", "frame_verdict.h", "run_engine.h",
        "pipeline_graph.h", "result_render_plan.h", "step_frame_cache.h",
        "frame_record.h", "headless_runner.h",
    };

    for (const QString& h : pureHeaders) {
        const QStringList inc = includeDirectives(readSource(base + "/" + h));
        QStringList bad;
        for (const QString& i : inc) {
            if (widgetHeaders.contains(i)) bad.append(i);
            if (guiOwningHeaders.contains(i)) bad.append(i);
            if (i.startsWith("opencv2/")) bad.append(i);
        }
        check(bad.isEmpty(),
              QString("%1 没拖 widget / OpenCV%2")
                  .arg(h, bad.isEmpty() ? QString()
                                        : QString("(实际拖了: %1)").arg(bad.join(", "))));
    }
}

// ============================================================
// 用例 5:那道编译期的闸本身不能被拆掉
// ============================================================
//
// ⚠ 如果有人往 w7_pure_test 的 target_link_libraries 里加上
//   Qt6::Widgets 或 ${OpenCV_LIBS},那么**编译期的那道闸就没了** ——
//   而这个套件照样全绿(它只是又拖进来一个 widget 而已)。
//
//   那就是「守卫自己被改掉」——
//   本项目栽过:变异 M154 把面板改成不用共享计划,
//   而守卫被一句注释喂饱,照样全绿。
//
//   所以这里守住**闸的形状**:那个 target 只准链 Qt6::Core。
// ============================================================
static void test_the_compile_time_gate_is_still_there() {
    beginCase("5. 编译期那道闸本身还在");

    const QString base = QString(W7_SOURCE_DIR);
    const QString cm = readSource(base + "/CMakeLists.txt");
    check(!cm.isEmpty(), "能读到 CMakeLists.txt");

    // 只看 w7_pure_test 那个 target 的那一段
    const int start = cm.indexOf("w7_pure_test");
    check(start >= 0, "CMakeLists.txt 里有 w7_pure_test 这个目标");
    if (start >= 0) {
        // 从 w7_pure_test 往后取到下一个 add_executable 为止
        QString block = cm.mid(start);
        const int next = block.indexOf("add_executable", 10);
        if (next > 0) block = block.left(next);
        // ⚠ CMake 的注释是 '#' 开头的 —— 先去掉,免得注释里的字样被当成配置
        QStringList lines;
        for (const QString& raw : block.split(QLatin1Char('\n'))) {
            const QString l = raw.trimmed();
            if (l.startsWith(QLatin1Char('#'))) continue;
            lines.append(l);
        }
        const QString cleaned = lines.join(QLatin1Char('\n'));

        check(cleaned.contains("Qt6::Core"),
              "w7_pure_test 链了 Qt6::Core");
        check(!cleaned.contains("Qt6::Widgets"),
              "w7_pure_test **没有**链 Qt6::Widgets(那会把闸拆掉)");
        check(!cleaned.contains("OpenCV_LIBS"),
              "w7_pure_test **没有**链 OpenCV(那会把闸拆掉)");
    }
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 纯层(零 Widgets / 零 OpenCV)回归测试";

    test_resolve_frame_count();
    test_both_callers_use_the_shared_rule();
    test_widget_dependency_is_isolated();
    test_pure_headers_drag_no_widgets();
    test_the_compile_time_gate_is_still_there();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
