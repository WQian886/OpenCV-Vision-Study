#pragma once
// ============================================================
// 结果面板「显示哪张图、上面画什么」—— **一份规则**
// ============================================================
//
// ⚠ 为什么要有这个文件:
//
//   这件事以前有**两份**实现,而且作者**自己知道**:
//
//     · step_frame_cache.h :: stepPrimaryImagePath(out)
//         优先级:mask_path -> marked_path -> image_path
//         它决定的是「这一步的**主图**是哪个」,
//         于是「图还在不在」这个判据查的就是它。
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
// ============================================================
// 它当时撒谎的那个现场(实测,不是推演)
// ============================================================
//
//   pcb_defect_judge 的输出里**根本没有 image_path**:
//     marked_path / defects / defect_count / result / metrics / ...
//
//   所以现在的显示逻辑会这样走:
//     分支 3(有 result=PASS/FAIL)-> imgPath = out["image_path"] = 空
//                              -> imgPath = fallbackPath = **工具栏那张静态图**
//     -> 显示:用户手填的那张图 + PASS/FAIL 大字
//
//   而「图还在不在」查的是 marked_path(逐帧的缺陷标注图)。
//   两个说的**不是同一张图**。后果:
//
//     · 多帧流水线里,工具栏那张静态图和第 7 帧**根本不是同一张**;
//     · 缺陷标注图(marked_path)明明在,却**一张都没显示**;
//     · 判据说「图在」(marked_path 存在),面板显示的却是另一张图。
//
//   而 mainwindow.cpp 里那段「不撒谎」的最后一道闸,**自己写下了这个坑**:
//
//     「分支 3/4 的 fallback 是工具栏那张原图 —— 用户手填的静态图,
//       和任何一帧都没有关系。拿它配这一帧的数字,屏幕上是一张正常图,
//       数字也正常,唯独**对应关系是假的**。」
//
//   它点名了分支 3,但代码里那道闸只挡了 `imgState == Dropped`,
//   挡不住「拿 fallback 冒充这一帧」。
//
// ============================================================
// 怎么收
// ============================================================
//
//   **基底图**和**叠加**分开定:
//
//     基底图 = 这张图的来源,按优先级取**第一个加载得到的**
//              marked_path -> mask_path -> image_path -> fallbackPath
//     叠加   = 画在基底图上的东西
//              result=PASS/FAIL -> 大字 + description
//              position_x+template_w -> 框 + score
//
//   两份实现都改成调这一份,于是:
//     · 「图还在不在」的判据和「显示哪张」**必然一致**
//       (同一个候选列表、同一套存在性判定);
//     · 优先级从**代码顺序**变成**显式可测的规则**。
//
//   ⚠ 而且**取哪张**也得只有一份(pickBaseImage)。
//     第一版只把「候选列表」收成一份,「从候选里挑哪个」还是各写一遍:
//       判据侧只看第一候选,面板侧取第一个**存在的**。
//     两份靠代码顺序对齐 —— 病没治好,只是换了个地方长。
//     这是本项目栽过**八遍**的「抄副本」的**第九个胚芽**。
//
//   ⚠ 纯函数、头文件内联 —— 没有状态、没有副作用,
//     单元测试可以不带 MainWindow、不带 QWidget 直接调它。
//     存在性判定通过回调传进来(照 step_frame_cache.h 的 ExistsFn 套路),
//     所以单测可以传假的,不用碰磁盘。
//
// ============================================================
// ⚠ 有意改掉的行为(不是 bugfix,是修正)
// ============================================================
//
//   pcb_inspect / pcb_defect_judge / dl_infer 的回看,
//   以前显示的是**原图**(或工具栏那张静态图),
//   现在显示的是**它们自己产出的标注图**。
//
//   理由:那才是「这一步干了什么」的证据。
//   缺陷标注图上有缺陷位置,原图上没有 ——
//   以前用户回看「PCB 缺陷判定」,**看不到缺陷在哪**。
//
//   而 marked_path 是中间产物,会被下一轮覆盖 ——
//   覆盖后判据会正确地说「图没了」,
//   而**不是**继续显示一张对不上的静态图。
//
//   这正是那段注释想要的:
//   「宁可空着,并把原因写出来。」
//
// ============================================================

#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace w7 {

// ------------------------------------------------------------
// 存在性谓词
// ------------------------------------------------------------
//
// ⚠ 2026-10-25 从 step_frame_cache.h **搬**过来的。
//   以前它只服务「图还在不在」这个判据;现在它也服务「取哪张图」,
//   两边必须用**同一个**口径 —— 不然「判据说有、面板没有」又回来了。
//
//   生产传 &w7::fileExists,单测传假的(照旧:不碰磁盘才能造出
//   「文件已被覆盖」那个世界)。
using ExistsFn = std::function<bool(const QString&)>;

inline bool fileExists(const QString& path) {
    if (path.isEmpty()) return false;
    return QFile::exists(path);
}

// ------------------------------------------------------------
// 基底图的来源
// ------------------------------------------------------------
//
// ⚠ 顺序**不能换** —— 它就是「这一步给用户看哪张图」的答案。
//   判据是「哪张图最能说明这一步干了什么」:
//     Marked   人工标注的产出(元件位置 / 缺陷位置)
//     Mask     分割产出
//     Source   原图 / 中间产物
//     Fallback 工具栏那张(和这一帧**可能无关** —— 最后的兜底)
enum class ImageSource {
    Marked,
    Mask,
    Source,
    Fallback,
};

// ------------------------------------------------------------
// 一个候选:从哪儿取图
// ------------------------------------------------------------
struct ImageCandidate {
    ImageSource source = ImageSource::Source;
    QString     path;          // 要加载的文件;空 = 这一项没有
    bool        isFallback = false;  // path 来自 fallbackPath —— 和这一帧可能无关
};

// ------------------------------------------------------------
// 叠加在基底图上的东西
// ------------------------------------------------------------
//
// ⚠ 大字**优先于**框:结果面板首先回答「判没判废」,其次才是「为什么」。
//   (这也是以前代码的实际行为 —— 大字那条无条件执行,会盖掉框。)
enum class RenderOverlay {
    None,         // 不叠加
    Verdict,      // PASS/FAIL 大字 + description
    TemplateBox,  // 模板框 + score
};

// ------------------------------------------------------------
// 显示计划
// ------------------------------------------------------------
struct RenderPlan {
    QVector<ImageCandidate> candidates;  // 按优先级排;空 = 这一步不产图
    RenderOverlay overlay = RenderOverlay::None;

    QString verdict;       // "PASS" / "FAIL"(overlay==Verdict 时有效)
    QString description;   // 大字下面那行
    int    boxX = 0;       // 模板框位置和大小(overlay==TemplateBox 时有效)
    int    boxY = 0;
    int    boxW = 0;
    int    boxH = 0;
    double score = 0.0;    // 框旁边那个 score=%.3f

    bool hasError = false; // 输出里带 error —— 结果图没有意义,直接把错误摆出来
    QString errorText;
};

// ------------------------------------------------------------
// 候选列表
// ------------------------------------------------------------
//
// out         这一步的输出
// fallbackPath 工具栏那张图(用户手填的静态图)
//
// ⚠ 这里**不碰磁盘** —— 「哪个文件真的在」由调用方用回调回答。
//   所以这一个函数是纯的,单测能直接调。
// ============================================================
inline QVector<ImageCandidate> resultImageCandidates(const QJsonObject& out,
                                                      const QString& fallbackPath) {
    // ⚠ 顺序就是「这一步给用户看哪张图」的答案 —— **不能换**。
    //
    //   这里用一张**表**写出来,而不是三段 if ——
    //   因为顺序写成代码,就又变成了「藏在代码顺序里的规则」:
    //   读的人得逐行读下去才知道谁优先,
    //   而改的人只要动一行就能悄悄改掉顺序。
    //   写成表,顺序是**数据**:一眼看得见,也能被一条变异单独打乱。
    struct KeySource { const char* key; ImageSource source; };
    static const KeySource kTable[] = {
        { "marked_path", ImageSource::Marked  },
        { "mask_path",   ImageSource::Mask    },
        { "image_path",  ImageSource::Source  },
    };

    QVector<ImageCandidate> c;
    for (const KeySource& ks : kTable) {
        const QString p = out.value(QLatin1String(ks.key)).toString();
        if (!p.isEmpty())
            c.append(ImageCandidate{ks.source, p, false});
    }

    // 工具栏那张 —— **最后的兜底**
    //
    // ⚠ 它和任何一帧都没有关系,只在前三个都没有时才用。
    //   以前 pcb_defect_judge 会直接落到这里(它没有 image_path),
    //   于是回看「缺陷判定」看到的是用户手填的静态图 + PASS/FAIL 大字 ——
    //   **对应关系是假的**,而判据查的却是 marked_path。
    if (!fallbackPath.isEmpty())
        c.append(ImageCandidate{ImageSource::Fallback, fallbackPath, true});

    return c;
}

// ------------------------------------------------------------
// 取基底图 —— **只有这一份**
// ============================================================
//
// ⚠ 规则:**只认第一个候选**。
//   它在,基底图就是它;它不在,就是「没有图」。
//   **不降级** —— 理由和以前一样:低一级的图可能早被下一轮覆盖了,
//   而用户会以为那就是这一帧的图。
//
// ⚠ 2026-10-25:这一份以前**不存在**。
//   「取哪张图」当时有两份实现:
//     · step_frame_cache.h :: imageState()  只看**第一**候选(不降级)
//     · mainwindow.cpp     :: 候选循环      取第一个**存在的**(降级)
//   两份靠「Dropped 那道闸在候选循环**之前**」这个**代码顺序**对齐 ——
//   而「藏在代码顺序里的规则」正是这个项目认定的病。
//   (优先级从代码顺序变成显式规则之后,**它自己**又长出了第三份。)
//
//   面板那条降级分支还顺手制造了一次撒谎:
//   「最终结果」那次调用**没有判据**(imgState 默认 Available),
//   于是判定节点的 marked_path 不在时,面板会退到工具栏那张静态图 ——
//   **一张和这一帧无关的图,配着这一帧的判定**。
//   那个文件开头自己写着:「唯独**对应关系是假的**。」
//
//   现在两边调**同一个**函数,所以
//   「判据说图在 ⟹ 面板显示的就是它」
//   不是靠人工对齐,是**同一份代码算出来的**。
//
// ⚠ 返回的指针指向 cands 内部 —— cands 改动之后别再用它。
// ============================================================
inline const ImageCandidate* pickBaseImage(const QVector<ImageCandidate>& cands,
                                           const ExistsFn& exists) {
    if (cands.isEmpty()) return nullptr;
    const ImageCandidate& first = cands.first();
    if (!exists(first.path)) return nullptr;
    return &first;
}

// ------------------------------------------------------------
// 叠加
// ------------------------------------------------------------
//
// ⚠ 顺序不能换:有判定就先说判定。
//   两者都有(理论上可能)的时候大字赢 —— 和以前的行为一致。
// ============================================================
inline RenderOverlay decideOverlay(const QJsonObject& out,
                                   QString* verdict,
                                   QString* description) {
    const QString v = out.value(QStringLiteral("result")).toString();
    if (v == QStringLiteral("PASS") || v == QStringLiteral("FAIL")) {
        if (verdict)    *verdict = v;
        if (description) *description = out.value(QStringLiteral("description")).toString();
        return RenderOverlay::Verdict;
    }
    if (out.contains(QStringLiteral("position_x")) &&
        out.contains(QStringLiteral("template_w"))) {
        return RenderOverlay::TemplateBox;
    }
    return RenderOverlay::None;
}

// ------------------------------------------------------------
// 整个计划
// ============================================================
inline RenderPlan planResultRender(const QJsonObject& out,
                                   const QString& fallbackPath) {
    RenderPlan p;

    // ⚠ 输出里带 error 的,**结果图没有意义**。
    //   (以前这种会掉到最后的 fallback 分支,显示一张原图,
    //    看上去「图出来了、挺正常」,但其实这一步压根没跑成。)
    if (out.contains(QStringLiteral("error"))) {
        p.hasError = true;
        p.errorText = out.value(QStringLiteral("error")).toString();
    }

    p.candidates = resultImageCandidates(out, fallbackPath);
    p.overlay = decideOverlay(out, &p.verdict, &p.description);

    if (p.overlay == RenderOverlay::TemplateBox) {
        p.boxX = out.value(QStringLiteral("position_x")).toInt();
        p.boxY = out.value(QStringLiteral("position_y")).toInt();
        p.boxW = out.value(QStringLiteral("template_w")).toInt();
        p.boxH = out.value(QStringLiteral("template_h")).toInt();
        p.score = out.value(QStringLiteral("score")).toDouble();
    }
    return p;
}

} // namespace w7
