#include "headless_cli.h"
#include "headless_runner.h"
#include "frame_verdict.h"   // batchVerdictName(判定口径,**和主程序共用**)

#include "operators/operator_base.h"
#include "operators/operator_factory.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextStream>

#include <cstdio>
#include <memory>

namespace w7 {

namespace {

// ============================================================
// 真算子的三个回调
// ============================================================
//
// ⚠ 这三个回调要 OpenCV(operator_base.h 拖 opencv2),
//   所以它们住在 .cpp 里 —— 头文件保持纯 QtCore,能进单元测试。
//
// ⚠ 每次都**新建**一个算子实例:
//   算子是有状态的(比如图像源缓存了当前帧),
//   共用一个实例会让第二个节点看到第一个节点的状态 ——
//   那是"抄副本"的一种:一份状态被两个节点共用。
//   新建完立刻扔(套 unique_ptr,漏 delete 的写法不可能发生)。

OpDefaults realFactory(const QString& name) {
    std::unique_ptr<OperatorBase> op(createOperatorByName(name));
    if (!op) return {};
    return {true, op->displayName(), op->category()};
}

QJsonObject realDefaults(const QString& name) {
    std::unique_ptr<OperatorBase> op(createOperatorByName(name));
    return op ? op->defaultParams() : QJsonObject();
}

bool realRunner(const QString& name, const QJsonObject& params,
                const QJsonObject& inputs, QJsonObject* out, QString* err) {
    std::unique_ptr<OperatorBase> op(createOperatorByName(name));
    if (!op) {
        *err = QStringLiteral("算子 %1 未注册").arg(name);
        return false;
    }
    *out = op->run(params, inputs);
    return true;
}

} // namespace

// ============================================================
// runHeadlessCli
// ============================================================
int runHeadlessCli(const CliOptions& opt) {
    QTextStream out(stdout);
    QTextStream err(stderr);

    // ---- 读文件 ----
    QFile f(opt.runFile);
    if (!f.open(QIODevice::ReadOnly)) {
        err << QStringLiteral("[Run] ❌ 打不开流水线文件: %1 (%2)\n")
               .arg(opt.runFile, f.errorString());
        return 2;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    // ---- 解析 JSON ----
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError) {
        err << QStringLiteral("[Run] ❌ JSON 解析失败: %1 (offset %2)\n")
               .arg(perr.errorString()).arg(perr.offset);
        return 2;
    }

    // ---- 跑 ----
    HeadlessOptions ho;
    ho.imagePath = opt.imagePath;
    ho.maxFrames = opt.maxFrames;

    HeadlessResult res;
    QString runErr;
    const bool ok = runPipelineJson(doc, ho, realFactory, realDefaults, realRunner,
                                    &res, runErr);

    // 日志照打 —— headless 也要能看出"跳过了什么"
    for (const QString& l : res.logs) out << l << "\n";

    // ---- 写结果 ----
    //
    // ⚠ 就算跑失败了也要写 —— 排查的人要靠它才说得清卡在哪。
    if (!opt.outFile.isEmpty()) {
        QFile of(opt.outFile);
        if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err << QStringLiteral("[Run] ❌ 写不了结果文件: %1 (%2)\n")
                   .arg(opt.outFile, of.errorString());
            return 2;
        }
        of.write(QJsonDocument(res.toJson()).toJson(QJsonDocument::Indented));
        of.close();
    }

    if (!ok) {
        err << QStringLiteral("[Run] ❌ %1\n").arg(runErr);
        return 2;
    }

    // ---- 摘要 ----
    //
    // ⚠ 末帧判定和整批结论是**两回事**,两个都打出来:
    //   末帧判定(final_result)—— 末帧判了什么
    //   整批结论(batch_verdict)—— 这一批是什么结论(退出码按这个来)
    //
    // 以前这里只打末帧判定,而退出码也是拿末帧判定推的 ——
    // 于是"判定 PASS"配"退出码 1"这种自相矛盾的话会出现。
    out << QStringLiteral("[Run] 跑完: %1 帧,PASS %2,FAIL %3,末帧判定 %4,整批结论 %5")
           .arg(res.frameCount).arg(res.passCount).arg(res.failCount)
           .arg(res.finalResult)
           .arg(w7::batchVerdictName(res.batchVerdict));
    if (res.errorCount > 0)
        out << QStringLiteral("(有 %1 步报错)").arg(res.errorCount);
    out << "\n";

    // ---- 退出码 ----
    //
    // ⚠ 三种失败必须分开:
    //   2 = 我们**没跑起来**(用法错/文件坏/有环)
    //   3 = 跑起来了,但**流水线里有步骤报错**(工具能用,流水线坏了)
    //   1 = 跑起来了,判定是 FAIL(工具能用,流水线说没通过)
    //
    //   3 比 1 更"根本",所以先判 3:
    //   步骤报错时那个 FAIL 往往只是它的**后果**。
    //
    // ⚠⚠ 整批结论用 res.batchVerdict,**不用** res.finalResult:
    //   finalResult 是**末帧**判了什么;batchVerdict 是这一批的结论
    //   (任一帧判废 -> FAIL;任一帧跑坏 -> ERROR)。
    //
    //   以前这里拿 res.finalResult 自己推,于是:
    //     30 帧抽检,第 7 帧判废,末帧(第 30 帧)正常
    //   -> finalResult 是 "PASS" -> 退出码 0 -> CI 说"过了"
    //   而主程序那边同一批说「整批判 NG」。
    //   **两个结论,CI 那个是漏检。**
    //
    //   整批结论现在由 w7::classifyBatch 算好(和主程序共用那一份),
    //   这里只准读,不准自己再推一遍。
    if (res.batchVerdict == w7::BatchVerdict::Errored) {
        err << QStringLiteral("[Run] ❌ 有 %1 步报错(算子跑了但报错了)\n")
               .arg(res.errorCount);
        return 3;
    }
    if (res.batchVerdict == w7::BatchVerdict::Failed) return 1;
    return 0;
}

} // namespace w7
