// ============================================================
// W7 Run 帧循环的"不许 runaway"回归测试
//
// ============================================================
// 起因:2026-10-08 用户报"还是容易卡死",且明确是**点 Run 跑批时**,
// 表现是纯卡住、不弹任何框。
//
// 先说清楚这个测试在测什么、以及**不**在测什么:
//
// 不测渲染快不快。实测画布逻辑 3 us/步、渲染 1 ms/帧 @40 节点,
// 画布无辜。测它等于测 Qt 本身,测不出我们自己的东西。
//
// 测的是三条"规则"本身(见 run_policy.h):
//   1) 帧数封顶  —— 循环上界是数据给的,数据不受控,所以必须封
//   2) UI 抽帧限频 —— processEvents 会真的重绘,不是"刷新一下"
//   3) 可取消 + 禁止重入 —— 用户得有退路,中途改场景得被拦住
//
// 为什么值得测:这三条**都不崩**。帧数不封顶照样能跑完,只是要等;
// 抽帧不限频照样出结果,只是界面卡。所以它们天然不会被"跑一遍看有没有
// 报错"发现 —— 而这类"不崩只是慢/只是挂住"的缺陷,正是这个项目
// 前两次都靠读代码猜、猜错两次的那一类。
// ============================================================

#include <QApplication>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <windows.h>

#include "run_policy.h"
#include "step_scope.h"
#include "artifact_path.h"
#include "step_frame_cache.h"
#include "frame_record.h"   // 帧接线的可测入口(见该文件头的变异 M4 记录)

using namespace w7;

// ============================================================
// 极简断言(与其余 8 个测试同一套风格)
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
// 用例 1:帧数封顶
//
// 这条是整条链的入口闸门。文件夹源报的 frame_count 就是目录里
// 文件的数量,指向一个大目录就是几万轮同步跑 —— 循环上界来自数据,
// 而数据不受控。回归的样子很朴素:报 50000 帧,只该跑 2000 帧。
// ------------------------------------------------------------
static void test_clamp_frame_count() {
    beginCase("帧数封顶:源报多少都不能照单全收");

    // 正常值原样通过
    check(clampFrameCount(1) == 1,    "报 1 帧 -> 跑 1 帧");
    check(clampFrameCount(8) == 8,    "报 8 帧 -> 跑 8 帧");
    check(clampFrameCount(2000) == 2000, "报 2000 帧(正好等于上限)-> 跑 2000 帧");

    // 越界必须被压住,而且要**告诉调用方**压过,
    // 因为静默截断 = 用户以为全跑完了
    bool truncated = false;
    const int big = clampFrameCount(50000, &truncated);
    check(big == kMaxRunFrames,
          QString("报 50000 帧 -> 只跑 %1 帧(实际 %2)").arg(kMaxRunFrames).arg(big));
    check(truncated, "超上限时 truncated=true(调用方据此在日志里说明)");

    // 没超上限时不能误报截断 —— 否则日志里会出现"只跑前 N 帧"这种
    // 莫名其妙的提示,而实际全跑了
    truncated = true;
    // 变量别叫 small —— Windows SDK 把它定义成 char 的宏,
    // 在 windef.h 里。撞上它的报错是莫名其妙的 C2632,很费时间。
    const int normal = clampFrameCount(37, &truncated);
    check(normal == 37, "报 37 帧 -> 跑 37 帧");
    check(!truncated, "没超上限时 truncated=false(不误报)");

    // 源报 0(空目录)或负数(参数填错)时,至少要跑第 0 帧,
    // 否则循环一次都不进,用户看到的是"点了 Run 没反应"
    check(clampFrameCount(0)  == 1, "报 0 帧 -> 至少跑第 0 帧(不然像没反应)");
    check(clampFrameCount(-5) == 1, "报 -5 帧 -> 同样当 1 帧,不进死循环也不空转");
    check(clampFrameCount(-5, &truncated) == 1 && !truncated,
          "负数不算截断(它是坏数据,不是超限,提示语要不一样)");
}

// ------------------------------------------------------------
// 用例 2:UI 抽帧限频
//
// processEvents() 名字像"处理一下事件",实际会把挂起的重绘真的画出去。
// 以前每个节点无条件调一次,而每调一次就要把整个日志文档
// (行数 = 帧数 x 节点数,而且还在变大)重排版+重画。实测单行代价
// 1545 us 且随行数上涨 —— 这就是"跑着跑着卡死"。
//
// 所以关键不是"调不调",是"调多密"。
// ------------------------------------------------------------
static void test_ui_pump_throttle() {
    beginCase("UI 抽帧限频:再快也最多每秒 10 次");

    check(!shouldPumpUi(0),   "距上次 0 ms    -> 不抽帧");
    check(!shouldPumpUi(1),   "距上次 1 ms    -> 不抽帧");
    check(!shouldPumpUi(99),  "距上次 99 ms   -> 不抽帧(还差 1 ms)");
    check(shouldPumpUi(100),  "距上次 100 ms  -> 抽帧(正好到间隔)");
    check(shouldPumpUi(101),  "距上次 101 ms  -> 抽帧");
    check(shouldPumpUi(5000), "距上次 5000 ms -> 抽帧");

    // 第 0 帧必须立刻抽一次,否则用户点了 Run 之后第一眼看到的是
    // "什么都没发生",要等 100 ms 才知道程序活着。
    // 起点取负的间隔 = "上一次抽帧在无限久以前",这是主窗口里的写法。
    check(shouldPumpUi(2 - (-kUiPumpIntervalMs)),
          "第 0 帧(上次抽帧 = 无限久以前)立刻抽帧,不用等满一个间隔");

    // 限频真正要保证的性质:一段时间内抽帧次数有上界。
    // 只测边界值不够 —— 边界对了但调用方每秒调 1000 次也没用,
    // 所以这里按"每 10 ms 醒一次、醒 1 秒"的节奏,照着主窗口的
    // 调用方式数一遍。
    //
    // 注意参数是 **距上次抽帧** 的时长,不是当前时刻。
    // 第一版这里传的是 t(当前时刻),结果 100 之后的每个 t 都 >= 100,
    // 数出 90 次 —— 测的是"函数被误用"而不是"限频生效"。
    // 传错参数的测试比没有测试更坏:它红了会让人去改对的代码。
    int pumps = 0;
    qint64 last = -kUiPumpIntervalMs;          // 与 onRunClicked 一致的起点
    for (int t = 0; t < 1000; t += 10) {
        if (shouldPumpUi(qint64(t) - last)) { last = t; ++pumps; }
    }
    // 1 秒 / 100 ms 间隔,首帧那次算在里面 -> 约 10 次。
    // 只断言上界,给一点余量:以后把间隔微调成 95 ms 也不该红。
    check(pumps <= 11, QString("1 秒内抽帧 %1 次(上界 11,间隔 %2 ms)")
          .arg(pumps).arg(kUiPumpIntervalMs));
    check(pumps >= 8, QString("限频没过头:1 秒内至少抽了 %1 次(不能把 UI 卡成不刷新)").arg(pumps));
}

// ------------------------------------------------------------
// 用例 3:可取消 + 禁止重入
//
// 这两个都是"状态机"级别的,写错的后果不是报错而是挂住:
//   · 重入:Run 里调 processEvents,用户再点一次 Run,第二次点击被
//     派发进来 -> 整个 onRunClicked 重入一层,两个帧循环叠着跑。
//   · 漏掉 endRun:任何一次 early return 没还原状态,程序就永久
//     锁在"正在跑",之后再也点不动 Run。
// ------------------------------------------------------------
static void test_run_state_machine() {
    beginCase("Run 状态机:不重入、能取消、能复位");

    RunState s = RunState::Idle;
    check(!isRunning(s), "初始是 Idle");
    check(beginRun(&s), "Idle -> Running 允许");
    check(isRunning(s), "Running 时 isRunning=true");

    // 重入必须被拒 —— 这是"点两次 Run 卡死"的直接防线
    check(!beginRun(&s), "Running 时再 beginRun 被拒(防重入)");
    check(!beginRun(&s), "连点三次 Run 依然被拒");
    check(isRunning(s), "被拒的重入不会把状态改坏(仍是 Running)");

    // 取消
    check(requestCancel(&s), "Running -> Cancelling 允许");
    check(isRunning(s), "Cancelling 仍然算'在跑'(直到当前帧跑完)");
    // 重复点 Stop:状态没变,返回 false。
    // 这一点有实际用处 —— 工具栏用返回值决定要不要打"已请求停止",
    // 不然连点三下会打三行一样的提示。
    check(!requestCancel(&s), "重复点 Stop 状态没变 -> 返回 false(不重复打提示)");

    // 复位 —— 漏掉这一步的表现是"跑一次之后再也点不动 Run"
    endRun(&s);
    check(!isRunning(s), "endRun 后回到 Idle");
    check(beginRun(&s), "复位之后还能再次开始(没被永久锁死)");
    endRun(&s);

    // 不在跑时点 Stop:必须拒绝,否则会留下一个"已请求停止"的脏状态,
    // 下次 Run 被自己的 beginRun 拒掉,表现同样是"点不动"
    RunState idle = RunState::Idle;
    check(!requestCancel(&idle), "Idle 时点 Stop 被拒");
    check(!isRunning(idle), "被拒的 Stop 不会污染状态");
    check(beginRun(&idle), "Idle 时 Run 依然能开始");
    endRun(&idle);
}

// ------------------------------------------------------------
// 用例 4:模拟真实帧循环,验证三条规则合起来是有效的
//
// 前三个用例验的是"规则本身对不对",这个验的是"接线对不对" ——
// 规则写得再对,循环里没接上也没用。
// 这里的循环刻意照着 onRunClicked 的骨架写:帧边界查取消、
// 循环上界来自 clampFrameCount。
// ------------------------------------------------------------
static void test_frame_loop_honours_policy() {
    beginCase("帧循环:同时受封顶和取消约束");

    // (a) 源报 50000 帧,循环只该跑 kMaxRunFrames 轮
    {
        RunState st = RunState::Idle;
        beginRun(&st);
        int total = clampFrameCount(50000);
        int ran = 0;
        for (int frame = 0; frame < total; ++frame) {
            if (st == RunState::Cancelling) break;
            ++ran;
        }
        endRun(&st);
        check(ran == kMaxRunFrames,
              QString("源报 50000 帧,循环实际只跑 %1 轮(期望 %2)")
                .arg(ran).arg(kMaxRunFrames));
    }

    // (b) 第 3 帧点 Stop,循环必须停下来 —— 而且是"跑完这帧就停"
    {
        RunState st = RunState::Idle;
        beginRun(&st);
        int total = clampFrameCount(1000);
        int ran = 0;
        for (int frame = 0; frame < total; ++frame) {
            if (st == RunState::Cancelling) break;
            ++ran;
            if (ran == 3) requestCancel(&st);   // 跑完第 3 帧用户点了 Stop
        }
        endRun(&st);
        check(ran == 3, QString("第 3 帧按 Stop,循环停在 3 轮(实际 %1)").arg(ran));
    }

    // (c) 控制组:不点 Stop 就该把 total 轮跑完。
    // 没有这条,"(b) 停在 3"可能是因为循环根本没跑起来而误判通过 ——
    // 这就是"控制组断言"的作用:证明修复没有过头。
    {
        RunState st = RunState::Idle;
        beginRun(&st);
        int total = clampFrameCount(10);
        int ran = 0;
        for (int frame = 0; frame < total; ++frame) {
            if (st == RunState::Cancelling) break;
            ++ran;
        }
        endRun(&st);
        check(ran == 10, QString("不点 Stop 时 10 帧要真的跑完 10 轮(实际 %1)").arg(ran));
    }
}

// ------------------------------------------------------------
// 用例 5:日志行数上限是真的封住了
//
// 这一条特意调的是 **configureLogView()** —— 和主窗口同一个函数。
// 之前这个测试自己 new 了个 QPlainTextEdit 再设上限,那只证明了
// "setMaximumBlockCount 这个机制管用",完全没证明"主窗口调了它";
// 把 mainwindow 里那行删掉测试照样全绿。测平行宇宙等于没测。
// ------------------------------------------------------------
static void test_log_document_is_bounded() {
    beginCase("日志文档行数被封顶(调的是主窗口同一个函数)");

    QPlainTextEdit log;
    configureLogView(&log);                  // ← 与 mainwindow.cpp 同一段代码
    check(log.isReadOnly(), "configureLogView 把它设成只读(日志不该能被编辑)");
    check(log.document()->maximumBlockCount() == kLogMaxLines,
          QString("行数上限就是 kLogMaxLines = %1(实际 %2)")
            .arg(kLogMaxLines).arg(log.document()->maximumBlockCount()));

    // 写 1.2 倍于上限的行:应该稳定停在上限,而不是 1.2 倍
    const int kLines = kLogMaxLines + kLogMaxLines / 5;
    for (int i = 0; i < kLines; ++i) {
        log.appendPlainText(QString("line %1").arg(i));
    }
    check(log.document()->blockCount() <= kLogMaxLines,
          QString("写 %1 行后文档只有 %2 块(上限 %3)")
            .arg(kLines).arg(log.document()->blockCount()).arg(kLogMaxLines));

    // 封顶之后最新的行必须还在 —— 丢的是最老的。
    // 如果实现是"写满就整个清空",上面那条会过,但用户会发现日志
    // 一片空白,那比不封顶还糟。
    check(log.toPlainText().contains(QString("line %1").arg(kLines - 1)),
          "封顶后保留的是最新行,不是清空");

    // 对照组:不调这个函数的那份,同样写 kLines 行会明显更大。
    // 有了这条,"块数 <= 上限"才不是在自证 —— 差异必须是真的。
    QPlainTextEdit unbounded;
    for (int i = 0; i < kLines; ++i) {
        unbounded.appendPlainText(QString("line %1").arg(i));
    }
    check(unbounded.document()->blockCount() > log.document()->blockCount(),
          QString("不封顶的那份确实更大(%1 块 vs %2 块)—— 上限真的在起作用")
            .arg(unbounded.document()->blockCount()).arg(log.document()->blockCount()));
}

// ------------------------------------------------------------
// 用例 6:长跑时日志重绘的单行代价不应该随行数明显变差
//
// 这条直接对着"越跑越卡"这个症状。写法上不设一个硬性能阈值
// (那会让测试在不同机器上飘),而是比较**同一个控件在不同时点**的
// 单行代价:前半段和后半段差得越多,说明代价随行数上涨得越厉害。
//
// 这里用小上限(而不是 kLogMaxLines)是为了跑得快 —— 测的是曲线**形状**,
// 封顶之后曲线应该基本平,和上限具体是 500 还是 5000 无关。
// 代价曲线的绝对值已经由 w7_bench 那次实测给过(785 us/行)。
// ------------------------------------------------------------
static void test_log_cost_stays_flat() {
    beginCase("封顶后:日志重绘代价不随行数暴涨");

    QPlainTextEdit log;
    log.resize(800, 500);
    configureLogView(&log, 500);            // 同一个函数,换个上限只为提速
    log.show();
    QApplication::processEvents();

    const int kTotal = 2000;                // 上限的 4 倍,足够进入稳态
    const QString filler(180, QLatin1Char('x'));

    QElapsedTimer t;
    qint64 firstHalf = 0, secondHalf = 0;
    t.start();
    for (int i = 0; i < kTotal; ++i) {
        log.appendPlainText(QString("[12:00:00] op_%1 -> %2").arg(i).arg(filler));
        log.viewport()->repaint();          // 模拟 processEvents 逼出来的那次重绘
        if (i == kTotal / 2 - 1) firstHalf = t.elapsed();
    }
    secondHalf = t.elapsed() - firstHalf;

    const double perFirst = firstHalf / double(kTotal / 2);
    const double perSecond = secondHalf / double(kTotal - kTotal / 2);
    const double ratio = perSecond / qMax(0.001, perFirst);

    qInfo().noquote() << QString("    前半段 %1 us/行,后半段 %2 us/行,比值 %3x")
        .arg(perFirst, 0, 'f', 1).arg(perSecond, 0, 'f', 1).arg(ratio, 0, 'f', 2);
    check(log.document()->blockCount() <= 500,
          QString("跑完 %1 行仍是 %2 块(没涨)")
            .arg(kTotal).arg(log.document()->blockCount()));
    // 宽松阈值:封顶后曲线应该基本平,给 4 倍余量给机器差异。
    // 变异验证过:把 setMaximumBlockCount 去掉,这个比值会明显超标。
    check(ratio < 4.0, QString("后半段没有比前半段慢 4 倍以上(实测 %1x)").arg(ratio, 0, 'f', 2));
    log.hide();
}

// ============================================================
// 单步执行:范围计算
//
// 这组测的是 step_scope.h 里的"只跑到 target 为止"。
//
// 真正要钉的不是 stepScopePrefix 那一行(它只有两行,抄一遍就知道对不对),
// 而是**它所依赖的那条推论**:
//
//     "拓扑序保证每个节点的全部上游都在它前面,
//      所以前缀自动包含全部上游。"
//
// 这条推论如果错了,后果是静默的:范围内某节点的上游没跑,
// 下游拿到空的 nodeOutputs,读到空值或上一帧的残留 ——
// 不崩、不报错,只有结果悄悄错了。所以用 stepScopeHasAllUpstream
// 把它变成可执行的检查:给一条**被打乱的顺序**进去,必须报 false。
// ============================================================

// 一条典型流水线:
//   src -> seg -> blob -> chk        (线性 4 节点)
//   src2 -> seg                       (多前驱测试用)
static QList<QString> sampleOrder() {
    return { "src", "seg", "blob", "chk" };
}
static QList<QPair<QString, QString>> sampleEdges() {
    return { {"src", "seg"}, {"seg", "blob"}, {"blob", "chk"} };
}

static void test_step_scope_includes_target() {
    beginCase("单步范围:必须含 target 本身");

    const auto order = sampleOrder();

    // 用户右键了 blob,就是要看它跑出来的结果 ——
    // 停在它前面等于什么都没发生
    const QList<int> s = stepScopePrefix(order, "blob");
    check(!s.isEmpty(), "target 存在时范围非空");
    check(s.contains(2), "范围包含 blob 自己在内的索引 2");
    check(s.first() == 0, "范围从拓扑序开头开始(src 必须在里面)");
    check(!s.contains(3), "范围不含 blob 后面的 chk");

    // 最后一个节点单步 = 完整跑,这个必须如实说
    const QList<int> last = stepScopePrefix(order, "chk");
    check(last.size() == 4, "对最后一个节点单步,范围就是全链(4 个)");

    // 第一个节点单步 = 只跑它自己
    const QList<int> first = stepScopePrefix(order, "src");
    check(first.size() == 1, "对第一个节点单步,范围只有 1 个");

    // 同一节点反复单步,结果必须一样(幂等)
    check(stepScopePrefix(order, "blob") == s, "同一目标重复调用结果一致");
}

static void test_step_scope_rejects_unknown_target() {
    beginCase("单步范围:target 不在序里必须返回空");

    const auto order = sampleOrder();
    check(stepScopePrefix(order, "not_in_graph").isEmpty(),
          "找不到的 target 返回空(调用方据此拒绝,而不是静默跑全链)");
    check(stepScopePrefix({}, "src").isEmpty(), "空拓扑序返回空");
}

static void test_step_scope_has_all_upstream() {
    beginCase("单步范围:前缀必须含全部上游");

    const auto order = sampleOrder();
    const auto edges = sampleEdges();

    // 正例:真实拓扑序下,任何节点的前缀都该通过
    for (const QString& tgt : order) {
        const QList<int> s = stepScopePrefix(order, tgt);
        check(stepScopeHasAllUpstream(order, s, edges),
              QString("真实拓扑序下,单步「%1」的范围含全部上游").arg(tgt));
    }

    // 反例:把顺序打乱 —— blob 跑到 seg 前面了。
    // 这是**唯一**能证伪那条推论的输入,必须有。
    const QList<QString> shuffled = { "src", "blob", "seg", "chk" };
    const QList<int> bad = stepScopePrefix(shuffled, "blob");
    check(!bad.isEmpty(), "打乱后范围仍非空(否则测的不是范围而是空返回)");
    check(!stepScopeHasAllUpstream(shuffled, bad, edges),
          "顺序被打乱时必须报 false(前缀漏了 seg)");

    // 只打乱一个不相干的节点,不该误报
    const QList<QString> mostlyOk = { "src", "seg", "chk", "blob" };
    check(!stepScopeHasAllUpstream(mostlyOk,
                                   stepScopePrefix(mostlyOk, "chk"), edges),
          "chk 跑到 blob 前面时必须报 false");
}

static void test_step_scope_diamond_graph() {
    beginCase("单步范围:菱形图(多前驱)也要含全部上游");

    //   src -> seg -> blob -> chk
    //   src -> tpl -------^
    // 单步 chk 时,seg 和 tpl 都必须跑到 ——
    // 漏掉 tpl 就是最典型的"下游拿到空数据却不报错"
    const QList<QString> order = { "src", "seg", "tpl", "blob", "chk" };
    const QList<QPair<QString, QString>> edges = {
        {"src", "seg"}, {"src", "tpl"}, {"seg", "blob"},
        {"tpl", "blob"}, {"blob", "chk"} };

    const QList<int> s = stepScopePrefix(order, "chk");
    check(stepScopeHasAllUpstream(order, s, edges), "菱形图单步末节点通过");

    // 单步 blob:tpl 也必须在范围内
    const QList<int> mid = stepScopePrefix(order, "blob");
    check(stepScopeHasAllUpstream(order, mid, edges), "单步中间节点通过");
    check(mid.contains(order.indexOf("tpl")), "tpl(第二条支路)在范围内");

    // 顺序被打乱:seg 和 tpl 换位,仍然都该在 blob 前面,检查必须仍通过
    const QList<QString> swapped = { "src", "tpl", "seg", "blob", "chk" };
    check(stepScopeHasAllUpstream(swapped,
                                  stepScopePrefix(swapped, "blob"), edges),
          "菱形图两条支路换序后仍通过(顺序不影响正确性)");

    // 反例:tpl 被排到 blob 后面 —— 必须报 false
    const QList<QString> broken = { "src", "seg", "blob", "tpl", "chk" };
    check(!stepScopeHasAllUpstream(broken,
                                   stepScopePrefix(broken, "blob"), edges),
          "支路被排到下游之后时必须报 false");
}

static void test_step_scope_description() {
    beginCase("单步范围:说明文字要能核对范围");

    // 只给个数字没用 —— 用户要知道"跳过几个、总共几个"
    const QString mid = describeStepScope("Blob 计数", 4, 2);
    check(mid.contains("Blob 计数"), "说明里点了节点名");
    check(mid.contains("2"),  "说明里给了跳过的节点数");
    check(mid.contains("4"),  "说明里给了总节点数");
    check(!mid.isEmpty(), "说明非空");

    // 对最后一个节点要如实说"等于完整运行",不能报"跳过 0 个"糊弄
    const QString full = describeStepScope("数量判定", 4, 4);
    check(full.contains("完整") || full.contains("最后一个"),
          QString("对末节点如实说明是完整运行: %1").arg(full));

    // 找不到节点要说清楚,不能返回空串
    check(!describeStepScope("幽灵节点", 4, 0).isEmpty(),
          "范围为空时也要有说明(不能是空串)");
    check(describeStepScope("幽灵节点", 4, 0).contains("幽灵节点"),
          "范围为空的说明里点出了节点名");
}

// ============================================================
// === 中间产物落盘路径(artifact_path.h) ===
//
// 这个纯函数看着像"拼字符串",但它守的是本项目栽过两次的同一个坑:
// 文件名写死 → 帧之间(或节点实例之间)互相覆盖 → 下游读到别人的数据
// → **不崩、不报错、状态点全绿,只有数是错的**。
//
// 它必须同时区分三样东西,少一样就退回老坑:
//   节点实例(node_uuid)、帧号(frame_index)、
//   以及"没有 uuid 的测试路径"(十几条断言直接 imread 那个固定名)
// ============================================================

// 用例:命名必须同时区分节点实例和帧
static void test_artifact_path_separates_node_and_frame() {
    beginCase("落盘路径:节点实例和帧号都要进文件名");

    const QString uuidA = "aaaaaaaa-0000-0000-0000-000000000001";
    const QString uuidB = "bbbbbbbb-0000-0000-0000-000000000002";

    // ① 同算子、同帧、不同节点 -> 必须不同。
    //    这是 2026-10 修过的第一个坑(两个颜色分割互相覆盖)。
    //    注意只断言"路径不同"**不够**:文件名不同但内容被覆盖一样坏。
    //    真正兜住后果的那层在 test_detection_operators 用例 2 ——
    //    它让下游 blob_count 真去读一次,比读回的像素数。
    const QString a0 = artifactPath("color_segment", uuidA, 0);
    const QString b0 = artifactPath("color_segment", uuidB, 0);
    check(a0 != b0,
          QString("同帧不同节点 -> 路径不同(A=%1 B=%2)").arg(a0, b0));

    // ② 同算子、同节点、不同帧 -> 必须不同。
    //    这是本轮要修的第二个坑(帧之间覆盖):
    //    改造前 9 个算子写死 `images/w7_line_fit.png`,
    //    12 帧跑完只剩最后一帧的图。
    const QString a1 = artifactPath("color_segment", uuidA, 1);
    const QString a7 = artifactPath("color_segment", uuidA, 7);
    check(a0 != a1 && a1 != a7,
          QString("同节点不同帧 -> 路径都不同(f0=%1 f1=%2 f7=%3)").arg(a0, a1, a7));

    // ③ 帧号零填充到 4 位:文件名排序 == 帧序,定宽也让覆盖行为可预期
    const QString f7 = artifactPath("line_fit", uuidA, 7);
    const QString f123 = artifactPath("line_fit", uuidA, 123);
    check(f7.contains("_f0007"),  QString("帧号 7 补成 _f0007(实际 %1)").arg(f7));
    check(f123.contains("_f0123"), QString("帧号 123 补成 _f0123(实际 %1)").arg(f123));

    // ④ 算子名要落在路径里 —— 否则两个算子写到同一个文件上,
    //    等于把"节点实例覆盖"缩小成"算子覆盖"
    check(artifactPath("line_fit", uuidA, 0) != artifactPath("circle_fit", uuidA, 0),
          "不同算子 -> 路径不同");

    // ⑤ 只取 uuid 前 8 位。这条不是审美问题:uuid 有 36 个字符,
    //    全塞进文件名会在嵌套目录里先撞 Windows 的路径上限。
    check(!artifactPath("line_fit", uuidA, 0).contains("-0000-"),
          QString("只取 uuid 前 8 位(实际 %1)").arg(artifactPath("line_fit", uuidA, 0)));
}

// 用例:没有 node_uuid 时退回历史文件名
//
// 为什么这条必须钉住:单测直接调 op->run() 时没有 node_uuid,
// 而十几条断言直接 imread 那个固定路径。哪天有人"顺手统一"成新命名,
// 这些断言会全部变成"文件不存在" —— 报红的原因和真实缺陷无关,
// 是最坏的一种红:它逼着人改测试,而不是去查命名。
static void test_artifact_path_falls_back_without_uuid() {
    beginCase("落盘路径:没有 uuid 时退回历史名(测试路径不受影响)");

    // 现状实测:断言 imread 的就是 images/w7_color_segment_mask.png
    // (test_detection_operators 用例 2)。这条守住它。
    const QString legacy = artifactPath("line_fit", QString(), 0);
    check(legacy == "images/w7_line_fit.png",
          QString("无 uuid -> 退回 images/w7_line_fit.png(实际 %1)").arg(legacy));

    // 无 uuid 时帧号**不**进名字。这条路只在测试里走,但必须钉住,
    // 否则下一个人会以为"没 uuid 就等于单帧",再改动就把测试打挂了。
    const QString legacy7 = artifactPath("line_fit", QString(), 7);
    check(legacy7 == legacy,
          QString("无 uuid 时忽略帧号(f0=%1 f7=%2)").arg(legacy, legacy7));

    // 有 uuid 但没帧号(非 Run 引擎调用):仍要区分节点实例,
    // 否则又退回"两个实例互相覆盖"。
    //
    // 注意断言怎么写:判"没有帧号"不能查 "_f" 子串 —— 算子名
    // line_fit 自带一个 _f,那样断言恒假。第一版就是这么写错的,
    // 报红的原因(断言写错)和被测行为(命名规则)毫无关系。
    // 正确的判法是查帧号那一段:文件名以 "_f" + 4 位数字结尾。
    const QString noFrame =
        artifactPath("line_fit", "aaaaaaaa-0000-0000-0000-000000000001", -1);
    static const QRegularExpression frameSuffix(R"(^_f\d{4}\.png$)");
    check(noFrame.contains("aaaaaaaa") && !noFrame.contains(frameSuffix),
          QString("有 uuid 无帧号 -> 只带实例不带帧(实际 %1)").arg(noFrame));
}

// 用例:保留窗口的判据
//
// 图只留最近 N 帧,更早的会被同名覆盖。被覆盖的帧**仍然有数字**,
// 但图没了 —— 此时界面必须明说,不能拿后来的图顶替。
// 拿错帧的图配这帧的数字,比完全没图更坏:用户会以为那就是这一帧。
static void test_artifact_keep_window() {
    beginCase("保留窗口:被覆盖的帧要能判出来");

    check(artifactFrameIsKept(0, 50),   "帧 0 在窗口内");
    check(artifactFrameIsKept(49, 50),  "帧 49(最后一个)在窗口内");
    check(!artifactFrameIsKept(50, 50), "帧 50(第一个溢出)不在窗口内");
    check(!artifactFrameIsKept(1200, 50), "帧 1200 不在窗口内");

    // 负帧号 = 单帧调用(没有帧概念),不该被当成"第 0 帧"
    check(!artifactFrameIsKept(-1, 50), "负帧号(单帧)不在多帧窗口内");

    // 窗口配 0:什么都不留 —— 判据必须照实报,不能默认放行
    check(!artifactFrameIsKept(0, 0), "窗口为 0 -> 连帧 0 也不留");
}

// ============================================================
// === 多帧缓存 (step_frame_cache.h)
// ============================================================
// 这一层最贵的不是"存多份",而是**被覆盖的那一帧不许拿别的帧顶替**。
// 顶替了不会崩、不会报错、界面还挺好看的 —— 用户只会得出一个错的结论。
// 所以下面的用例重心不在"能不能存下来",而在"状态判得对不对、
// 以及丢掉的时候说不说"。
// ============================================================

// 造一条输出。只给几个典型字段,够用来测主图路径的优先级。
static QJsonObject fakeOutput(const QString& imagePath, const QString& maskPath = {}) {
    QJsonObject o;
    o["result"] = "PASS";
    o["score"]  = 0.87;
    if (!imagePath.isEmpty()) o["image_path"] = imagePath;
    if (!maskPath.isEmpty())  o["mask_path"]  = maskPath;
    return o;
}

// ------------------------------------------------------------
// 用例:每帧一份,而不是只留最后一帧
//
// 这是这次改动的**全部意义**。改动前 30 帧里第 7 帧判 FAIL,
// 右键回看只能看到末帧 —— 而末帧 PASS 时,回看反而掩盖了要找的那一帧。
// ------------------------------------------------------------
static void test_cache_keeps_every_frame() {
    beginCase("多帧缓存:每个帧号各存一份");

    StepFrameCache cache;
    const QString node = "aaaaaaaa-0000-0000-0000-000000000001";
    for (int f = 0; f < 12; ++f) {
        QJsonObject o = fakeOutput(QString("images/src_f%1.png").arg(f));
        o["count"] = 10 + f;   // 每帧一个能认出来的数
        cache.put(node, f, o, QString("第 3 步 · 颜色分割"));
    }

    check(cache.hasNode(node), "缓存里有这个节点");
    check(cache.framesOf(node).size() == 12, "12 帧存了 12 份(不是 1 份)");

    // 逐帧取值:每一帧的数必须是她自己那一帧的,不能串。
    // 这条是"回看不再掩盖问题"的判据。
    bool allDistinct = true;
    for (int f = 0; f < 12; ++f) {
        if (cache.at(node, f).output.value("count").toInt() != 10 + f) allDistinct = false;
    }
    check(allDistinct, "逐帧取值,每帧拿到的是自己那一帧的数(没串)");

    // 帧号必须升序 —— 帧选择器(下一轮做)按这个列表画下拉框
    const QList<int> ks = cache.framesOf(node);
    bool ascending = true;
    for (int i = 1; i < ks.size(); ++i) if (ks[i] <= ks[i-1]) ascending = false;
    check(ascending, "帧号列表升序(帧选择器要靠它)");

    // 默认回看仍是最后一帧:这次改动**不改变**用户看到的默认内容,
    // 只是底下的数据从一份变成 N 份。所以 latestFrameOf 必须还是 11。
    check(cache.latestFrameOf(node) == 11, "latestFrameOf = 11(默认仍看最后一帧)");
    check(cache.latestFrame() == 11, "全局 latestFrame = 11");

    // 没缓存过的帧号:at() 返回空,hasFrame() 报 false。
    // 两者必须能区分 —— "没有这一帧"和"这一帧确实没有输出"是两回事,
    // 混起来的话界面会给一个空的详情框,用户以为是算子没跑。
    check(!cache.hasFrame(node, 99), "没存过的帧号 hasFrame=false");
    check(cache.at(node, 99).output.isEmpty(), "没存过的帧号 at() 返回空记录");
    check(cache.latestFrameOf("不存在的节点") == -1, "陌生节点 latestFrameOf=-1");
}

// ------------------------------------------------------------
// 用例:图没了必须判出来,而且**不许**由别的路径顶替
//
// 注入假的 exists 谓词来构造"文件已被覆盖"的世界 —— 真去建文件再删
// 也能造,但那样测的就变成了文件系统,而不是判据本身。
// ------------------------------------------------------------
static void test_cache_reports_missing_image() {
    beginCase("多帧缓存:图被覆盖时要判出来,不能顶替");

    StepFrameCache cache;
    const QString node = "aaaaaaaa-0000-0000-0000-000000000002";

    // 三种帧各来一份:
    //   0 -> 有图,且文件在
    //   1 -> 有图,但文件已经不在(被后来的运行覆盖了)
    //   2 -> 纯数值算子,本来就不产图
    cache.put(node, 0, fakeOutput("images/w7_line_fit_x_f0000.png"),
              QString("第 3 步 · 直线拟合"));
    cache.put(node, 1, fakeOutput("images/w7_line_fit_x_f0001.png"),
              QString("第 3 步 · 直线拟合"));
    cache.put(node, 2, fakeOutput(QString()), QString("第 4 步 · 计数"));

    // 只有 f0000 还在(模拟"跑完下一批,老文件被同名覆盖掉了")
    const ExistsFn onDisk = [](const QString& p) {
        return p.endsWith("_f0000.png");
    };

    check(cache.imageState(node, 0, onDisk) == FrameImageState::Available,
          "帧 0:文件在 -> Available");
    check(cache.imageState(node, 1, onDisk) == FrameImageState::Dropped,
          "帧 1:文件不在 -> Dropped(不许显示别的帧的图)");
    check(cache.imageState(node, 2, onDisk) == FrameImageState::NoImage,
          "帧 2:本来就不产图 -> NoImage(不是 Dropped)");
    check(cache.imageState(node, 99, onDisk) == FrameImageState::Dropped,
          "帧 99:数字都没了 -> Dropped");

    // 三种状态给用户看的话必须**不一样**,尤其 Dropped 那句
    // 必须明说"没有配图"。写成空串 = 界面什么都不说 =
    // 用户以为图还在。Available 才是空串(有图,无需解释)。
    const QString sAvail = StepFrameCache::describeImageState(
        FrameImageState::Available, 0);
    const QString sNoImg = StepFrameCache::describeImageState(
        FrameImageState::NoImage, 2);
    const QString sDrop  = StepFrameCache::describeImageState(
        FrameImageState::Dropped, 1);

    check(sAvail.isEmpty(), "Available 不加任何解释句");
    check(!sNoImg.isEmpty() && !sDrop.isEmpty(), "NoImage / Dropped 都有话说");
    check(sNoImg != sDrop, "NoImage 和 Dropped 的文案不同(不能共用一句)");
    check(sDrop.contains("2") && sDrop.contains("覆盖"),
          QString("Dropped 文案说清是第 2 帧、且说了原因: %1").arg(sDrop));
    check(!sNoImg.contains("覆盖"), "NoImage 不该说'被覆盖' —— 它压根没产过图");

    // 帧号文案走重载传进来,而不是让调用方拿正则去改字符串。
    // 第二版实现就是在 mainwindow 里 .replace("^第 1 帧", "这一帧") ——
    // 那种改法一改文案就静默失配,而失配的表现是"看起来正常,
    // 只是帧号又对不上了"。所以这条钉住文案是**原样**用进去的。
    const QString sDropCustom = StepFrameCache::describeImageState(
        FrameImageState::Dropped, QStringLiteral("这一帧"));
    check(sDropCustom.contains("这一帧") && !sDropCustom.contains("第 1 帧"),
          QString("文案原样传入,不被改写: %1").arg(sDropCustom));

    // 判据必须**每次现场问**,不能是 put 时存下来的标志。
    // 这一条专门钉住:如果哪天改成 put 时算一次存进 StepFrameResult,
    // 下面这两行会立刻分出胜负(第一次查时文件还在,第二次被删)。
    check(cache.imageState(node, 0, onDisk) == FrameImageState::Available,
          "第一次查:帧 0 文件在");
    const ExistsFn gone = [](const QString&) { return false; };
    check(cache.imageState(node, 0, gone) == FrameImageState::Dropped,
          "同一帧换个 exists 就报 Dropped -> 判据是现场的,不是缓存的");
}

// ------------------------------------------------------------
// 用例:主图路径的优先级
//
// 和 renderOutputToResultPanel 的显示顺序必须一致。两份各写一份的话,
// 会出现"判据说 A 图在、界面显示 B 图",而 B 可能已经被覆盖了 ——
// 于是图没了却不报。这正是要防的方向。
// ------------------------------------------------------------
static void test_primary_image_path_priority() {
    beginCase("多帧缓存:主图路径优先级");

    check(stepPrimaryImagePath(fakeOutput("images/x.png", "images/m.png"))
              == "images/m.png",
          "mask_path 优先于 image_path(颜色分割)");

    QJsonObject marked;
    marked["marked_path"] = "images/marked.png";
    marked["image_path"]  = "images/raw.png";
    check(stepPrimaryImagePath(marked) == "images/marked.png",
          "marked_path 优先于 image_path(pcb/dl 的标注图)");

    check(stepPrimaryImagePath(fakeOutput("images/x.png")) == "images/x.png",
          "只有 image_path 时用它");
    check(stepPrimaryImagePath(QJsonObject()).isEmpty(),
          "什么都没有 -> 空串(= NoImage,不是 Available)");
}

// ------------------------------------------------------------
// 用例:上限是保险丝,触发了必须报
//
// kStepCacheMaxFrames == kMaxRunFrames,正常单次 Run 打不到。
// 这里用小的 maxFrames 强制触发,验的是"触发了会怎样",
// 不是"默认上限是多少"。
// ------------------------------------------------------------
static void test_cache_frame_cap_reports() {
    beginCase("多帧缓存:撞上限丢最早的帧,而且要报出来");

    StepFrameCache cache;
    const QString node = "aaaaaaaa-0000-0000-0000-000000000003";

    int evicted = 0;
    for (int f = 0; f < 5; ++f) {
        evicted += cache.put(node, f, fakeOutput(QString("images/x_f%1.png").arg(f)),
                             QString("第 3 步"), /*maxFrames=*/3);
    }
    check(evicted == 2, QString("5 帧进 3 帧的窗口 -> 丢了 2 帧(实际 %1)").arg(evicted));
    check(cache.evictedCount() == 2, "evictedCount 累计=2(调用方据此打日志)");
    check(cache.framesOf(node).size() == 3, "缓存里剩 3 帧");

    // 丢的是**最早**的两帧,不是最新 —— Run 是流式的,
    // 用户早就看过前面那些帧的日志了;而且丢最新会让末帧的回看变空。
    check(!cache.hasFrame(node, 0), "帧 0 已被丢弃");
    check(!cache.hasFrame(node, 1), "帧 1 已被丢弃");
    check(cache.hasFrame(node, 4),  "帧 4(最后一帧)还在");

    // 重复写同一帧**不算**新增:重跑同一帧不该把上限撑爆。
    // 源越界时会一直停在最后一张,没这条的话一个 3 帧窗口会被
    // 最后一帧自己撑掉另外两帧。
    StepFrameCache c2;
    int e2 = 0;
    for (int i = 0; i < 10; ++i) {
        e2 += c2.put(node, 2, fakeOutput("images/x.png"), QString("第 3 步"), 3);
    }
    check(e2 == 0, "同一帧重复写 10 次不触发上限");
    check(c2.framesOf(node).size() == 1, "重复写同一帧只占 1 个位置");

    // 上限 0:什么都不留,而且必须报出来(不能装作成功)。
    // 这里最容易写出"看着在验证其实没验证"的断言 ——
    // 第一版写的是 size()==1 || isEmpty(),两个都过,等于没验。
    StepFrameCache c3;
    const int e3 = c3.put(node, 0, fakeOutput("images/x.png"), QString("第 3 步"), 0);
    check(e3 == 1, "上限 0 -> 这一次就是丢弃,put 返回 1");
    check(!c3.hasFrame(node, 0), "上限 0 -> 帧 0 没存进去");
    check(c3.framesOf(node).isEmpty(), "上限 0 -> 这个节点一条都没留下");

    // 非法输入直接拒掉,不留半截
    StepFrameCache c4;
    check(c4.put(node, -1, fakeOutput("images/x.png"), QString()) == 0,
          "负帧号被拒,且不计入丢弃");
    check(c4.put(QString(), 0, fakeOutput("images/x.png"), QString()) == 0,
          "空节点 id 被拒");
    check(c4.nodeCount() == 0, "被拒的两次没有建出节点条目");

    // 两个上限必须绑定:帧循环最多跑 kMaxRunFrames 帧,
    // 缓存留 kStepCacheMaxFrames 帧。后者 >= 前者时上限打不到。
    // 两个数分开写却不绑定,迟早有人只改其中一个 —— 那时
    // 单次 Run 就会静默丢帧,而界面什么都不说。
    check(kStepCacheMaxFrames >= kMaxRunFrames,
          QString("缓存上限 %1 >= 帧循环上限 %2(否则单次 Run 就撞上限)")
              .arg(kStepCacheMaxFrames).arg(kMaxRunFrames));

    // 清缓存要把累计丢弃数一起归零 —— 否则下一次 Run 会
    // 报一个属于上一次的丢弃数
    cache.clear();
    check(cache.evictedCount() == 0, "clear() 后 evictedCount 归零");
    check(cache.nodeCount() == 0, "clear() 后节点条目清空");
}

// ============================================================
// 用例:帧选择器 —— spin 的序号 ↔ 真实帧号
//
// ------------------------------------------------------------
// 为什么这条要单独测
// ------------------------------------------------------------
// 帧选择器(spin)显示的是「第几个**已缓存**的帧」,标题里显示的是
// **真实帧号**。两者只在丢过帧的时候才不一样 —— 而丢帧是上限撞满才
// 发生,平时根本看不见,所以最容易在这里写出一个"平时对、炸了才错"的
// 映射。测试必须**故意造出丢帧**,让那条例外路径每天都被走到。
//
// 这也正是这次 UI 设计里被特意避开的东西:如果拿真实帧号直接当
// spin 的值,丢帧之后 spin 会指向一个已经不存在的条目 ——
// 界面显示「第 3 帧」,用户以为看的是第 3 帧,其实那是第 5 帧的内容。
// ============================================================
static void test_frame_selector_positions() {
    beginCase("帧选择器:序号映射 + 什么时候藏起来");

    StepFrameCache cache;
    const QString node = "aaaaaaaa-0000-0000-0000-000000000003";

    check(!cache.showFrameSelector(node), "没跑过 -> 不显示选择器");

    cache.put(node, 0, fakeOutput("images/x_f0.png"), QString("第 3 步"), 3);
    check(!cache.showFrameSelector(node),
          "只缓存了 1 帧 -> 不显示选择器(两个方向的按钮都按不动,留着像坏了)");

    // 正常情况(没撞上限):序号和真实帧号一一对应
    cache.put(node, 1, fakeOutput("images/x_f1.png"), QString("第 3 步"), 3);
    cache.put(node, 2, fakeOutput("images/x_f2.png"), QString("第 3 步"), 3);
    check(cache.showFrameSelector(node), "缓存了 3 帧 -> 显示选择器");

    QList<int> ks = cache.framesOf(node);
    check(ks.size() == 3 && ks.at(0) == 0 && ks.at(1) == 1 && ks.at(2) == 2,
          "没撞上限时:序号 n 就是真实帧号 n-1");

    // 撞上限之后 —— 这条是本用例的重点。
    // 再塞一帧把帧 0 挤掉,于是真实帧号变成 {1,2,3}。
    cache.put(node, 3, fakeOutput("images/x_f3.png"), QString("第 3 步"), 3);
    ks = cache.framesOf(node);
    check(ks.size() == 3, "撞上限后仍是 3 帧");
    check(ks.at(0) == 1 && ks.at(1) == 2 && ks.at(2) == 3,
          QString("撞上限后真实帧号是 1,2,3(实际 %1,%2,%3)")
              .arg(ks.at(0)).arg(ks.at(1)).arg(ks.at(2)));
    // 序号 1 指的是**真实帧 1**,不是真实帧 0(已经被丢了)。
    // 这里写成查表而不是 "-1",因为查表才是 UI 实际做的事。
    check(ks.indexOf(0) == -1,
          "丢掉的帧 0 在位置表里查不到 -> spin 永远不会指向它");
    check(ks.indexOf(2) == 1, "真实帧 2 在第 2 个位置(1-based)");
    check(cache.latestFrameOf(node) == 3,
          "末帧是真实帧 3,不是位置 3 恰好撞上");

    // 换节点互不影响 —— UI 的 m_viewNodeId 是全局一份,
    // 万一按错了节点,不能把上一个节点的选择器状态带过来。
    const QString other = "bbbbbbbb-0000-0000-0000-000000000004";
    cache.put(other, 9, fakeOutput("images/y_f9.png"), QString("第 5 步"), 10);
    check(!cache.showFrameSelector(other), "另一个节点只有 1 帧 -> 它的选择器也不显示");
    check(cache.showFrameSelector(node), "上一个节点的选择器状态没被带偏");

    cache.clear();
    check(!cache.showFrameSelector(node), "clear() 之后不再显示选择器");
}

// ============================================================
// 用例:mainwindow 每帧都要喂缓存(源码级守卫)
//
// ------------------------------------------------------------
// 先说清楚这条测的是什么、以及**不**是什么
// ------------------------------------------------------------
// 变异 M4 把 mainwindow.cpp 里的 m_stepCache.put() 重新包回
//     if (frame == totalFrames - 1 || totalFrames == 1)
// ——也就是把代码改回这次改动之前的样子。跑完整套:**12/12 全绿**。
//
// 原因:mainwindow.cpp 不在任何测试目标里(它要真窗口 + 全部算子,
// 无头构造不了),所以上面 128 条断言一条都没碰到这段代码。
// 这类"改回去也没人发现"正是本项目记过的第 12 条教训
// (见 memory/feedback-verification-doesnt-verify.md 第 12 条)。
//
// 这条守卫是唯一能机械拦住它的办法,但它的能力**很有限**,别夸大:
//   ✓ 认得 "totalFrames - 1" 这种 last-frame-only 的写法
//   ✗ 换成 latestFrameOf(...) 提前 return —— 它就看不见了
//   ✗ 换成任何别的等价写法 —— 同样看不见
//
// 所以它是一条 **lint**,不是行为测试。放进测试套件是因为
// 放进来至少比不放强,并且能让人在改坏的那天立刻看见 ——
// 但它**不能**被当作"接线已经验证过了"的证据。
// 想真正验证接线,得能无头构造 MainWindow,那是另一件事。
// ------------------------------------------------------------
// 去掉 C++ 行注释,但**保留字符串字面量里的内容**。
//
// 为什么必须去注释:守卫要找的 `totalFrames - 1` 这行,恰好
// 也被我写在了"这里以前有个 last-frame-only 判断"的说明注释里
// (记录为什么改是必要的)。不去注释的话守卫第一天就误报 ——
// 而一个对自身文档误报的守卫,下一个人只会把它删掉。
//
// 为什么要区分字符串:src 里可能有 "D://xxx" 这种路径,
// 按行 naively 地切 // 会把后面真代码一起切掉。
static QString stripLineComments(const QString& src) {
    QString out;
    out.reserve(src.size());
    bool inStr = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) {           // 转义,跳过下一个字符
                out += c;
                if (i + 1 < src.size()) out += src[++i];
                continue;
            }
            if (c == QLatin1Char('"')) inStr = false;
            out += c;
            continue;
        }
        if (c == QLatin1Char('"')) { inStr = true; out += c; continue; }
        if (c == QLatin1Char('/') && i + 1 < src.size() &&
            src[i + 1] == QLatin1Char('/')) {
            while (i < src.size() && src[i] != QLatin1Char('\n')) ++i;
            if (i < src.size()) out += QLatin1Char('\n');
            continue;
        }
        out += c;
    }
    return out;
}

// ============================================================
// 找 mainwindow.cpp
//
// ⚠ 2026-10-20:不能写死相对路径 "mainwindow.cpp"。
//
//   以前工作目录是工程根(W7_VisionMasterLite/),相对路径恰好能打开。
//   现在工作目录改成了 build 里的测试目录
//   (为了不让测试产物落进源码树),相对路径就**打不开了**。
//
//   而"打不开"这件事本身有段血案史:
//   第一版是 `if (!opened) return;` —— 找不到文件就直接返回,
//   于是一条断言都没跑,测试照样全绿。
//   那是**恒真的空断言**:比没有守卫更坏,因为它让人以为守卫在生效。
//
//   所以现在:路径自己往上找,找不到就**报红**。
// ============================================================
static QString findMainWindowCpp() {
    QDir dir = QDir::current();
    for (int i = 0; i < 8; ++i) {
        const QString cand = dir.filePath("mainwindow.cpp");
        if (QFile::exists(cand)) return cand;
        if (!dir.cdUp()) break;
    }
    return QString();
}

static void test_mainwindow_records_every_frame() {
    beginCase("源码守卫:mainwindow 不能退回只缓存最后一帧");

    const QString mwPath = findMainWindowCpp();
    QFile f(mwPath);
    const bool opened = f.open(QIODevice::ReadOnly | QIODevice::Text);
    // 打不开文件**必须报红**,不能 return 了事。
    // 第一版写的是 "if (!opened) return;" —— 而 CMake 里
    // add_test(run_policy) 没给 WORKING_DIRECTORY,ctest 在 build/ 下跑,
    // 找不到 mainwindow.cpp,于是这条守卫每次都提前 return,
    // 一条断言都没跑,测试照样全绿。
    //
    // 那是"恒真的空断言":比没有守卫更坏,因为它让人以为守卫在生效。
    // 所以这里 check 的是 false 分支 —— 路径找不到就当场报红,
    // 而不是"这条守卫今天不算数"。
    check(opened,
          "能打开 mainwindow.cpp —— 打不开说明它不在预期位置("
          "找过 " + QDir::currentPath() + " 往上 8 层),"
          "这条源码守卫会变成恒真的空断言");
    if (!opened) return;
    const QString src = stripLineComments(QString::fromUtf8(f.readAll()));
    f.close();

    // 锚点:多帧缓存真正落地的那次调用
    //
    // ⚠ 2026-10-20:锚点从 "m_stepCache.put(" 改成了
    //   "recordFrameResult(" —— 因为"记一笔"这一步已经抽到
    //   frame_record.h 里,现在**有行为测试**覆盖它
    //   (见 test_frame_record_behaviour)。
    //
    //   这条 lint 的职责因此也变了:从"禁止某种写法"变成
    //   "**必须用这个可测的入口**"。后者比前者强 ——
    //   禁止某个字符串会被等价写法绕过,要求某个入口不会。
    const int putAt = src.indexOf(QStringLiteral("recordFrameResult("));
    check(putAt >= 0,
          "mainwindow.cpp 里调的是 recordFrameResult() —— "
          "\"每帧都记\"这条不变量现在有行为测试覆盖,"
          "绕开这个入口就绕开了测试");

    // 老 bug 的判据。改动前这行就在 put 调用正上方,
    // 把每一次 put 都关在"只留最后一帧"的条件里。
    const int stale = src.indexOf(QStringLiteral("totalFrames - 1"));
    check(stale < 0,
          QString("mainwindow.cpp 代码里没有 'totalFrames - 1' 这个 "
                  "last-frame-only 判据(它会把记缓存关成只缓存最后一帧 —— "
                  "实测把它加回去,12 个套件全绿,没有任何行为测试会发现)"
                  "(已剔除注释,所以本文件里记录这段历史的那段说明不会误报)"));

    // 反向自查:不能把老判据整个字符串换成别的写法后,
    // 这条守卫就变成"查一个没人写的字符串"—— 那种断言恒真,
    // 比没有还坏。所以这里确认锚点本身还在,证明守卫没落空。
    check(src.contains(QStringLiteral("m_stepCache.imageState(")),
          "onViewStepResultRequested 里有 imageState() 调用"
          "(守卫本身没落空的反向自查)");
}

// ------------------------------------------------------------
// 同一条 lint 的第二个锚点:帧选择器必须走 framesOf 的位置表
//
// ------------------------------------------------------------
// 变异 M5 把 spin 的回调从
//     showStepFrame(m_viewNodeId, fs.at(pos - 1));
// 改成
//     showStepFrame(m_viewNodeId, pos - 1);        // 拿序号当帧号
//
// 平时**看不出任何区别** —— 没撞过上限时 pos-1 恰好就是帧号。
// 只有真的丢过帧(上限 2000 撞满)之后,spin 的第 1 个位置会指向
// 已经被丢掉的第 0 帧,于是 showStepFrame 走 hasFrame 失败那条路,
// 用户点 [◀] 得到一行"[每步] 第 1 帧已经不在缓存里了",翻不动帧。
//
// 这条同样是 mainwindow.cpp,同样只能 lint。
// 它的能力边界和上面那条一模一样:认得这一种写法,换个等价写法
// 就看不见。别把它当成"帧选择器的映射已经验证过了"。
// ------------------------------------------------------------
static void test_mainwindow_frame_selector_maps_through_framesOf() {
    beginCase("源码守卫:帧选择器必须经 framesOf 换算,不能拿序号当帧号");

    QFile f(findMainWindowCpp());
    const bool opened = f.open(QIODevice::ReadOnly | QIODevice::Text);
    check(opened,
          "能打开 mainwindow.cpp —— 打不开说明它不在预期位置("
          "找过 " + QDir::currentPath() + " 往上 8 层),"
          "这条源码守卫会变成恒真的空断言");
    if (!opened) return;
    const QString src = stripLineComments(QString::fromUtf8(f.readAll()));
    f.close();

    // 正锚点:确实是从 framesOf() 的位置表里取的。
    // 没有它,下面那条负向断言就可能只是在查一个根本不存在的写法
    // —— 那种断言恒真,比没有还坏。
    check(src.contains(QStringLiteral("m_stepCache.framesOf(")),
          "mainwindow.cpp 里有 m_stepCache.framesOf() 调用"
          "(负向断言的正锚点,证明它不是在校验一个没人写的字符串)");

    // ⚠ 2026-10-20:换算本身抽到了 frame_record.h 的
    //   frameNumberForPosition(),现在**有行为测试**覆盖
    //   (含"丢过帧之后序号≠帧号"这个关键场景)。
    //   所以这条 lint 的职责变成"必须走那个可测的换算函数"。
    check(src.contains(QStringLiteral("frameNumberForPosition(")),
          "帧选择器走的是 frameNumberForPosition() —— "
          "序号→帧号的换算有行为测试覆盖(含丢帧后的错位场景)");

    // 负锚点:把 pos-1 直接当帧号传进去。
    // 注意匹配的是 "showStepFrame(m_viewNodeId, pos - 1)" 这种
    // **完整的调用形态**,不是裸的 "pos - 1" —— 后者在无关的地方
    // 也会出现,守卫会开始误报,误报的守卫迟早被删。
    check(!src.contains(QStringLiteral("showStepFrame(m_viewNodeId, pos - 1)")),
          "帧选择器没有把 spin 的序号直接当帧号传给 showStepFrame —— "
          "丢过帧之后序号和真实帧号会错开,那样翻帧会指向已丢弃的帧"
          "(实测没有任何行为测试会发现:mainwindow.cpp 不在测试目标里)");
}

// ============================================================
// 用例:帧接线的**行为**测试(不只是 lint)
// ============================================================
//
// 上面那两条 lint 守的是"mainwindow.cpp 里有没有这么写"。
// 它们挡不住"换个等价写法"—— 这是 lint 的固有上限,注释里写清楚了。
//
// 2026-10-20:帧循环里"记一笔"和"序号→帧号换算"抽到了
// frame_record.h,于是这两件事**本身**可以被行为测试覆盖 ——
// 不再只能读源码。
//
// 变异 M4(把"每帧都记"改回"只记最后一帧")现在会让
// 下面第 2 条断言**立刻变红**,而不是像以前那样 12/12 全绿。
//
// ------------------------------------------------------------
static void test_frame_record_behaviour() {
    beginCase("帧接线:每帧都记 + 序号→帧号换算(行为测试)");

    // --- 1. shouldCacheFrame:每一帧都要记 ---
    //
    // 变异 M4 把它改回 "frame == totalFrames - 1" 时,这一条立刻变红。
    bool allCached = true;
    for (int f = 0; f < 30; ++f) {
        if (!w7::shouldCacheFrame(f, 30)) allCached = false;
    }
    check(allCached,
          "30 帧里每一帧都判为\"要记\"(不是只记最后一帧 —— "
          "只记末帧的话,第 7 帧判 FAIL 而末帧 PASS 时,"
          "\"回看\"这个查错入口反而掩盖了要查的那一帧)");

    // --- 2. recordFrameResult:真的驱动它跑 30 帧 ---
    StepFrameCache cache;
    const QString node = "bbbbbbbb-0000-0000-0000-00000000000f";
    for (int f = 0; f < 30; ++f) {
        QJsonObject out;
        out["value"] = f;
        out["image_path"] = QString("images/x_f%1.png").arg(f);
        w7::recordFrameResult(&cache, node, f, out, QString("第 1 步"));
    }

    const QList<int> ks = cache.framesOf(node);
    check(ks.size() == 30,
          QString("跑完 30 帧,缓存里应该有 30 帧(实际 %1)")
              .arg(ks.size()));

    // 每一帧的**数字**都必须还在(数字全帧留,图才只留最近 N 帧)。
    int missing = 0, wrongValue = 0;
    for (int f = 0; f < 30; ++f) {
        if (!cache.hasFrame(node, f)) { ++missing; continue; }
        const StepFrameResult r = cache.at(node, f);
        if (r.output.value("value").toInt(-1) != f) ++wrongValue;
    }
    check(missing == 0,
          QString("30 帧的数字一个都不能少(少了 %1 帧)—— "
                  "用户在右键里看不到的那一帧,就是查不出来的那一帧")
              .arg(missing));
    check(wrongValue == 0,
          QString("每一帧存的数字要和它自己的帧号对得上(错了 %1 帧)")
              .arg(wrongValue));

    // --- 3. frameNumberForPosition:序号 ≠ 帧号 ---
    //
    // 关键场景:**丢过帧之后**。缓存满了会丢最早的帧,
    // 于是剩下的帧号是 1,2,3,... 这时"第 1 个"对应真实帧 1,不是 0。
    // 拿 pos-1 当帧号用,平时(没丢帧)看不出问题,炸了才错。
    QList<int> frames{0, 1, 2};
    check(w7::frameNumberForPosition(frames, 1) == 0,
          "没丢帧时:序号 1 -> 真实帧 0");
    check(w7::frameNumberForPosition(frames, 3) == 2,
          "没丢帧时:序号 3 -> 真实帧 2");

    // 丢过帧:真实帧号是 7,8,9
    QList<int> gapped{7, 8, 9};
    check(w7::frameNumberForPosition(gapped, 1) == 7,
          "丢过帧时:序号 1 -> 真实帧 7(不是 0 —— 帧 0 已经被丢掉了)");
    check(w7::frameNumberForPosition(gapped, 3) == 9,
          "丢过帧时:序号 3 -> 真实帧 9(不是 2)");

    // 越界
    check(w7::frameNumberForPosition(gapped, 0) == -1,
          "序号 0 -> 越界(-1),不猜");
    check(w7::frameNumberForPosition(gapped, -3) == -1,
          "负序号 -> 越界(-1),不猜");
    check(w7::frameNumberForPosition(gapped, 4) == -1,
          "序号超出缓存帧数 -> 越界(-1),不猜");
    check(w7::frameNumberForPosition(QList<int>{}, 1) == -1,
          "没有帧 -> 越界(-1),不猜");

    // --- 4. 空输入不能崩 ---
    // 调用点在帧循环里,每一帧都要走一遍 —— 这里抛异常就会带崩整次 Run。
    int dropped = w7::recordFrameResult(nullptr, node, 0, QJsonObject{}, QString());
    check(dropped == 0, "cache 为空时不崩、返回 0");
    dropped = w7::recordFrameResult(&cache, QString(), 0, QJsonObject{}, QString());
    check(dropped == 0, "nodeId 为空时不崩、返回 0");
}

// ------------------------------------------------------------
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "=== W7 Run 策略 回归测试 ===";

    test_clamp_frame_count();
    test_ui_pump_throttle();
    test_run_state_machine();
    test_frame_loop_honours_policy();
    test_log_document_is_bounded();
    test_log_cost_stays_flat();

    test_step_scope_includes_target();
    test_step_scope_rejects_unknown_target();
    test_step_scope_has_all_upstream();
    test_step_scope_diamond_graph();
    test_step_scope_description();

    test_artifact_path_separates_node_and_frame();
    test_artifact_path_falls_back_without_uuid();
    test_artifact_keep_window();

    test_cache_keeps_every_frame();
    test_cache_reports_missing_image();
    test_primary_image_path_priority();
    test_cache_frame_cap_reports();
    test_frame_selector_positions();
    test_frame_record_behaviour();
    test_mainwindow_records_every_frame();
    test_mainwindow_frame_selector_maps_through_framesOf();

    qInfo().noquote() << QString("\n========================================");
    qInfo().noquote() << QString("pass=%1  fail=%2").arg(g_pass).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}
