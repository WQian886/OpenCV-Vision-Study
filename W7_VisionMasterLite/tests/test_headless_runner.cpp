// ============================================================
// W7 headless runner 的回归测试
// ============================================================
//
// 测的是:JSON -> 图 -> 引擎 -> 结果 JSON 这条链。
//
// ⚠ 这一套**零 OpenCV 零 QtWidgets**:
//   算子怎么跑、算子认不认识,是**传进来**的(三个回调),
//   所以测试塞假算子就行 ——
//   建得快(几秒)、不需要显示器、CI 里跑得稳。
//
//   这不是"少测了 GUI",而是**这一层本来就不该有 GUI**:
//   引擎那一层已经证明了"执行逻辑不需要 GUI",
//   这一层继续证明"连读盘都不需要"。
//
// ============================================================

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QUuid>

#include <memory>

#include "headless_cli.h"
#include "headless_runner.h"

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

// ============================================================
// 假算子
// ============================================================

// 只认这几个名字,别的当"不认识"
static OpDefaults fakeFactory(const QString& name) {
    if (name == "src" || name == "mid" || name == "dst" || name == "judge")
        return {true, name, QStringLiteral("test")};
    return {};   // 不认识
}

// 记录用状态。
//
// ⚠ 必须放**共享**的地方:runner 收的是 std::function,
//   而 std::function **按值存** —— 传进去的 functor 会被拷一份,
//   记到副本上外面就看不见了。
//   (这个坑在引擎那一轮的 atomic 卡里记着。)
struct Rec {
    QVector<QString> opOrder;      // 算子被调用的顺序
    QVector<QJsonObject> inputs;   // 每次收到的 inputs
    QVector<QJsonObject> params;   // 每次收到的 params
    QString failOn;                // 这个算子要失败(runner 返回 false)
    QString errorOn;               // 这个算子要"报错"(runner 返回 true,但输出带 error)
    int  frameCount = 0;           // 源算子报多少帧
    QString verdict = QStringLiteral("PASS");   // 判定节点给的 result
    // 逐帧的判定(空 = 每帧都用 verdict)。
    //
    // ⚠ 要有它才能测"多帧里中间某帧判废、末帧正常"那个场景 ——
    //   而那正是这次抓到的 bug:整批结论以前只看得见末帧。
    QVector<QString> verdictsByFrame;
};

struct FakeOps {
    std::shared_ptr<Rec> rec;
    bool operator()(const QString& opName, const QJsonObject& params,
                    const QJsonObject& inputs, QJsonObject* out, QString* err) {
        rec->opOrder.append(opName);
        rec->inputs.append(inputs);
        rec->params.append(params);
        if (!rec->failOn.isEmpty() && opName == rec->failOn) {
            *err = QStringLiteral("这个算子炸了");
            return false;
        }
        QJsonObject o;
        o["op"] = opName;
        // ⚠ 算子"跑成功了"但报了个错 —— 引擎看它没崩,当成功记下。
        //   可对用户来说那就是没算成,退出码不能是 0。
        if (!rec->errorOn.isEmpty() && opName == rec->errorOn)
            o["error"] = QStringLiteral("读不到配方文件");
        if (opName == "src" && rec->frameCount > 0)
            o["frame_count"] = rec->frameCount;
        if (opName == "judge") {
            const int fi = inputs.value("frame_index").toInt(0);
            o["result"] = (fi >= 0 && fi < rec->verdictsByFrame.size())
                          ? rec->verdictsByFrame.at(fi)
                          : rec->verdict;
        }
        *out = o;
        return true;
    }
};

// 声明默认值:假算子说"我有一个默认参数"
static QJsonObject fakeDefaults(const QString& opName) {
    QJsonObject d;
    d["declared_by"] = opName;
    return d;
}

// 由标签派生出**合法**的 UUID(不带花括号,和 GUI 存盘写出来的形状一样)
//
// ⚠ 为什么要费这个劲 —— 这条注释本身就是一次教训:
//   解析层判断"这条边是不是新版格式"靠的是 QUuid::fromString();
//   而 GUI 存盘写的正是 uuid().toString(QUuid::WithoutBraces) —— 真 UUID。
//
//   测试要是随手写个 "u-src",它解析不出 UUID,于是会被当成**旧格式**
//   (两端存算子名)去按 FIFO 兜底 ——
//   测试护的就成了一个**磁盘上根本不存在的格式**。
//   那种测试是绿的,但它什么也没护住:
//   真实文件里的边接不接得上,和它一点关系都没有。
//
//   ⚠ 而且它会**骗人**:测试里"边接上了",可真实文件根本不是这么写的。
//     绿得毫无意义,比红还糟。
static QString uuidOf(const QString& label) {
    const QByteArray h = QCryptographicHash::hash(label.toUtf8(),
                                                  QCryptographicHash::Md5).toHex();
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(QString(h.mid(0, 8)), QString(h.mid(8, 4)), QString(h.mid(12, 4)),
             QString(h.mid(16, 4)), QString(h.mid(20, 12)));
}

// ============================================================
// 造 JSON
// ============================================================
//
// ⚠ mkNode / mkEdge 收的是**标签**,不是真的键 ——
//   标签会被 uuidOf() 变成合法 UUID。
//   想要"非 UUID 的键"(旧格式、坏边),那一律**手写** QJsonObject,
//   别开一个后门进来 —— 后门迟早被人当成正常路径用。
static QJsonObject mkNode(const QString& op, const QString& uuidLabel,
                          const QJsonObject& params = QJsonObject()) {
    QJsonObject n;
    n["op"]       = op;
    n["uuid"]     = uuidOf(uuidLabel);
    n["display"]  = op;
    n["category"] = QStringLiteral("test");
    n["x"]        = 0;
    n["y"]        = 0;
    n["params"]   = params;
    return n;
}

static QJsonObject mkEdge(const QString& fromLabel, const QString& toLabel) {
    QJsonObject e;
    e["from"] = uuidOf(fromLabel);
    e["to"]   = uuidOf(toLabel);
    return e;
}

static QJsonDocument mkDoc(const QJsonArray& nodes, const QJsonArray& edges,
                            const QString& imagePath = QString()) {
    QJsonObject root;
    root["version"] = QStringLiteral("1.1");
    root["name"]    = QStringLiteral("test");
    if (!imagePath.isEmpty()) root["image_path"] = imagePath;
    root["nodes"] = nodes;
    root["edges"] = edges;
    return QJsonDocument(root);
}

// 一条最简单的链:src -> judge
static QJsonDocument simpleChain() {
    return mkDoc(QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
                 QJsonArray{ mkEdge("u-src", "u-judge") });
}

// ============================================================
// 用例 1:文件本身不能用
// ============================================================
static void test_bad_input() {
    beginCase("1. 文件不是流水线:必须拒绝,而且说清为什么");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    // 根不是对象
    {
        QJsonDocument doc;
        doc.setArray(QJsonArray{1, 2, 3});
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(!ok, "根是数组 -> 拒绝跑");
        check(!err.isEmpty(),
              QString("说清为什么(实际 \"%1\")").arg(err));
        check(rec->opOrder.isEmpty(), "一步都没跑");
    }

    // 缺 nodes 数组
    {
        QJsonObject root;
        root["version"] = QStringLiteral("1.1");
        const QJsonDocument doc(root);
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(!ok, "缺 nodes 数组 -> 拒绝跑");
        check(err.contains("nodes"),
              QString("说清缺的是 nodes(实际 \"%1\")").arg(err));
        check(rec->opOrder.isEmpty(), "一步都没跑");
    }

    // 一个节点都没有
    {
        const QJsonDocument doc = mkDoc(QJsonArray{}, QJsonArray{});
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(!ok, "空流水线 -> 拒绝跑");
        check(!err.isEmpty(), QString("说清为什么(实际 \"%1\")").arg(err));
        check(rec->opOrder.isEmpty(), "一步都没跑");
    }
}

// ============================================================
// 用例 2:跳过必须报出来
// ============================================================
static void test_skipped_are_reported() {
    beginCase("2. 跳过的节点和连线:必须报出来,不许悄悄咽下去");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    // 未知算子
    {
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("不认识的算子", "u-x"),
                        mkNode("judge", "u-judge") },
            QJsonArray{ mkEdge("u-src", "u-judge") });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, "有未知算子 -> 其余的照跑");
        check(rec->opOrder.contains(QStringLiteral("src")),
              "认识的算子照跑");
        check(!rec->opOrder.contains(QStringLiteral("不认识的算子")),
              "不认识的算子**没有**被跑");
        bool reported = false;
        for (const QString& l : res.logs)
            if (l.contains(QStringLiteral("跳过未知算子"))) reported = true;
        check(reported, "未知算子被**报出来**了(静默失效比崩溃更难查)");
    }

    // 边的端点缺失
    {
        auto rec2 = std::make_shared<Rec>();
        FakeOps ops2{rec2};
        HeadlessResult res2;
        QString err2;
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
            QJsonArray{ mkEdge("u-src", "u-judge"), mkEdge("u-src", "u-不存在") });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops2, &res2, err2);
        check(ok, "有一条边的端点缺失 -> 其余的照跑");
        bool reported = false;
        for (const QString& l : res2.logs)
            if (l.contains(QStringLiteral("端点缺失"))) reported = true;
        check(reported, "端点缺失的连线被**报出来**了");
    }
}

// ============================================================
// 用例 3:有环 -> 什么都不跑
// ============================================================
static void test_cycle_refused() {
    beginCase("3. 有环:宁可什么都不跑");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-a"), mkNode("mid", "u-b"), mkNode("dst", "u-c") },
        QJsonArray{ mkEdge("u-a", "u-b"), mkEdge("u-b", "u-c"), mkEdge("u-c", "u-a") });

    const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
    check(!ok, "有环 -> 拒绝跑");
    check(err.contains(QStringLiteral("环")),
          QString("说清是环(实际 \"%1\")").arg(err));
    check(rec->opOrder.isEmpty(),
          "一步都没跑 —— 跑一半的话下游拿到的是上一帧的输出,"
          "数字看着正常,结论却是错的");
}

// ============================================================
// 用例 4:正常跑通
// ============================================================
static void test_happy_path() {
    beginCase("4. 正常跑通:顺序、传递、判定");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = simpleChain();
    const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
    check(ok, QString("跑通了(实际 \"%1\")").arg(err));
    check(rec->opOrder.size() == 2,
          QString("两个算子都跑了(实际 %1)").arg(rec->opOrder.size()));
    check(rec->opOrder.value(0) == "src" && rec->opOrder.value(1) == "judge",
          QString("按拓扑序跑(实际 %1)").arg(rec->opOrder.join(QLatin1Char(','))));
    check(res.finalResult == "PASS",
          QString("判定是 PASS(实际 \"%1\")").arg(res.finalResult));
    check(res.passCount == 1 && res.failCount == 0, "PASS 计数对");
    check(res.frameCount == 1, "跑了一帧");
    check(res.outputs.contains(uuidOf("u-src")) && res.outputs.contains(uuidOf("u-judge")),
          "每个节点的输出都在结果里(按 uuid 索引)");

    // 下游拿到的是上游的输出
    check(rec->inputs.value(1).contains("op"),
          "下游拿到的 inputs 里带着上游的输出");
    check(rec->inputs.value(1).value("op").toString() == "src",
          QString("下游拿到的是**直接上游**的输出(实际 \"%1\")")
              .arg(rec->inputs.value(1).value("op").toString()));
}

// ============================================================
// 用例 5:算子失败
// ============================================================
static void test_operator_failure() {
    beginCase("5. 算子失败:这一步没产出,但链继续往下跑");

    auto rec = std::make_shared<Rec>();
    rec->failOn = QStringLiteral("src");
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = simpleChain();
    const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
    check(ok, "中间那个算子失败 -> 整条**不算崩**");
    check(rec->opOrder.size() == 2,
          QString("失败的那步之后**继续往下跑**(实际跑了 %1 步)").arg(rec->opOrder.size()));

    bool sawFailed = false;
    QString seenErr;
    for (const HeadlessStep& s : res.steps) {
        if (s.failed) { sawFailed = true; seenErr = s.error; }
    }
    check(sawFailed, "结果里标出了那一步失败");
    check(seenErr.contains(QStringLiteral("炸了")),
          QString("失败原因带上了(实际 \"%1\")").arg(seenErr));
}

// ============================================================
// 用例 6:兜底 image_path
// ============================================================
static void test_fallback_image_path() {
    beginCase("6. 兜底 image_path:上游给了就不用它");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    // JSON 里带了图路径
    {
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
            QJsonArray{ mkEdge("u-src", "u-judge") },
            QStringLiteral("D:/imgs/from-json.png"));
        runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(rec->inputs.value(0).value("image_path").toString() == "D:/imgs/from-json.png",
              QString("用 JSON 里的图路径(实际 \"%1\")")
                  .arg(rec->inputs.value(0).value("image_path").toString()));
    }

    // 命令行给了图路径 -> 优先
    {
        auto rec2 = std::make_shared<Rec>();
        FakeOps ops2{rec2};
        HeadlessResult res2;
        QString err2;
        HeadlessOptions opt;
        opt.imagePath = QStringLiteral("D:/imgs/from-cli.png");
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
            QJsonArray{ mkEdge("u-src", "u-judge") },
            QStringLiteral("D:/imgs/from-json.png"));
        runPipelineJson(doc, opt, fakeFactory, fakeDefaults, ops2, &res2, err2);
        check(rec2->inputs.value(0).value("image_path").toString() == "D:/imgs/from-cli.png",
              QString("命令行给的图路径**优先**(实际 \"%1\")")
                  .arg(rec2->inputs.value(0).value("image_path").toString()));
    }
}

// ============================================================
// 用例 7:多帧
// ============================================================
static void test_multi_frame() {
    beginCase("7. 多帧:源报几帧就跑几帧");

    auto rec = std::make_shared<Rec>();
    rec->frameCount = 3;
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = simpleChain();
    const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
    check(ok, "多帧 -> 跑通");
    check(res.frameCount == 3,
          QString("源报 3 帧 -> 跑了 3 帧(实际 %1)").arg(res.frameCount));
    check(res.passCount == 3, "三帧都判了 PASS");
    check(rec->opOrder.size() == 6,
          QString("每帧两个算子,一共调了 6 次(实际 %1)").arg(rec->opOrder.size()));

    // 帧号要真的传下去 —— 源靠它换图
    bool sawFrame2 = false;
    for (const QJsonObject& in : rec->inputs) {
        if (in.value("frame_index").toInt(-1) == 2) sawFrame2 = true;
    }
    check(sawFrame2, "frame_index 传下去了(源靠它决定取第几张图)");
}

// ============================================================
// 用例 7b:整批结论 —— 末帧判了什么 vs 这一批是什么结论
// ============================================================
//
// ⚠ 这条是 2026-10-24 抽规则时抓到的 bug 的回归测试:
//
//   以前 CLI 的退出码是拿 res.finalResult 推的,而 finalResult 是
//   **末帧**判定。于是:
//     30 帧抽检,第 7 帧判废,末帧(第 30 帧)正常
//   -> finalResult = "PASS" -> 退出码 0 -> CI 说"过了"
//   而主程序那边同一批说「整批判 NG」。
//   **两个结论,CI 那个是漏检。**
//
//   现在整批结论由 w7::classifyBatch 算,放在 res.batchVerdict 里。
//   口径在 frame_verdict.h,**和主程序共用**。
// ============================================================
static void test_batch_verdict() {
    beginCase("7b. 整批结论:末帧正常**不代表**整批正常");

    // ---- 3 帧:第 1 帧(下标 1)判废,末帧正常 ----
    {
        auto rec = std::make_shared<Rec>();
        rec->frameCount = 3;
        rec->verdictsByFrame = {QStringLiteral("PASS"),
                                QStringLiteral("FAIL"),
                                QStringLiteral("PASS")};
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;

        const QJsonDocument doc = simpleChain();
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, "多帧 -> 跑通");
        check(res.frameCount == 3, "跑了 3 帧");
        check(res.failCount == 1,
              QString("有 1 帧判废(实际 %1)").arg(res.failCount));
        check(res.passCount == 2,
              QString("另外 2 帧判 PASS(实际 %1)").arg(res.passCount));
        // 末帧判定:末帧确实说了 PASS
        check(res.finalResult == "PASS",
              QString("末帧判定是 PASS(实际 \"%1\")").arg(res.finalResult));
        // ⚠ 整批结论:必须是 FAIL
        check(res.batchVerdict == w7::BatchVerdict::Failed,
              QString("整批结论是 FAIL(实际 %1)—— 有一帧判废就不能说整批 OK")
                  .arg(w7::batchVerdictName(res.batchVerdict)));
        // 结果 JSON 里也有
        check(res.toJson().value("batch_verdict").toString() == "FAIL",
              QString("结果 JSON 里 batch_verdict = FAIL(实际 \"%1\")")
                  .arg(res.toJson().value("batch_verdict").toString()));
    }

    // ---- 全批都 PASS -> 整批 PASS ----
    {
        auto rec = std::make_shared<Rec>();
        rec->frameCount = 3;
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        runPipelineJson(simpleChain(), {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(res.batchVerdict == w7::BatchVerdict::Passed,
              "三帧全 PASS -> 整批 PASS");
        check(res.toJson().value("batch_verdict").toString() == "PASS",
              "结果 JSON 里 batch_verdict = PASS");
    }

    // ---- 有一步报错 -> 整批 ERROR(不是 FAIL,更不是 PASS) ----
    {
        auto rec = std::make_shared<Rec>();
        rec->frameCount = 2;
        rec->errorOn = QStringLiteral("src");
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        runPipelineJson(simpleChain(), {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(res.errorCount == 2,
              QString("两帧各报一步错(实际 %1)").arg(res.errorCount));
        check(res.batchVerdict == w7::BatchVerdict::Errored,
              QString("整批结论是 ERROR(实际 %1)—— 跑坏的帧不算判废,也不算通过")
                  .arg(w7::batchVerdictName(res.batchVerdict)));
    }

    // ---- 一帧判废 -> 也是 Failed(判据是"有没有帧判废",不是"有几帧") ----
    {
        auto rec = std::make_shared<Rec>();
        rec->frameCount = 1;
        rec->verdict = QStringLiteral("FAIL");
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        runPipelineJson(simpleChain(), {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(res.batchVerdict == w7::BatchVerdict::Failed,
              "一帧判废 -> 也是 FAIL(和 30 帧里 1 帧判废是同一条规则)");
    }
}

// ============================================================
// 用例 8:帧数封顶
// ============================================================
static void test_frame_cap() {
    beginCase("8. 帧数封顶:源报多少都不能照单全收");

    auto rec = std::make_shared<Rec>();
    rec->frameCount = 50000;      // 源报了一个离谱的数
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    HeadlessOptions opt;
    opt.maxFrames = 2;            // 命令行只让跑 2 帧
    const QJsonDocument doc = simpleChain();
    const bool ok = runPipelineJson(doc, opt, fakeFactory, fakeDefaults, ops, &res, err);
    check(ok, "帧数被压住 -> 跑通");
    check(res.frameCount == 2,
          QString("只跑了 2 帧(实际 %1)").arg(res.frameCount));

    bool reported = false;
    for (const QString& l : res.logs)
        if (l.contains(QStringLiteral("超过单次上限"))) reported = true;
    check(reported, "被截断了必须**报出来** —— 静默截断 = 用户以为全跑完了");
}

// ============================================================
// 用例 9:结果 JSON
// ============================================================
static void test_result_json() {
    beginCase("9. 结果 JSON:字段齐全,别让下游猜");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = simpleChain();
    runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);

    const QJsonObject j = res.toJson();
    check(j.contains("ok"), "有 ok");
    check(j.contains("final_result"), "有 final_result");
    check(j.contains("frames"), "有 frames");
    check(j.contains("pass"), "有 pass");
    check(j.contains("fail"), "有 fail");
    check(j.contains("outputs"), "有 outputs");
    check(j.contains("steps"), "有 steps");
    check(j.contains("logs"), "有 logs");
    check(j.value("ok").toBool(), "ok=true");
    check(j.value("final_result").toString() == "PASS", "final_result=PASS");
    check(j.value("steps").toArray().size() == 2,
          QString("两步都在 steps 里(实际 %1)").arg(j.value("steps").toArray().size()));

    // 每一步的字段
    const QJsonObject st = j.value("steps").toArray().at(0).toObject();
    check(st.contains("frame") && st.contains("step") && st.contains("op")
          && st.contains("uuid") && st.contains("failed") && st.contains("output"),
          "每一步的字段齐全");
    check(st.value("op").toString() == "src", "第一步是 src");
    check(st.value("frame").toInt() == 0, "第一步在第 0 帧");
}

// ============================================================
// 用例 10:守卫 —— 这条用例自己不能是空转
// ============================================================
static void test_not_vacuous() {
    beginCase("10. 守卫:这条用例不是空转");

    auto rec = std::make_shared<Rec>();
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    // 把边的方向反过来:src 不再是上游
    const QJsonDocument doc = mkDoc(
        QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
        QJsonArray{ mkEdge("u-judge", "u-src") });
    runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);

    check(rec->opOrder.value(0) == "judge",
          QString("边反了 -> 执行序也反了(实际第一个跑的是 \"%1\")")
              .arg(rec->opOrder.value(0)));
    check(rec->opOrder.value(0) != "src", "这条断言不是恒真的");
}

// ============================================================
// 用例 11:算子没崩但报错了 —— 也要算成错误
// ============================================================
//
// ⚠ 这条单独立一个用例,因为它是最容易漏的那种:
//   算子"跑成功了"(runner 返回 true),只是输出里带了个 error。
//   引擎看的是"有没有崩",于是当成功记下了。
//   可对用户来说那就是**没算成** ——
//   退出码要是 0,CI 就当它过了,那就是最贵的静默失效。
// ============================================================
static void test_step_error_counted() {
    beginCase("11. 算子没崩但报错了:也得算成错误");

    auto rec = std::make_shared<Rec>();
    rec->errorOn = QStringLiteral("src");
    FakeOps ops{rec};
    HeadlessResult res;
    QString err;

    const QJsonDocument doc = simpleChain();
    const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
    check(ok, "算子报错了,但整条**不算崩**");
    check(res.errorCount == 1,
          QString("报错的那一步被数出来了(实际 %1)").arg(res.errorCount));
    check(res.toJson().value("errors").toInt() == 1,
          "结果 JSON 里也报出来了(不然 CI 看不见)");
    // ⚠ 这一条最要紧:判定节点说 PASS,但上游报过错 ——
    //   那个 PASS 是**空头**的,不许拿它当"跑通了"。
    check(res.finalResult == "ERROR",
          QString("上游报过错 -> 判定不许说 PASS(实际 \"%1\")").arg(res.finalResult));
    check(res.passCount == 0,
          QString("报过错的那一帧既不算 PASS 也不算 FAIL(实际 pass=%1)")
              .arg(res.passCount));
    // 判定节点自己的原始输出还在 —— 没丢
    // ⚠ QJsonValue::value() 在 Qt 6 里是 private,得先 toObject()
    check(res.outputs.value(uuidOf("u-judge")).toObject().value("result").toString() == "PASS",
          "判定节点的原始结论还在 outputs 里(没被抹掉)");

    // 没报错的时候不能误报
    {
        auto rec2 = std::make_shared<Rec>();
        FakeOps ops2{rec2};
        HeadlessResult res2;
        QString err2;
        runPipelineJson(simpleChain(), {}, fakeFactory, fakeDefaults, ops2, &res2, err2);
        check(res2.errorCount == 0,
              QString("没报错的时候不误报(实际 %1)").arg(res2.errorCount));
    }
}

// ============================================================
// 用例 12:命令行参数
// ============================================================
//
// ⚠ 这一层是纯 QtCore,所以能直接测 ——
//   但它以前**根本没有测试**:命令行解析写错了,
//   表现是"参数被悄悄忽略",然后跑了个错的流水线。
// ============================================================
static void test_cli_args() {
    beginCase("12. 命令行参数:解析对了,参数才谈得上别的");

    const QString exe = QStringLiteral("w7_vm_lite.exe");

    // 什么都不给 -> 开窗口
    {
        const CliOptions o = parseCliArgs({exe});
        check(o.valid, "光杆命令 -> 合法");
        check(!o.runMode, "光杆命令 -> 开窗口模式(不是无头)");
        check(!o.help, "光杆命令 -> 不是 --help");
    }

    // --run
    {
        const CliOptions o = parseCliArgs({exe, "--run", "p.json"});
        check(o.valid, "--run -> 合法");
        check(o.runMode, "--run -> 无头模式");
        check(o.runFile == "p.json", QString("run 文件名对(实际 \"%1\")").arg(o.runFile));
    }

    // --run + --out + --image + --frames
    {
        const CliOptions o = parseCliArgs({exe, "--run", "p.json", "--out", "r.json",
                                           "--image", "D:/a.png", "--frames", "3"});
        check(o.valid, "全套参数 -> 合法");
        check(o.runFile == "p.json", "run 文件名对");
        check(o.outFile == "r.json", "out 文件名对");
        check(o.imagePath == "D:/a.png", "image 路径对");
        check(o.maxFrames == 3, QString("帧数上限对(实际 %1)").arg(o.maxFrames));
    }

    // 短选项
    {
        const CliOptions o = parseCliArgs({exe, "-r", "p.json", "-o", "r.json"});
        check(o.valid, "短选项 -> 合法");
        check(o.runFile == "p.json", "短选项的 run 文件名对");
        check(o.outFile == "r.json", "短选项的 out 文件名对");
    }

    // --help
    {
        const CliOptions o = parseCliArgs({exe, "--help"});
        check(o.valid, "--help -> 合法");
        check(o.help, "--help -> 要打用法");
        check(!o.runMode, "--help -> 不是无头模式");
    }

    // --out 没有 --run -> 报错
    {
        const CliOptions o = parseCliArgs({exe, "--out", "r.json"});
        check(!o.valid, "光有 --out -> 报错(它只对无头模式有意义)");
        check(!o.error.isEmpty(), "报错要说清为什么");
    }

    // --frames 不给数 / 给 0 / 给负数
    {
        check(!parseCliArgs({exe, "--run", "p.json", "--frames"}).valid,
              "--frames 缺值 -> 报错");
        check(!parseCliArgs({exe, "--run", "p.json", "--frames", "0"}).valid,
              "--frames 0 -> 报错(跑 0 帧等于什么都没跑)");
        check(!parseCliArgs({exe, "--run", "p.json", "--frames", "-1"}).valid,
              "--frames 负数 -> 报错");
        check(!parseCliArgs({exe, "--run", "p.json", "--frames", "abc"}).valid,
              "--frames 不是数 -> 报错");
    }

    // 不认识的选项 -> 报错(别悄悄忽略)
    {
        const CliOptions o = parseCliArgs({exe, "--bogus"});
        check(!o.valid, "不认识的选项 -> 报错");
        check(!o.error.isEmpty(), "报错要说清为什么");
    }

    // --run 缺值 -> 报错
    {
        const CliOptions o = parseCliArgs({exe, "--run"});
        check(!o.valid, "--run 缺文件名 -> 报错");
    }
}

// ============================================================
// 用例 13:旧格式的边
// ============================================================
//
// ⚠ 为什么单独立一条:
//   旧文件(v1.0 之前)边的两端存的是**算子名**,不是 uuid。
//   那时靠"按名字 FIFO 配对"来兜底 —— 同名的节点一个一个配上。
//
//   这条规则以前住在 pipeline_serialize.cpp 里,搬进纯解析层之后,
//   **如果没人测,它就只是"看起来在那儿"** ——
//   而它一坏,老工程打开就是"线接不上",还不报错。
// ============================================================
static void test_old_format_edges() {
    beginCase("13. 旧格式的边(两端存算子名):按 FIFO 兜底");

    // ---- 旧格式,各一个节点 ----
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        QJsonObject e;
        e["from"] = QStringLiteral("src");     // 算子名,不是 uuid
        e["to"]   = QStringLiteral("judge");
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
            QJsonArray{ e });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, QString("旧格式的边 -> 照样跑通(实际 \"%1\")").arg(err));
        check(rec->opOrder.size() == 2,
              QString("两个节点都跑了(实际 %1)").arg(rec->opOrder.size()));
        check(rec->opOrder.value(0) == "src",
              QString("旧格式下拓扑序也对(实际第一个跑的是 \"%1\")")
                  .arg(rec->opOrder.value(0)));
    }

    // ---- 旧格式,同名节点:一个一个配对 ----
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        QJsonObject e1; e1["from"] = QStringLiteral("src");  e1["to"] = QStringLiteral("judge");
        QJsonObject e2; e2["from"] = QStringLiteral("src");  e2["to"] = QStringLiteral("judge");
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u1"), mkNode("src", "u2"),
                        mkNode("judge", "u3"), mkNode("judge", "u4") },
            QJsonArray{ e1, e2 });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, "两个同名上游、两个同名下游 -> 跑通");
        check(rec->opOrder.size() == 4,
              QString("四个节点都跑了(实际 %1)—— FIFO 把同名节点一个一个配上了")
                  .arg(rec->opOrder.size()));
        bool reported = false;
        for (const QString& l : res.logs)
            if (l.contains(QStringLiteral("端点缺失"))) reported = true;
        check(!reported, "两条旧格式的边都配上了,不该报端点缺失");
    }

    // ---- 旧格式,但那个算子名压根不存在 ----
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;
        QJsonObject e;
        e["from"] = QStringLiteral("src");
        e["to"]   = QStringLiteral("不存在的算子");
        const QJsonDocument doc = mkDoc(
            QJsonArray{ mkNode("src", "u-src"), mkNode("judge", "u-judge") },
            QJsonArray{ e });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, "有一条旧格式的边配不上 -> 其余的照跑");
        bool reported = false;
        for (const QString& l : res.logs)
            if (l.contains(QStringLiteral("端点缺失"))) reported = true;
        check(reported, "配不上的旧格式边被**报出来**了(不然就是静默丢了一条线)");
    }
}

// ============================================================
// 用例 14:新版格式的判据 —— 「像不像 UUID」,不是「在不在节点表里」
// ============================================================
//
// ⚠ 这条规则最容易写歪,而且歪了**没人看得出来**:
//
//   「两个端点都能解析成 UUID」= 这条边是**新版格式**
//   「两个端点都在节点表里」    = 这条边**接得上**
//
//   这是两件事。一个 UUID 形状的键指向一个不存在的节点,
//   那是一条**坏边**(跳过 + 报出来),不是「旧格式」。
//
//   混起来的后果不是"接错",是**更隐蔽**:
//   坏边被当成旧格式 -> 去按算子名 FIFO 兜底 ->
//   兜底配上了另一对节点 -> 线接到了**别的地方** ->
//   工程打开看着挺正常,跑出来的结论却是错的。
//
//   ⚠ 这条判据还必须和 GUI 那边**一模一样**:
//     主程序存盘写的是 uuid().toString(QUuid::WithoutBraces) —— 真 UUID;
//     旧版 pipeline_serialize.cpp 判断的也是 QUuid::fromString() 成不成立。
//     两边判据不一样 = 同一个文件,窗口能开、无头打不开。
// ============================================================
static void test_uuid_format_criterion() {
    beginCase("14. 新版格式的判据:像不像 UUID");

    // ---- 1. 节点 uuid 是个**非法**的 UUID 串 -> 它不配当身份 ----
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;

        QJsonObject n1 = mkNode("src", "x");  n1["uuid"] = QStringLiteral("not-a-uuid");
        QJsonObject n2 = mkNode("judge", "y"); n2["uuid"] = QStringLiteral("also-not-a-uuid");
        QJsonObject e;  e["from"] = QStringLiteral("not-a-uuid");
        e["to"] = QStringLiteral("also-not-a-uuid");

        const QJsonDocument doc = mkDoc(QJsonArray{ n1, n2 }, QJsonArray{ e });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, "非法 uuid -> 其余的照跑");

        // 非法 uuid 的节点**不该**拿那个串当身份
        check(!res.outputs.contains(QStringLiteral("not-a-uuid")),
              "非法 uuid 没被当成身份(它压根不是 UUID)");

        // 边接不上,而且必须**报出来**
        bool reported = false;
        for (const QString& l : res.logs)
            if (l.contains(QStringLiteral("端点缺失"))) reported = true;
        check(reported, "接不上的边被报出来了(静默丢一条线最难查)");

        // ⚠ 最要紧的一条:这条边**不该**被当成旧格式去按算子名兜底。
        //   判据要是写成"在不在节点表里",这条边就会**悄悄接上** ——
        //   所以这里直接盯结果:线没接上。
        check(rec->inputs.value(1).value("op").toString().isEmpty(),
              "这条边没有偷偷接上别的节点(判据错了的话它会接上)");
    }

    // ---- 2. 同一个 UUID 的两种写法:要能对上 ----
    //
    // ⚠ 这就是"用 QUuid 当键、不用字符串"的理由:
    //   文件是人也会改的 —— 加花括号、改大小写,都还是同一个 UUID。
    //   按字符串比就漏了,而漏掉的代价是"这条边接不上"。
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;

        QJsonObject n1 = mkNode("src", "x");
        n1["uuid"] = QStringLiteral("{A1B2C3D4-0000-0000-0000-000000000001}");
        QJsonObject n2 = mkNode("judge", "y");
        n2["uuid"] = QStringLiteral("a1b2c3d4-0000-0000-0000-000000000002");

        // 边的两端用**小写无花括号**的写法 —— 和节点上的写法不一样
        QJsonObject e;
        e["from"] = QStringLiteral("a1b2c3d4-0000-0000-0000-000000000001");
        e["to"]   = QStringLiteral("a1b2c3d4-0000-0000-0000-000000000002");

        const QJsonDocument doc = mkDoc(QJsonArray{ n1, n2 }, QJsonArray{ e });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, QString("写法不同的同一个 UUID -> 跑通(实际 \"%1\")").arg(err));
        check(rec->inputs.value(1).value("op").toString() == "src",
              QString("边接上了(实际下游拿到的是 \"%1\")")
                  .arg(rec->inputs.value(1).value("op").toString()));
        // uuid 存的是**规范化**过的
        check(res.outputs.contains(QStringLiteral("a1b2c3d4-0000-0000-0000-000000000001")),
              "uuid 存规范化过的(和 GUI 那边一致)");
    }

    // ---- 3. 节点没有 uuid 字段 -> outputs 用 #<下标> 兜底 ----
    {
        auto rec = std::make_shared<Rec>();
        FakeOps ops{rec};
        HeadlessResult res;
        QString err;

        QJsonObject n1 = mkNode("src", "x");  n1.remove("uuid");
        QJsonObject n2 = mkNode("judge", "y"); n2.remove("uuid");
        QJsonObject e;  e["from"] = QStringLiteral("src");   // 旧格式:算子名
        e["to"] = QStringLiteral("judge");

        const QJsonDocument doc = mkDoc(QJsonArray{ n1, n2 }, QJsonArray{ e });
        const bool ok = runPipelineJson(doc, {}, fakeFactory, fakeDefaults, ops, &res, err);
        check(ok, QString("没有 uuid 的节点 -> 跑通(实际 \"%1\")").arg(err));
        check(res.outputs.contains(QStringLiteral("#0")) &&
              res.outputs.contains(QStringLiteral("#1")),
              "没有 uuid 的节点用 #<下标> 兜底(键不能空)");
        bool anyEmptyKey = false;
        for (auto it = res.outputs.constBegin(); it != res.outputs.constEnd(); ++it)
            if (it.key().isEmpty()) anyEmptyKey = true;
        check(!anyEmptyKey, "没有一个节点的键是空的(空键分不出是哪个节点)");
    }
}

// ============================================================
// 用例 15:仓库里那条流水线文件本身
// ============================================================
//
// ⚠ 为什么**数据文件**也要有测试:
//   pipelines/smoke.json 是 CI 冒烟跑的那条流水线,它是**手写**的。
//   手写的流水线最容易出的错,是「边的两端不是真 UUID」:
//     解析层分"新版/旧版格式"靠的是 QUuid::fromString(),
//     一个像 "smoke-judge" 的假键不是 UUID ->
//     被当成旧格式(两端存算子名)去兜底 -> 查不到 -> 线接不上。
//
//   后果是流水线判定 ERROR,而"文件写错了"这件事一眼看不出来:
//   CI 冒烟第一次跑就是这么红的 —— 退出码 3,日志说"端点缺失"。
//
//   这条断言把那个错挪到**单元层**:几毫秒,而且能说清是哪个文件。
//
// ⚠ 文件路径:测试工作目录是 build/test_work/headless_runner/,
//   pipelines/ 由 CMake **每次构建**时拷进去(见 CMakeLists.txt 的
//   w7_test_data)—— 改了文件不重新构建就 ctest,拿到的是**旧的**。
//   那是"陈旧数据"那个老坑,所以这里只查结构,不依赖内容的具体数字。
//
// ⚠ 这里的 factory 认识**真的**算子名(fakeFactory 不认识它们)。
static OpDefaults shippedFactory(const QString& name) {
    if (name == "pcb_inspect" || name == "pcb_defect_judge")
        return {true, name, QStringLiteral("detection")};
    return {};
}

// ============================================================
// 找仓库根
// ============================================================
//
// ⚠ 判据是 **common/ 存在**,不是"工程目录名存在":
//   CI 的构建目录是 build/ci/<工程名>/,那下面**正好**有一份
//   W6_PCB_DefectProject 和 W7_VisionMasterLite ——
//   拿工程目录名当判据,会把**构建目录当成仓库根**,
//   然后"找不到文件",报红的原因和真正的问题完全对不上。
//   (common/pcb_core/tests/test_pcb_core.cpp 栽过这个,注释还留着。)
//
//   common/ 只有仓库根有(共享核心在那儿),构建树里不可能有同名子目录。
//
// ⚠ 为什么**不**把 pipelines/ 拷进测试工作目录:
//   拷贝 = 多一份副本。改了源文件不重新构建,测试拿到的还是**旧的** ——
//   而"数据是陈的"这件事**没有任何提示**。
//   流水线文件是**源码**,不是生成的数据,所以直接读源码树那一份。
//   (本项目栽过五遍的"抄副本"坑,这里不添第六遍。)
static QString findRepoRoot() {
    QDir d = QDir::current();
    for (int i = 0; i < 8; ++i) {
        if (QFile::exists(d.filePath(QStringLiteral("common/pcb_core/pcb_core.h"))) &&
            QFile::exists(d.filePath(QStringLiteral("W7_VisionMasterLite"))) &&
            QFile::exists(d.filePath(QStringLiteral("W6_PCB_DefectProject"))))
            return d.absolutePath();
        if (!d.cdUp()) break;
    }
    return QString();
}

static void test_shipped_pipeline_file() {
    beginCase("15. 仓库里那条流水线文件:必须真的接得上");

    const QString root = findRepoRoot();
    if (root.isEmpty()) {
        check(false, "找不到仓库根(工作目录不在仓库里?)");
        return;
    }
    const QString path = root +
        QStringLiteral("/W7_VisionMasterLite/pipelines/smoke.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        check(false, QString("打不开 %1").arg(path));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    check(doc.isObject(), "smoke.json 的根是对象");

    PipelineGraph g;
    QString err;
    QStringList logs;
    const bool ok = graphFromJson(doc,
                                  [&logs](const QString& s) { logs.append(s); },
                                  shippedFactory, &g, err);
    check(ok, QString("smoke.json 解析得了(实际 \"%1\")").arg(err));
    check(g.nodes.size() == 2,
          QString("两个节点都在(实际 %1)").arg(g.nodes.size()));
    check(g.skippedNodes == 0,
          QString("没有节点被跳过(实际 %1)").arg(g.skippedNodes));
    check(g.edges.size() == 1,
          QString("一条边接上了(实际 %1 条)").arg(g.edges.size()));
    check(g.skippedEdges == 0,
          QString("没有边被跳过(实际 %1)").arg(g.skippedEdges));

    // ⚠ 最要紧的一条:边的两端必须是**真 UUID**。
    //   不是的话,这条边会被当成旧格式去按算子名兜底 ——
    //   而这里的两个算子名(pcb_inspect / pcb_defect_judge)
    //   都不等于那两个键,于是查不到,线接不上,流水线判定 ERROR。
    if (g.edges.size() == 1) {
        const GraphEdge& e = g.edges.at(0);
        check(!QUuid::fromString(e.fromKey).isNull() &&
              !QUuid::fromString(e.toKey).isNull(),
              QString("边的两端都是真 UUID(实际 \"%1\" -> \"%2\")")
                  .arg(e.fromKey, e.toKey));
    }

    // 节点自己的 uuid 也要是真 UUID —— 结果 JSON 靠它当键
    for (const GraphNode& n : g.nodes) {
        check(!QUuid::fromString(n.uuid).isNull(),
              QString("节点 %1 的 uuid 是真 UUID(实际 \"%2\")").arg(n.op, n.uuid));
    }
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 headless runner 回归测试";

    test_bad_input();
    test_skipped_are_reported();
    test_cycle_refused();
    test_happy_path();
    test_operator_failure();
    test_fallback_image_path();
    test_multi_frame();
    test_batch_verdict();
    test_frame_cap();
    test_result_json();
    test_not_vacuous();
    test_step_error_counted();
    test_cli_args();
    test_old_format_edges();
    test_uuid_format_criterion();
    test_shipped_pipeline_file();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
