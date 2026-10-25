// ============================================================
// W7 结果面板「显示哪张图、上面画什么」的回归测试
// ============================================================
//
// 测的是哪件事:
//
//   跑完一步之后,结果面板该给用户看**哪一张图**,
//   上面该**画什么**。
//
// 为什么这件事以前没有测试:
//
//   它有**两份**实现,而且作者**自己知道**:
//
//     · step_frame_cache.h :: stepPrimaryImagePath(out)
//         优先级:mask_path -> marked_path -> image_path
//         它决定的是「图还在不在」这个**判据**查哪个文件。
//
//     · mainwindow.cpp :: renderOutputToResultPanel(...)
//         显示顺序:mask 图 -> 模板框 -> PASS/FAIL 大字 -> 原图
//         大字那一条**无条件执行**,会盖掉前面画的。
//
//   step_frame_cache.h 里那段注释写得很直白:
//
//     「优先级必须和 renderOutputToResultPanel 的显示顺序对齐 ——
//       两份各写一份的话,加一种新输出类型就只改一处,于是
//       "图还在不在"的判据和"显示哪张"会跑偏,
//       而**跑偏的方向恰恰是那个会撒谎的方向**。」
//
//   也就是说:**两份规则,靠人工对齐,偏了就会撒谎。**
//   这是本项目栽过**七遍**的「抄副本」,第八次。
//
// 它真的撒谎了(实测,不是推演):
//
//   pcb_defect_judge 的输出里**根本没有 image_path**
//   (只有 marked_path / defects / defect_count / result / metrics)。
//   所以面板的「PASS/FAIL 大字」那条会退到 fallback ——
//   **工具栏那张用户手填的静态图**;
//   而「图还在不在」查的是 marked_path(逐帧的缺陷标注图)。
//   **两个说的不是同一张图。**
//
//   后果:
//     · 缺陷标注图明明在,却**一张都没显示过**;
//     · 多帧流水线里,工具栏那张静态图和第 7 帧不是同一张;
//     · 判据说「图在」,面板显示的却是另一张 —— 对应关系是假的。
//
// 所以现在:规则收成**一份**(result_render_plan.h),
// 「判据」和「显示」都用它 —— 这里直接测它,
// 外加一条源码守卫,守住「两边都走这一份」。
//
// ⚠ 2026-10-25 补记:第一版只收了一半,被变异逮了个正着。
//
//   · 「候选列表」收成了一份,但「从候选里挑哪个」还是两份:
//       判据侧只看**第一**候选(不降级),
//       面板侧取第一个**存在的**(降级)。
//     两份靠「Dropped 那道闸在候选循环之前」这个**代码顺序**对齐 ——
//     病没治好,只是换了个地方长。这是本项目栽过八遍的那种病的**第九个胚芽**。
//
//   · 源码守卫查的是 `src.contains("w7::planResultRender")`,
//     而 mainwindow.cpp 的**注释里**就写着这句话 ——
//     守卫被一句注释喂饱了。
//     「注释会骗人,断言不会」原来只说到了断言,
//     没想到**源码守卫也会被注释骗**。
//     所以守卫改成**先剥掉注释再查**。
//
// ============================================================

#include <QJsonObject>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVector>

#include "result_render_plan.h"
#include "source_scan.h"   // 剥注释 / 读源码(共享的,见该文件头的说明)

using namespace w7;
using namespace w7test;   // stripComments / readSource(共享的测试工具)

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

// 造一个算子输出
static QJsonObject out(const QString& imagePath,
                       const QString& maskPath   = {},
                       const QString& markedPath = {}) {
    QJsonObject o;
    if (!imagePath.isEmpty()) o["image_path"]  = imagePath;
    if (!maskPath.isEmpty())   o["mask_path"]   = maskPath;
    if (!markedPath.isEmpty()) o["marked_path"] = markedPath;
    return o;
}

// ============================================================
// 剥掉注释再查源码
// ============================================================
//
// ⚠ 2026-10-25:这是**被变异逼出来**的。
//
//   原来那条源码守卫是:
//       src.contains("w7::planResultRender")
//
//   而 mainwindow.cpp 的**注释里**就写着这句话!
//   于是变异 M154「面板改成不用共享计划」把真正的调用换掉之后,
//   守卫**照样通过** —— 它被一句注释喂饱了。
//
//   这个项目有一句老话:「注释会骗人,断言不会。」
//   原来那句只说到了断言,没想到**源码守卫也会被注释骗**。
//   于是:查源码之前**先剥掉注释**。
//
//   ⚠ 工具住在**共享的** tests/source_scan.h 里 ——
//     第二个套件也要用它的时候,抄一份就是**第十一份抄副本**。
//
//   ⚠ 字符串里的 // 和 /* 不算注释 —— 剥错了同样会骗人。
// ============================================================

// ============================================================
// 用例 1:候选列表的优先级
//
// ⚠ 顺序就是「这一步给用户看哪张图」的答案,不能换。
// ============================================================
static void test_candidate_priority() {
    beginCase("1. 候选列表:marked -> mask -> source -> fallback");

    // ---- 全部都有:marked 排第一 ----
    {
        const QVector<ImageCandidate> c = resultImageCandidates(
            out("images/raw.png", "images/mask.png", "images/marked.png"),
            QStringLiteral("images/toolbar.png"));
        check(c.size() == 4,
              QString("四个候选(标注/分割/原图/兜底),实际 %1").arg(c.size()));
        if (c.size() == 4) {
            check(c.at(0).source == ImageSource::Marked,
                  "第一是标注图 —— 它最能说明这一步干了什么");
            check(c.at(1).source == ImageSource::Mask,   "第二是分割图");
            check(c.at(2).source == ImageSource::Source, "第三是原图");
            check(c.at(3).source == ImageSource::Fallback,"第四是工具栏兜底");
            check(!c.at(0).isFallback && !c.at(1).isFallback
                  && !c.at(2).isFallback,
                  "前三个都不是兜底");
            check(c.at(3).isFallback,
                  "兜底那项标了 isFallback —— 它和这一帧可能无关");
        }
    }

    // ---- 只有 mask + image ----
    {
        const QVector<ImageCandidate> c =
            resultImageCandidates(out("images/raw.png", "images/mask.png"), QString());
        check(c.size() == 2, QString("两个候选,实际 %1").arg(c.size()));
        if (c.size() == 2) {
            check(c.at(0).source == ImageSource::Mask,   "mask 排在原图前面");
            check(c.at(1).source == ImageSource::Source, "然后才是原图");
        }
    }

    // ---- 只有 image ----
    {
        const QVector<ImageCandidate> c =
            resultImageCandidates(out("images/raw.png"), QString());
        check(c.size() == 1 && c.at(0).source == ImageSource::Source,
              "只有原图时用它");
    }

    // ---- 只有 marked(PCB / DL 那几个) ----
    {
        const QVector<ImageCandidate> c =
            resultImageCandidates(out(QString(), QString(), "images/marked.png"),
                                  QString());
        check(c.size() == 1 && c.at(0).source == ImageSource::Marked,
              "只有标注图时用它");
    }

    // ---- 什么都没有 + 没有兜底 ----
    {
        const QVector<ImageCandidate> c =
            resultImageCandidates(QJsonObject(), QString());
        check(c.isEmpty(), "什么都没有 -> 候选为空(= 这一步不产图)");
    }

    // ---- 只有兜底(纯数值算子) ----
    {
        const QVector<ImageCandidate> c =
            resultImageCandidates(QJsonObject(), QStringLiteral("images/toolbar.png"));
        check(c.size() == 1 && c.at(0).source == ImageSource::Fallback,
              "什么都没有时才有兜底 —— 工具栏那张静态图");
    }

    // ---- 空路径不算候选 ----
    {
        QJsonObject o;
        o["mask_path"]   = "";    // 字段在,但值是空
        o["marked_path"] = "";
        o["image_path"]  = "";
        const QVector<ImageCandidate> c = resultImageCandidates(o, QString());
        check(c.isEmpty(), "字段在但值是空 -> 仍然算没有");
    }
}

// ============================================================
// 用例 2:叠加 —— 画什么
// ============================================================
static void test_overlay() {
    beginCase("2. 叠加:大字优先于框");

    // ---- PASS / FAIL -> 大字 ----
    {
        QJsonObject o = out("images/raw.png");
        o["result"] = "PASS";
        o["description"] = "没有缺陷";
        QString verdict, desc;
        const RenderOverlay ov = decideOverlay(o, &verdict, &desc);
        check(ov == RenderOverlay::Verdict, "有 PASS -> 大字");
        check(verdict == "PASS", QString("判定串带出来: %1").arg(verdict));
        check(desc == "没有缺陷", QString("描述带出来: %1").arg(desc));
    }
    {
        QJsonObject o = out("images/raw.png");
        o["result"] = "FAIL";
        const RenderOverlay ov = decideOverlay(o, nullptr, nullptr);
        check(ov == RenderOverlay::Verdict, "有 FAIL -> 也是大字");
    }

    // ---- 两者都有:大字赢 ----
    //
    // ⚠ 这条以前**从来没被测过**。
    //   本用例的标题一直写着「2. 叠加:大字优先于框」,
    //   但前面那几条断言里,result 和 position_x **从来没同时出现** ——
    //   于是「优先」这两个字从来没被验证过。
    //   变异 M148 把它改成「框赢过大字」,整套测试**一条没红**。
    //
    //   规则写在注释里、用例名字里,但没人在**同一个输出**上
    //   让两种叠加**打架** —— 这是「零违规不等于规则在干活」的又一例。
    {
        QJsonObject o = out("images/raw.png");
        o["result"]      = "FAIL";
        o["description"] = "有缺陷";
        o["position_x"]  = 12;
        o["position_y"]  = 34;
        o["template_w"]  = 56;
        o["template_h"]  = 78;
        o["score"]       = 0.87;
        QString verdict, desc;
        const RenderOverlay ov = decideOverlay(o, &verdict, &desc);
        check(ov == RenderOverlay::Verdict,
              "result 和框**同时**有 -> 大字赢(结果面板先回答判没判废)");
        check(verdict == "FAIL", "判定串还是带出来了");
        check(desc == "有缺陷", "描述还是带出来了");

        const RenderPlan p = planResultRender(o, QString());
        check(p.overlay == RenderOverlay::Verdict, "计划里也是大字");
        check(p.boxX == 0 && p.boxY == 0 && p.boxW == 0 && p.boxH == 0,
              "框的坐标**不**被读出来(大字赢的时候框不算)");
    }

    // ---- 模板框 ----
    {
        QJsonObject o = out("images/raw.png");
        o["position_x"] = 12;
        o["position_y"] = 34;
        o["template_w"] = 56;
        o["template_h"] = 78;
        o["score"] = 0.87;
        const RenderOverlay ov = decideOverlay(o, nullptr, nullptr);
        check(ov == RenderOverlay::TemplateBox, "有位置和模板尺寸 -> 框");
    }

    // ---- 都没有 ----
    {
        const RenderOverlay ov = decideOverlay(out("images/raw.png"), nullptr, nullptr);
        check(ov == RenderOverlay::None, "什么都没有 -> 不叠加");
    }

    // ---- result 存在但不是 PASS/FAIL ----
    {
        QJsonObject o = out("images/raw.png");
        o["result"] = "UNKNOWN";
        const RenderOverlay ov = decideOverlay(o, nullptr, nullptr);
        check(ov == RenderOverlay::None, "result 不是 PASS/FAIL -> 不叠加");
    }

    // ---- 框的坐标和 score 读对 ----
    {
        QJsonObject o = out("images/raw.png");
        o["position_x"] = 12;
        o["position_y"] = 34;
        o["template_w"] = 56;
        o["template_h"] = 78;
        o["score"] = 0.87;
        const RenderPlan p = planResultRender(o, QString());
        check(p.overlay == RenderOverlay::TemplateBox, "计划里也是框");
        check(p.boxX == 12 && p.boxY == 34 && p.boxW == 56 && p.boxH == 78,
              QString("框的坐标尺寸读对: %1,%2 %3x%4")
                  .arg(p.boxX).arg(p.boxY).arg(p.boxW).arg(p.boxH));
        check(qAbs(p.score - 0.87) < 1e-9, "score 读对");
    }
}

// ============================================================
// 用例 3:整个计划 —— **本轮修的那个洞**
//
// pcb_defect_judge 的输出形状:
//   没有 image_path,只有 marked_path / result / defect_count ...
//
// 修之前:面板会退到 fallback(工具栏那张用户手填的静态图),
//         而「图还在不在」查的是 marked_path —— 两个说的不是同一张图。
// ============================================================
static void test_plan_for_defect_judge() {
    beginCase("3. 缺陷判定这一步:显示的必须是它自己的标注图");

    QJsonObject judge;
    judge["marked_path"]  = "images/w7_pcb_defect_judge_f0007.png";
    judge["defect_count"] = 3;
    judge["result"]       = "FAIL";
    judge["description"]  = "发现 3 处缺陷";

    const RenderPlan p = planResultRender(judge, QStringLiteral("images/toolbar.png"));

    // ---- ⚠ 本轮的核心断言 ----
    check(!p.candidates.isEmpty(), "有候选");
    if (!p.candidates.isEmpty()) {
        check(p.candidates.first().source == ImageSource::Marked,
              "第一候选是**它自己的标注图**(不是工具栏那张静态图)");
        check(p.candidates.first().path == QStringLiteral("images/w7_pcb_defect_judge_f0007.png"),
              "第一候选的路径就是 marked_path");
    }

    // ---- 兜底还在,但排在最后 ----
    bool fallbackLast = true;
    int  fallbackAt  = -1;
    for (int i = 0; i < p.candidates.size(); ++i) {
        if (p.candidates.at(i).source == ImageSource::Fallback) {
            fallbackAt = i;
            if (i != p.candidates.size() - 1) fallbackLast = false;
        }
    }
    check(fallbackAt >= 0, "兜底候选还在(纯数值步骤还得靠它)");
    check(fallbackLast, "兜底排在**最后** —— 它和这一帧可能无关");

    // ---- 叠加是大字:结果面板首先回答"判没判废" ----
    check(p.overlay == RenderOverlay::Verdict, "叠加是大字");
    check(p.verdict == "FAIL", QString("判定是 FAIL: %1").arg(p.verdict));

    // ---- PCB 检查那一步:有标注图但没有 result ----
    {
        QJsonObject insp;
        insp["marked_path"] = "images/w7_pcb_inspect_f0007.png";
        insp["count"]       = 42;
        const RenderPlan q = planResultRender(insp, QStringLiteral("images/toolbar.png"));
        check(!q.candidates.isEmpty()
              && q.candidates.first().source == ImageSource::Marked,
              "PCB 检查那一步:第一候选也是标注图");
        check(q.overlay == RenderOverlay::None, "它没有判定 -> 不叠加");
    }
}

// ============================================================
// 用例 4:错误优先 —— 有 error 的步骤不配图
// ============================================================
static void test_error_wins() {
    beginCase("4. 输出带 error:结果图没有意义");

    QJsonObject o = out("images/raw.png");
    o["error"] = "读不到配方文件";
    const RenderPlan p = planResultRender(o, QStringLiteral("images/toolbar.png"));
    check(p.hasError, "hasError 置上了");
    check(p.errorText == "读不到配方文件",
          QString("错误原文带出来: %1").arg(p.errorText));
}

// ============================================================
// 用例 5:源码守卫 —— 守住「只有这一份」
//
// ⚠ 2026-10-25:守卫改成**先剥注释再查**。
//   原来查的是原文,而原文的**注释里**就写着守卫要找的那句话 ——
//   于是守卫被一句注释喂饱:变异 M154「面板改成不用共享计划」
//   把真正的调用换掉之后,守卫**照样通过**。
//
//   「注释会骗人,断言不会」—— 原来那句只说到了断言,
//   没想到**源码守卫也会被注释骗**。
// ============================================================
static void test_only_one_copy() {
    beginCase("5. 源码守卫:规则只有一份");

    const QString base = QString(W7_SOURCE_DIR);

    // ---- 剥注释这件事本身要能被测 ----
    {
        const QString sample = QStringLiteral(
            "int a = 1; // w7::planResultRender\n"
            "/* w7::planResultRender */\n"
            "const char* s = \"// not a comment\"; // w7::pickBaseImage\n");
        const QString stripped = stripComments(sample);
        check(!stripped.contains("w7::planResultRender"),
              "行注释和块注释里的那句都被剥掉了");
        check(!stripped.contains("w7::pickBaseImage"),
              "行注释里的那句也被剥掉了");
        check(stripped.contains("// not a comment"),
              "字符串里的 // 没被当成注释(剥错了同样会骗人)");
    }

    // ---- mainwindow.cpp:必须走这一份 ----
    {
        QFile f(base + "/mainwindow.cpp");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString src  = QString::fromUtf8(f.readAll());
            const QString code = stripComments(src);   // ⚠ 只查代码
            check(code.contains("w7::planResultRender"),
                  "mainwindow.cpp 的**代码**里走的是 w7::planResultRender(...)");
            check(code.contains("w7::pickBaseImage"),
                  "mainwindow.cpp 的**代码**里走的是 w7::pickBaseImage(...)");
            check(code.contains("w7::ImageCandidate"),
                  "mainwindow.cpp 按候选列表取图");
            // ⚠ 旧的内联优先级不许再出现 —— 那是抄副本的本体。
            check(!code.contains("out.value(\"mask_path\")"),
                  "mainwindow.cpp 里没有自己写的 \"mask_path\" 优先级");
            check(!code.contains("out.contains(\"position_x\")"),
                  "mainwindow.cpp 里没有自己写的 \"position_x\" 判据");
        } else {
            check(false, "能打开 mainwindow.cpp(没传 W7_SOURCE_DIR?)");
        }
    }

    // ---- step_frame_cache.h:必须走这一份 ----
    {
        QFile f(base + "/step_frame_cache.h");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString src  = QString::fromUtf8(f.readAll());
            const QString code = stripComments(src);
            check(code.contains("w7::resultImageCandidates"),
                  "step_frame_cache.h 的存在性判据走的是同一份候选列表");
            check(code.contains("w7::pickBaseImage"),
                  "step_frame_cache.h 的判据走的是同一份取图规则");
            check(!code.contains("stepPrimaryImagePath"),
                  "step_frame_cache.h 里那份「主图优先级」已经没了");
            // ⚠ 存在性谓词只有一份 —— 重复定义 = 第二份 = 会漂。
            check(code.count("using ExistsFn") <= 1,
                  "ExistsFn 只定义了一次(谓词也跟着规则搬到一起)");
        } else {
            check(false, "能打开 step_frame_cache.h");
        }
    }
}

// ============================================================
// 用例 6:取基底图 —— **只有这一份**
//
// ⚠ 这一份以前**不存在**。
//   「从候选里挑哪个」当时有两份实现:
//     · step_frame_cache.h :: imageState()  只看**第一**候选(不降级)
//     · mainwindow.cpp     :: 候选循环      取第一个**存在的**(降级)
//   两份靠「Dropped 那道闸在候选循环之前」这个**代码顺序**对齐。
//
//   收成一份之后,「判据说图在 ⟹ 面板显示的就是它」
//   就不是靠人工对齐,是**同一份代码算出来的**。
// ============================================================
static void test_pick_base_image() {
    beginCase("6. 取基底图:只认第一个候选,不降级");

    const QJsonObject o = out("images/raw.png", "images/mask.png", "images/marked.png");
    const QVector<ImageCandidate> c =
        resultImageCandidates(o, QStringLiteral("images/toolbar.png"));
    check(c.size() == 4, QString("四个候选,实际 %1").arg(c.size()));

    // ---- 第一个在 -> 就是它 ----
    {
        const ExistsFn markedOnly = [](const QString& p) {
            return p.endsWith("marked.png");
        };
        const ImageCandidate* b = pickBaseImage(c, markedOnly);
        check(b != nullptr, "第一个候选在 -> 有基底图");
        check(b && b->source == ImageSource::Marked,
              "基底图就是第一个候选(标注图)");
    }

    // ---- 第一个不在 -> **没有**,不降级 ----
    {
        const ExistsFn rawOnly = [](const QString& p) {
            return p.endsWith("raw.png");
        };
        const ImageCandidate* b = pickBaseImage(c, rawOnly);
        check(b == nullptr,
              "第一个候选不在 -> 没有基底图(**不**拿原图顶上)");
    }

    // ---- 一个都不在 -> 没有 ----
    {
        const ExistsFn none = [](const QString&) { return false; };
        check(pickBaseImage(c, none) == nullptr, "全都不在 -> 没有基底图");
    }

    // ---- 候选为空 -> 没有 ----
    {
        const ExistsFn all = [](const QString&) { return true; };
        check(pickBaseImage(QVector<ImageCandidate>(), all) == nullptr,
              "没有候选 -> 没有基底图");
    }

    // ---- 纯数值步骤:只有兜底那一项 ----
    {
        const QVector<ImageCandidate> onlyFallback =
            resultImageCandidates(QJsonObject(), QStringLiteral("images/toolbar.png"));
        check(onlyFallback.size() == 1, "纯数值步骤只有一个候选(工具栏那张)");
        const ExistsFn all = [](const QString&) { return true; };
        const ImageCandidate* b = pickBaseImage(onlyFallback, all);
        check(b && b->source == ImageSource::Fallback,
              "纯数值步骤 -> 基底图是工具栏那张(现状如此,记在这儿)");
        const ExistsFn none = [](const QString&) { return false; };
        check(pickBaseImage(onlyFallback, none) == nullptr,
              "工具栏那张也不在 -> 没有基底图");
    }
}

int main(int argc, char* argv[]) {
    qInfo().noquote() << "W7 结果面板显示计划 回归测试";

    test_candidate_priority();
    test_overlay();
    test_plan_for_defect_judge();
    test_error_wins();
    test_only_one_copy();
    test_pick_base_image();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
