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
#include <QDebug>
#include <QElapsedTimer>
#include <windows.h>

#include "run_policy.h"

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

    qInfo().noquote() << QString("\n========================================");
    qInfo().noquote() << QString("pass=%1  fail=%2").arg(g_pass).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}
