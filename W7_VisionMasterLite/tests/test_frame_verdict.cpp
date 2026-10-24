// ============================================================
// W7 帧判定与整批结论的回归测试
// ============================================================
//
// 测的是哪件事:
//
//   「跑完一条流水线,结论到底是什么」——
//   一帧判了什么、整批判了什么、最后结果那个字符串该怎么说。
//
// 为什么这件事以前没有测试:
//
//   它有**两份**:mainwindow.cpp 里的 runPipeline(给人看)
//   和 headless_runner.cpp 里的 runPipelineJson(给 CI 看)。
//   mainwindow.cpp **不在任何测试目标里**(它要真窗口 + 整套算子,
//   建一次目标要好几分钟)。
//
//   2026-10-24 结项对账时发现:headless 那份有两条规则,GUI 那份没有 ——
//
//     规则 1:一帧里有步骤报错 -> 这一帧既不算 PASS 也不算 FAIL
//     规则 2:整条流水线有步骤报错 -> 最终结果强制说 ERROR
//
//   后果:同一条流水线(一个步骤报错,判定节点照样说 PASS),
//     GUI 说「🎉 最终结果: PASS」,headless 说「final_result: ERROR」。
//   **两份都说得很有底气,结论却相反。**
//   产线上判废看的是 GUI 那份。
//
// 所以现在:规则抽成**一份**(frame_verdict.h),两边都用它,
// 这里直接测它 —— 加上一条源码守卫,守住「两边都走这一份」。
//
// ============================================================

#include <QJsonObject>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QStringList>

#include "frame_verdict.h"

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

static QString verdictName(FrameVerdict v) {
    switch (v) {
    case FrameVerdict::Pass:    return QStringLiteral("Pass");
    case FrameVerdict::Fail:    return QStringLiteral("Fail");
    case FrameVerdict::Error:   return QStringLiteral("Error");
    case FrameVerdict::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("?");
}

static QString batchName(BatchVerdict v) {
    switch (v) {
    case BatchVerdict::Passed:    return QStringLiteral("Passed");
    case BatchVerdict::Failed:    return QStringLiteral("Failed");
    case BatchVerdict::Errored:   return QStringLiteral("Errored");
    case BatchVerdict::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("?");
}

// ============================================================
// 用例 1:classifyFrame —— 一帧判了什么
// ============================================================
static void test_classify_frame() {
    beginCase("1. classifyFrame:一帧的四种判定");

    check(classifyFrame("PASS", 0) == FrameVerdict::Pass, "result=PASS 且没报错 -> Pass");
    check(classifyFrame("FAIL", 0) == FrameVerdict::Fail, "result=FAIL 且没报错 -> Fail");
    check(classifyFrame("", 0) == FrameVerdict::Unknown, "没有 result 且没报错 -> Unknown");

    // ⚠ 这两条是**规则 1**:有步骤报错就不算通过
    check(classifyFrame("PASS", 1) == FrameVerdict::Error,
          QString("result=PASS 但有步骤报错 -> Error(实际 %1) —— "
                  "跑坏的帧不算通过")
              .arg(verdictName(classifyFrame("PASS", 1))));
    check(classifyFrame("FAIL", 1) == FrameVerdict::Error,
          QString("result=FAIL 但有步骤报错 -> Error(实际 %1) —— "
                  "跑坏的帧也不算不通过")
              .arg(verdictName(classifyFrame("FAIL", 1))));
    check(classifyFrame("PASS", 3) == FrameVerdict::Error,
          "报错的步数是 1 还是 3,结论一样(只要 >0 就是坏帧)");

    // ⚠ 判据的**顺序**不能换:先看报错,再看 result。
    //   顺序换了的话,上面两条就变成 Pass/Fail。
    check(classifyFrame("PASS", 0) != FrameVerdict::Error,
          "没报错时 PASS 不该被当成 Error(证明上面两条不是恒真)");

    // 大小写敏感
    check(classifyFrame("pass", 0) == FrameVerdict::Unknown,
          "\"pass\"(小写)不算判定 -> Unknown(别悄悄容错)");
    check(classifyFrame("PASS ", 0) == FrameVerdict::Unknown,
          "\"PASS \"(带空格)不算判定 -> Unknown");
}

// ============================================================
// 用例 2:FrameTally —— 逐帧累计
// ============================================================
static void test_frame_tally() {
    beginCase("2. FrameTally:逐帧累计");

    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("FAIL", 0);
        t.addFrame("PASS", 0);
        check(t.frames == 3, "跑了 3 帧");
        check(t.pass == 2 && t.fail == 1 && t.error == 0, "2 PASS / 1 FAIL / 0 坏帧");
        check(t.lastResult == "PASS", "lastResult 是末帧那个");
    }

    // ⚠ 坏帧不计入 pass/fail
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 1);   // 这一帧跑坏了
        check(t.frames == 2, "跑了 2 帧");
        check(t.pass == 1,
              QString("坏帧不算 PASS(实际 pass=%1)—— 抽检 30 帧里坏 1 帧,"
                      "末帧正常的话只看完帧就是漏检").arg(t.pass));
        check(t.fail == 0, "坏帧也不算 FAIL");
        check(t.error == 1, "坏帧被单独数出来了");
    }

    // 没判定的帧:算跑过了,但不计入任何一类
    {
        FrameTally t;
        t.addFrame("", 0);
        check(t.frames == 1, "没判定的帧也算跑过了");
        check(t.pass == 0 && t.fail == 0 && t.error == 0, "但不计入任何一类");
        check(t.lastResult.isEmpty(), "lastResult 是空的");
    }

    // ⚠ lastResult 存的是**末帧**的,不是"最后一个有判定的"
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("", 0);
        check(t.lastResult.isEmpty(),
              "末帧没判定 -> lastResult 是空的 —— "
              "不许回退到上一帧那个 PASS(回退会让用户以为末帧判过了)");
        check(t.pass == 1, "但第一帧那个 PASS 还是记着");
    }
}

// ============================================================
// 用例 3:finalResultOf —— 最后结果那个字符串
// ============================================================
static void test_final_result() {
    beginCase("3. finalResultOf:最后结果怎么说");

    {
        FrameTally t;
        t.addFrame("PASS", 0);
        check(finalResultOf(t) == "PASS", "没报错 + 末帧 PASS -> PASS");
    }
    {
        FrameTally t;
        t.addFrame("FAIL", 0);
        check(finalResultOf(t) == "FAIL", "没报错 + 末帧 FAIL -> FAIL");
    }
    {
        FrameTally t;
        t.addFrame("", 0);
        check(finalResultOf(t) == "UNKNOWN",
              "末帧没判定 -> UNKNOWN —— UNKNOWN 和 PASS 是两回事: "
              "前者是\"没判\",后者是\"判过了\"");
    }

    // ⚠⚠ 这就是**规则 2**,也是 GUI 那份缺的那条
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 1);   // 末帧有步骤报错,但判定节点还是说了 PASS
        check(finalResultOf(t) == "ERROR",
              QString("有步骤报错 -> 强制 ERROR(实际 \"%1\")—— "
                      "\"判定节点说 PASS\"不代表\"整条流水线跑成了\","
                      "那个 PASS 是**空头**的").arg(finalResultOf(t)));
    }
    {
        FrameTally t;
        t.addFrame("PASS", 1);   // 唯一一帧就报错了
        check(finalResultOf(t) == "ERROR", "只有一帧、那一帧就报错 -> 也是 ERROR");
    }
    {
        FrameTally t;
        t.addFrame("FAIL", 1);
        check(finalResultOf(t) == "ERROR",
              "报错优先于 FAIL —— 跑坏的帧连判都没判成,比判废更\"不成结论\"");
    }
}

// ============================================================
// 用例 4:classifyBatch —— 整批结论
// ============================================================
static void test_batch_verdict() {
    beginCase("4. classifyBatch:整批结论(优先级不能换)");

    // 一帧:也走同一套口径(判据是"有没有帧判废",不是"有几帧")
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        check(classifyBatch(t, false) == BatchVerdict::Passed,
              "一帧 + PASS -> Passed");
        FrameTally t2;
        t2.addFrame("FAIL", 0);
        check(classifyBatch(t2, false) == BatchVerdict::Failed,
              "一帧 + FAIL -> Failed(和 30 帧里 1 帧判废是同一条规则)");
    }

    // 全批 OK
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 0);
        check(classifyBatch(t, false) == BatchVerdict::Passed, "3 帧全 PASS -> Passed");
    }

    // 有帧判废
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("FAIL", 0);
        check(classifyBatch(t, false) == BatchVerdict::Failed, "有 FAIL -> Failed");
    }

    // ⚠⚠ **本轮抓到的那条**:早先有一帧判废,末帧正常 —— 不许说"全批 OK"
    {
        // 30 帧抽检,第 7 帧判废,末帧(第 30 帧)正常
        FrameTally t;
        for (int i = 0; i < 30; ++i)
            t.addFrame(i == 6 ? QStringLiteral("FAIL") : QStringLiteral("PASS"), 0);
        check(t.fail == 1, "有 1 帧判废");
        check(t.lastResult == "PASS", "末帧是 PASS");
        check(classifyBatch(t, false) == BatchVerdict::Failed,
              QString("第 7 帧判废 -> 整批判 NG(实际 %1)—— "
                      "末帧正常不代表整批正常,只看末帧就是抽检漏检")
                  .arg(batchName(classifyBatch(t, false))));
    }

    // ⚠ 有帧跑坏 -> **绝不能**说"全批 OK"
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 1);   // 坏帧
        check(classifyBatch(t, false) == BatchVerdict::Errored,
              QString("有坏帧 -> Errored(实际 %1)—— "
                      "\"全批 N 帧判 OK\"这句话的每一个字都得是真的")
                  .arg(batchName(classifyBatch(t, false))));
    }

    // Errored 优先于 Failed
    {
        FrameTally t;
        t.addFrame("FAIL", 0);
        t.addFrame("PASS", 1);
        check(classifyBatch(t, false) == BatchVerdict::Errored,
              "又判废又跑坏 -> Errored 优先(跑坏比判废更\"不成结论\")");
    }

    // ⚠ Cancelled 最高优先
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 0);
        check(classifyBatch(t, true) == BatchVerdict::Cancelled,
              "中途 Stop -> Cancelled —— 只跑完 2 帧,不是整批结论");
    }
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("FAIL", 0);
        check(classifyBatch(t, true) == BatchVerdict::Cancelled,
              "又中止又判废 -> Cancelled 优先");
    }
    {
        FrameTally t;
        t.addFrame("PASS", 0);
        t.addFrame("PASS", 1);
        check(classifyBatch(t, true) == BatchVerdict::Cancelled,
              "又中止又跑坏 -> Cancelled 优先");
    }

    // 英文名(JSON / 退出码用)
    check(batchVerdictName(BatchVerdict::Passed) == "PASS", "Passed -> \"PASS\"");
    check(batchVerdictName(BatchVerdict::Failed) == "FAIL", "Failed -> \"FAIL\"");
    check(batchVerdictName(BatchVerdict::Errored) == "ERROR", "Errored -> \"ERROR\"");
    check(batchVerdictName(BatchVerdict::Cancelled) == "CANCELLED",
          "Cancelled -> \"CANCELLED\"");
}

// ============================================================
// 用例 5:pickFrameResult —— 这一帧的判定输出是哪个
// ============================================================
static void test_pick_frame_result() {
    beginCase("5. pickFrameResult:取\"最后一个跑了而且带 result 的\"");

    // 只有一个带 result 的
    {
        QVector<QJsonObject> outs;
        outs.append(QJsonObject{{"count", 5}});              // 0:没有 result
        outs.append(QJsonObject{{"result", "PASS"}});        // 1:带 result
        int fi = -1;
        const QJsonObject r = pickFrameResult(outs, {0, 1}, &fi);
        check(r.value("result").toString() == "PASS", "挑中了带 result 的那个");
        check(fi == 1, "finalIndex 是最后一个跑了的(1)");
    }

    // ⚠ 多个带 result 的 -> 取**最后**一个
    {
        QVector<QJsonObject> outs;
        outs.append(QJsonObject{{"result", "PASS"}});
        outs.append(QJsonObject{{"result", "FAIL"}});
        int fi = -1;
        const QJsonObject r = pickFrameResult(outs, {0, 1}, &fi);
        check(r.value("result").toString() == "FAIL",
              QString("多个带 result 的 -> 取最后一个(实际 %1)")
                  .arg(r.value("result").toString()));
    }

    // 一个都没有 -> 退到最后一个跑了的节点的输出
    {
        QVector<QJsonObject> outs;
        outs.append(QJsonObject{{"count", 5}});
        outs.append(QJsonObject{{"count", 7}});
        int fi = -1;
        const QJsonObject r = pickFrameResult(outs, {0, 1}, &fi);
        check(r.contains("count") && !r.contains("result"),
              "没人给 result -> 退到最后一个跑了的节点的输出");
        check(r.value("count").toInt() == 7, "退到的那个是最后一个(7)");
        check(fi == 1, "finalIndex 还是最后一个跑了的");
    }

    // 一个都没跑
    {
        int fi = -1;
        const QJsonObject r = pickFrameResult({}, {}, &fi);
        check(r.isEmpty() && fi == -1, "一个节点都没跑 -> 空输出 + finalIndex=-1");
    }

    // ⚠ executed 里的下标可能越界(引擎保证有效,但这份逻辑自己保证)
    {
        QVector<QJsonObject> outs;
        outs.append(QJsonObject{{"result", "PASS"}});
        int fi = -1;
        const QJsonObject r = pickFrameResult(outs, {0, 99, -3}, &fi);
        check(r.value("result").toString() == "PASS", "越界的下标被跳过,没崩");
        check(fi == 0, "finalIndex 是最后一个**有效**的下标");
    }
}

// ============================================================
// 用例 6:源码守卫 —— 两条路都必须走这一份
// ============================================================
//
// 为什么这条是 lint 而不是行为测试:
//   mainwindow.cpp **不在任何测试目标里**,所以"主程序有没有接上
//   frame_verdict"没法用行为测。
//
//   但 lint 也能干这件事 —— 只要把判据从"禁止某种写法"
//   改成"**要求某个可测入口**"(见 run_policy 那条记录):
//   两个调用方都必须调 w7::finalResultOf / w7::classifyBatch /
//   w7::pickFrameResult,而且不许再出现自己写的计数循环。
//
//   后一条是关键:规则抽出来了,某一边却照旧自己写一遍,
//   那就等于**两份拷贝** —— 抽了个寂寞,而且两份会漂。
//   本次就已经漂过一次了(规则 1 / 规则 2 只在 headless 那边)。
// ============================================================
static void test_both_callers_use_shared_rules() {
    beginCase("6. 源码守卫:两条路都必须走 frame_verdict.h");

    const QString base = QString(W7_SOURCE_DIR);

    struct Caller { const char* file; const char* label; };
    const Caller callers[] = {
        {"mainwindow.cpp",     "主程序(GUI)"},
        {"headless_runner.cpp", "headless runner(CI)"},
    };

    for (const Caller& c : callers) {
        QFile f(base + "/" + c.file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            check(false, QString("能打开 %1 —— 打不开说明构建时没把源码目录传进来")
                         .arg(c.file));
            continue;
        }
        const QString src = QString::fromUtf8(f.readAll());

        check(src.contains("frame_verdict.h"),
              QString("%1(%2) 引入了 frame_verdict.h")
                  .arg(c.file, c.label));
        check(src.contains("w7::finalResultOf("),
              QString("%1 走的是 w7::finalResultOf(...) —— "
                      "自己再写一遍最终结果就是抄副本").arg(c.file));
        check(src.contains("w7::classifyBatch("),
              QString("%1 走的是 w7::classifyBatch(...) —— "
                      "整批结论的口径只能有一份").arg(c.file));
        check(src.contains("w7::pickFrameResult("),
              QString("%1 走的是 w7::pickFrameResult(...) —— "
                      "\"取最后一个带 result 的\"只能有一份").arg(c.file));

        // 不许再出现自己写的计数循环 —— 那正是这次漂掉的那两份
        check(!src.contains("if (r == \"PASS\")"),
              QString("%1 里没有自己写的 `if (r == \"PASS\")` 计数循环").arg(c.file));
        check(!src.contains("if (verdict == \"PASS\")"),
              QString("%1 里没有自己写的 `if (verdict == \"PASS\")` 计数循环").arg(c.file));
        check(!src.contains("if (result == \"PASS\")"),
              QString("%1 里没有自己写的 `if (result == \"PASS\")` 计数循环").arg(c.file));
    }

    // ⚠ 主程序**必须**有坏帧计数 —— 以前它压根没有这个概念
    {
        QFile f(base + "/mainwindow.cpp");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString src = QString::fromUtf8(f.readAll());
            // ⚠ 判据是 "**++**frameErrors",不是 "frameErrors" ——
            //   只查名字的话,`frameErrors += 0` 也能过(那等于不数)。
            //   变异 M129 正是这一条:它把自增改成加零,坏帧就又不数了。
            check(src.contains("++frameErrors"),
                  "mainwindow.cpp 真的在给 frameErrors **自增** —— "
                  "以前它压根没有这个概念,所以坏帧被算成了 PASS");
        }
    }

    // ⚠⚠ headless 的**退出码**不许自己推导整批结论
    {
        // 以前 headless_cli.cpp 是这么写的:
        //     if (res.finalResult == "FAIL") return 1;
        // 那是"**末帧**判了什么" —— 30 帧里第 7 帧判废、末帧正常,
        // 这行会说"通过"(退出码 0),而主程序那边说"整批判 NG"。
        // 同一批东西,两个结论 —— 而 CI 那个结论是**漏检**。
        //
        // 现在整批结论由 w7::classifyBatch 算好,放在 res.batchVerdict 里,
        // CLI 只准读它,不准自己再推一遍。
        QFile f(base + "/headless_cli.cpp");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString src = QString::fromUtf8(f.readAll());
            check(src.contains("res.batchVerdict"),
                  "headless_cli.cpp 读的是 res.batchVerdict —— "
                  "整批结论只能有一处算");
            check(!src.contains("res.finalResult == QStringLiteral(\"FAIL\")"),
                  "headless_cli.cpp 不再拿末帧判定当整批结论"
                  "(那是抽检漏检:30 帧里 1 帧判废、末帧正常,会被判成通过)");
        }
    }
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 帧判定与整批结论 回归测试";

    test_classify_frame();
    test_frame_tally();
    test_final_result();
    test_batch_verdict();
    test_pick_frame_result();
    test_both_callers_use_shared_rules();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
