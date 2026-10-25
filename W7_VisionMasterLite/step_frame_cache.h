#pragma once
// ============================================================
// step_frame_cache.h — 每步结果的「多帧」缓存
//
// ============================================================
// 改这一层之前,每步结果只有一份,还只写最后一帧:
//
//     if (frame == totalFrames - 1 || totalFrames == 1) {
//         m_stepOutputs[n->uuid()] = out;
//     }
//
// 也就是说 30 帧里第 7 帧判 FAIL,数字只在日志里,而右键回看
// 只能看到末帧 —— 末帧恰好是 PASS 时,用户看到的是一张绿图,
// 于是"回看"这个动作反而**掩盖**了要查的那一帧。
//
// 中间产物的图已经按帧分开命名了(见 artifact_path.h),这一层
// 解决剩下两件事:
//   ① 数字从"每节点一份"变成"每节点每帧一份",全帧留
//   ② 某一帧的图被覆盖/清掉时,**明说图没了**,绝不拿别的帧顶替
//
// ============================================================
// ① 为什么数字全留、图不全留
//
// 两个数字都是实测的(基准跑完即删,方法见下),不是估的:
//
//   数字(QJsonObject):2000 帧 × 8 节点 = 16000 条,净增 **119 MB**,
//     每条 7.6 KB,put 全程 438 ms(27 us/条)。
//   图(PNG 落盘):约 27 KB/张(见 artifact_path.h),2000 帧 × 9 个
//     写文件的算子 ≈ 480 MB。
//
// 119 MB 换 480 MB,内存比磁盘便宜 —— 所以数字全留。
//
// 顺带一句:这一节我第一版写的是"约 30~60 MB",是拍脑袋估的,
// 实测跑出来 119 MB,差了 2~4 倍。估的数字写进注释里,下一个
// 照着它做决定的人就被带偏了 —— 所以现在这个数是量出来的。
//
// 所以:**数字全留,图只留最近 kStepCacheMaxFrames 帧。**
//
// (基准怎么做的:在 test_run_policy.cpp 里临时插一段 #ifdef W7_TMP_BENCH,
//  造 pcb_defect_judge 量级的输出 —— 13 个字段含一段几十字的中文
//  description,外加 12 个元件的 QJsonArray —— 灌满 16000 条,
//  前后读 GetProcessMemoryInfo 的 WorkingSetSize。跑完已删。)
//
// ============================================================
// ② 「图没了」为什么必须判据可测
//
// 最危险的不是图没了,是**假装图还在**。两条路径会静默撒谎:
//
//   · renderOutputToResultPanel 的 fallback 分支拿工具栏那张原图兜底。
//     工具栏是用户手填的一张静态图,和任何一帧都没有关系 ——
//     拿它配第 7 帧的数字,屏幕上是一张"看起来很正常"的图。
//   · 缓存里存一个 "imageAvailable" 标志。文件在运行期间被删
//     (用户清 images/、换工程、磁盘满)标志不会变,于是继续显示一张
//     早就没了的图,或者反过来。
//
// 所以判据是**每次问的时候去看磁盘**,不是一个存下来的标志:
//     imageState() 用调用方注入的 exists 谓词现场判断。
// 注入是为了能无头单测(传一个假的 exists 就能造出"文件已删"的世界),
// 生产传 &w7::fileExists。这也让"判据"和"事实"之间没有缓存层。
//
// ============================================================
// ③ 上限不是为了省内存,是为了有确定的失败方式
//
// kStepCacheMaxFrames == kMaxRunFrames,所以正常单次 Run 打不到它
// —— 它现在是个**保险丝**:哪天帧数上限被调高,内存不会无声地涨,
// 而是明确地丢最早的帧,并且调用方被要求把丢弃这件事报出来。
//
// 丢最早而不是丢最新:Run 是流式的,用户早就看过前面那些帧的日志了;
// 而且用户批准的口径是"数字全留",超限时保最近的帧是最不坏的选择。
//
// 为什么上限到了必须报而不能静默:
// 本项目栽过好几次"静默失效" —— 条件永远不成立 → 中间量变成 0 →
// 下游阈值反而通过 → 报 PASS。缓存丢帧是同一类:界面一切正常,
// 只是你要的那一帧的数字**不在了**,而你不知道。
//
// ============================================================
// ④ 这个文件的测试**测不到什么**(变异验证实测出来的,不是推测)
//
// 跑了 4 次变异,前 3 次被杀,第 4 次活了 —— 而第 4 次恰好是
// **这个改动本身要修的那个 bug**:
//
//   M4:把 mainwindow.cpp 里的 put() 调用重新包回
//       `if (frame == totalFrames - 1 || totalFrames == 1)`
//       ——也就是把代码改回改动前的样子。
//   结果:12/12 全绿。
//
// 原因很直白:本文件里的断言测的是**这个类的行为**,
// 而"主窗口每帧都调一次 put"是**接线**,它在 mainwindow.cpp 里,
// 而 mainwindow.cpp 根本不在任何测试目标里(它要真窗口 + 全部算子,
// 无头构造不了 —— 这也是 run_policy.h 里 configureLogView
// 遇到过的同一个问题)。
//
// 现在的能力边界:
//   ✓ 判据对不对、保留策略对不对、文案说不说 —— 测到了
//   △ 主窗口有没有真的每帧都喂进来 —— 只由一条**源码级 lint** 兜着
//     (test_run_policy.cpp 的 test_mainwindow_records_every_frame,
//     它能杀掉 M4,但换个等价写法就看不见 —— 它是 lint 不是行为测试)
//   ✗ 主窗口真的跑一遍之后缓存里有什么 —— 没测到,也没法测
//
// 说这一条是因为:如果不写下来,下一个人会以为"132 条断言全绿"
// 就等于"多帧缓存这层没问题"。它不是。它等于"这个类没问题,
// 接线只被一条 lint 看着"。这两句话的差别就是出事故时的差别。
//
// ============================================================

#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <algorithm>
#include <functional>

// ⚠ 2026-10-25:「这一步显示哪张图」的规则**只有一份**,在这里。
//   以前这一份和 mainwindow.cpp 里那份各写一遍,靠人工对齐 ——
//   然后真的跑偏了(判据查 A 图、显示 B 图)。
#include "result_render_plan.h"

namespace w7 {

// ============================================================
// 数字保留上限(帧/节点)—— 见本文件开头 ③
//
// 取值和 run_policy.h 的 kMaxRunFrames 一致,两个上限必须一致:
// 帧循环最多跑 kMaxRunFrames 帧,缓存最多留 kStepCacheMaxFrames 帧,
// 后者 >= 前者时上限打不到。两个数分开写却不绑定,迟早有人只改一个。
// (test_run_policy.cpp 里有一条断言专门钉这个不变量)
// ============================================================
inline constexpr int kStepCacheMaxFrames = 2000;

// ============================================================
// 一帧的图现在是什么状态
//
// Available 图在,可以正常显示
// Dropped  这一帧的图已经没了(被后来的运行覆盖,或被人删了)
//          —— 必须明说,不许拿别的帧 / 工具栏那张图顶替
// NoImage  这一步本来就不产图(纯数值算子,比如 blob_count)
// ============================================================
enum class FrameImageState {
    Available,
    Dropped,
    NoImage
};

// 存在性谓词:生产传 &w7::fileExists,单测传假的。
//
// ⚠ 2026-10-25:`ExistsFn` 和 `fileExists` **搬到 result_render_plan.h**。
//
//   以前它们只服务本文件的判据;现在「取哪张图」也要用同一个口径
//   (pickBaseImage),谓词得跟着规则一起搬 ——
//   规则搬到哪儿,能证明它的那把尺子也得在哪儿。
//
//   这里**不再重复定义**:重复定义 = 第二份 = 会漂。
//   (w7::ExistsFn / w7::fileExists 还能用,是因为本文件 include 了
//    result_render_plan.h。)

// ============================================================
// 这一步的「主图」是哪个字段
//
// ⚠ 2026-10-25:这一份**删掉了**,规则搬到 result_render_plan.h。
//
//   以前这里写着:
//
//     「优先级必须和 renderOutputToResultPanel 的显示顺序对齐 ——
//       两份各写一份的话,加一种新输出类型就只改一处,于是"图还在不在"
//       的判据和"显示哪张"会跑偏,而跑偏的方向恰恰是那个会撒谎的方向。」
//
//   ——**作者自己写下了这条警告,然后两份还是各写了一份**,
//   然后它们真的跑偏了:
//
//     · 这一份的优先级:mask_path -> marked_path -> image_path
//     · 面板那份的显示顺序:mask -> 模板框 -> PASS/FAIL 大字 -> 原图
//         (面板**压根不认 marked_path**)
//
//   而 pcb_defect_judge 的输出**没有 image_path**,只有 marked_path ——
//   于是面板会落到 fallback(工具栏那张用户手填的静态图),
//   判据却查 marked_path(逐帧的缺陷标注图)。
//   **两个说的不是同一张图。**
//
//   收法:两份都改成用 result_render_plan.h 的**同一份候选列表**,
//   「图还在不在」和「显示哪张」从此必然一致。
//   详见 result_render_plan.h 开头。
// ============================================================

// ============================================================
// 一条缓存记录
//
// QJsonObject / QString 都是隐式共享的值类型,拷贝几乎不要钱,
// 所以每帧一份在内存上是划算的(实测见文件末尾)。
// ============================================================
struct StepFrameResult {
    QJsonObject output;      // 数字,全帧留
    QString     label;       // "第 3 步 · 颜色分割"(与帧无关,但跟着存一份最省心)
    // ⚠ 2026-10-25:原来的 `QString imagePath` 字段**删掉了**。
    //   它存的是「主图路径」—— 而「主图是哪个」这件事以前有**两份**规则
    //   (这里一份、mainwindow.cpp 里一份),而且真的跑偏了。
    //   现在「主图」由 result_render_plan.h 的候选列表决定,
    //   需要哪张图时**现场算**(连同 fallbackPath),不缓存 ——
    //   缓存一份就等于又埋了一份会过期的副本。
};

// ============================================================
// 多帧缓存:节点 uuid -> 帧号 -> 记录
// ============================================================
// 用 QUuid / QString 做 key 而不是 OperatorNode* —— 节点删掉重建
// 地址会变,裸指针做 key 会留下指向已释放内存的 key,清缓存时崩。
// (mainwindow.cpp 里 onNodeDeleteRequested 那个"先取 uuid 再删"的
//  注释就是这件事的现场记录。)
// ============================================================
class StepFrameCache {
public:
    void clear() {
        m_byNode.clear();
        m_evicted = 0;
    }

    // 记一帧。返回**这次因为撞上限而丢弃的帧数**(0 或 1)。
    // 返回这个数而不是 void,是为了让调用方**没法装作没看见** ——
    // 上限触发时必须打一行日志,理由见文件开头 ③。
    int put(const QString& nodeId,
            int frameIndex,
            const QJsonObject& out,
            const QString& label,
            int maxFrames = kStepCacheMaxFrames) {
        if (frameIndex < 0 || nodeId.isEmpty()) return 0;
        // 上限 0 = 什么都不留。这条必须在**建条目之前**判,否则
        // "丢最老的一帧"在空表上无事可丢,这一帧反而被存了进去 ——
        // 上限 0 却留着 1 帧,比不留还难解释。
        if (maxFrames <= 0) { ++m_evicted; return 1; }

        QHash<int, StepFrameResult>& frames = m_byNode[nodeId];
        // 已经存过这一帧(重跑同一帧,或源越界停在最后一张):
        // 覆盖即可,不能算作新增,否则上限会被同一帧反复撑爆。
        if (frames.contains(frameIndex)) {
            frames[frameIndex] = makeEntry(out, label);
            return 0;
        }
        int evicted = 0;
        if (frames.size() >= maxFrames) {
            evictOldest(frames);   // 丢最小帧号 = 最老的一帧
            ++evicted;
            ++m_evicted;
        }
        frames.insert(frameIndex, makeEntry(out, label));
        return evicted;
    }

    bool hasNode(const QString& nodeId) const {
        return m_byNode.contains(nodeId);
    }

    bool hasFrame(const QString& nodeId, int frameIndex) const {
        return frameOf(nodeId, frameIndex) != nullptr;
    }

    // 取一帧;没有就返回空记录(output 为空)。
    // 调用方要用 hasFrame 区分"没有这一帧"和"这一帧确实没有输出"。
    StepFrameResult at(const QString& nodeId, int frameIndex) const {
        if (const StepFrameResult* e = frameOf(nodeId, frameIndex)) return *e;
        return {};
    }

    // 某节点缓存了哪些帧 —— 升序
    QList<int> framesOf(const QString& nodeId) const {
        QList<int> keys;
        const auto it = m_byNode.constFind(nodeId);
        if (it != m_byNode.constEnd()) keys = it.value().keys();
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    // 某节点最后缓存的那一帧(没有则 -1)。
    // 本轮右键回看默认看哪一帧就靠它 —— 和改动前"只缓存最后一帧"
    // 的行为一致,所以这一步**不改变**用户看到的内容,只是
    // 底下的数据从一份变成了 N 份。
    int latestFrameOf(const QString& nodeId) const {
        QList<int> ks = framesOf(nodeId);
        return ks.isEmpty() ? -1 : ks.last();
    }

    // --------------------------------------------------------
    // 要不要显示帧选择器
    //
    // 只有一帧时藏掉:两个方向的"首尾"按钮都按不动,spin 只有一个值,
    // 留着它只会让人以为右边那几个按钮坏了。
    //
    // 这条判据放在**这里**而不是 updateFrameBar() 里,是因为它纯逻辑、
    // 无头可测,而 updateFrameBar 动的是 widget —— 头文件里不动 widget
    // 那个约定(见本文件开头的说明)就是为这种时候准备的。
    // --------------------------------------------------------
    bool showFrameSelector(const QString& nodeId) const {
        return framesOf(nodeId).size() > 1;
    }

    // 全部节点里最大的帧号(没有则 -1)—— 用来在标题里说"共 M 帧"
    int latestFrame() const {
        int best = -1;
        for (auto it = m_byNode.constBegin(); it != m_byNode.constEnd(); ++it) {
            const QList<int> ks = framesOf(it.key());
            if (!ks.isEmpty() && ks.last() > best) best = ks.last();
        }
        return best;
    }

    int nodeCount() const { return m_byNode.size(); }
    int entryCount() const {
        int n = 0;
        for (auto it = m_byNode.constBegin(); it != m_byNode.constEnd(); ++it)
            n += it.value().size();
        return n;
    }
    // 因为撞上限而被丢弃的帧数,累计。
    int evictedCount() const { return m_evicted; }

    void removeNode(const QString& nodeId) { m_byNode.remove(nodeId); }

    // --------------------------------------------------------
    // 图在不在 —— 现场问磁盘,不查存下来的标志(理由见文件开头 ②)
    // --------------------------------------------------------
    //
    // ⚠ 2026-10-25:判据改成用**共享的候选列表**,
    //   而候选列表和 mainwindow.cpp 里「显示哪张图」用的是**同一份**
    //   (result_render_plan.h :: resultImageCandidates)。
    //
    //   以前这里查的是 `e->imagePath`(一份单独的"主图"规则),
    //   而面板显示的是另一套顺序 —— **两个说的不是同一张图**。
    //   后果是:判据说「图在」(标注图还在),面板显示的却是
    //   工具栏那张用户手填的静态图 —— 对应关系是假的。
    //
    // ⚠ 判据只看**第一候选**,**不降级**:
    //   候选列表第一项 = 「最能说明这一步干了什么」的那张图。
    //   它不在了,就说「图没了」,**不拿低一级的图顶上** ——
    //   顶上来的那张可能早就被下一轮覆盖了,
    //   而用户会以为那就是这一帧的图。
    //
    //   这也是以前的行为(主图不在 -> Dropped -> 面板那道闸把图关掉),
    //   只是现在「主图是谁」由**一份**规则说了算。
    //
    //   不降级还带来一个好处:
    //   判据说 Available ⟹ 第一候选**真的在**
    //   ⟹ 面板显示的**就是它**。
    //   两个说的永远是同一张图 —— 而且这**不是靠两边对齐**,
    //   是因为下面这一行和面板那一边调的是**同一个函数**。
    FrameImageState imageState(const QString& nodeId,
                               int frameIndex,
                               const QString& fallbackPath,
                               const ExistsFn& exists) const {
        const StepFrameResult* e = frameOf(nodeId, frameIndex);
        if (!e) return FrameImageState::Dropped;   // 数字都没了,图更谈不上
        const QVector<w7::ImageCandidate> cands =
            w7::resultImageCandidates(e->output, fallbackPath);
        if (cands.isEmpty()) return FrameImageState::NoImage;   // 这一步压根不产图
        // ⚠ 取哪张图:**同一个函数**(result_render_plan.h :: pickBaseImage),
        //   面板显示时也调它。
        //   以前这里是 exists(cands.first().path),面板那边是「取第一个存在的」——
        //   两份实现,靠「Dropped 那道闸在候选循环之前」这个代码顺序对齐。
        //   那是**第三份**抄副本(本项目栽过八遍的那种病),
        //   而且面板那份还会撒谎:「最终结果」那次调用没有判据,
        //   于是判定图不在时,面板会退到工具栏那张静态图 ——
        //   一张和这一帧无关的图,配着这一帧的判定。
        return w7::pickBaseImage(cands, exists) ? FrameImageState::Available
                                                 : FrameImageState::Dropped;
    }

    // --------------------------------------------------------
    // 给用户看的一句话 —— 别让界面自己编文案
    // --------------------------------------------------------
    // 单独抽成函数是因为这段话有三种状态,写三遍必然出现
    // "有一种状态忘了说",而那一种恰好是要命的(Dropped 不说
    // 就变成拿别的图顶替)。
    //
    // 帧号用「文案」而不是 int 传进来,是为了让调用方能写
    // "第 7 帧"也可以写"这一帧" —— 而不用在调用处拿正则去改
    // 上一个调用点生成的字符串(那种改法迟早匹配失败,而且
    // 匹配失败时是**静默**的:文案照旧,只是帧号又对不上了)。
    static QString describeImageState(FrameImageState s, const QString& frameDesc) {
        switch (s) {
        case FrameImageState::Available:
            return QString();
        case FrameImageState::NoImage:
            return QStringLiteral("这一步不产生图像,下面是它的输出数字。");
        case FrameImageState::Dropped:
            return QStringLiteral(
                "%1的中间图已经不在了(被后来的运行覆盖,或被删除了)。\n"
                "下面的数字是这一帧自己的结果,没有配图 —— "
                "**不会**用别的帧的图来替它。").arg(frameDesc);
        }
        return QString();
    }

    // 帧号已知的场合用这个重载(内部把 0-based 帧号转成 1-based 文案)
    static QString describeImageState(FrameImageState s, int frameIndex) {
        return describeImageState(s, QStringLiteral("第 %1 帧").arg(frameIndex + 1));
    }

private:
    static StepFrameResult makeEntry(const QJsonObject& out, const QString& label) {
        StepFrameResult e;
        e.output = out;
        e.label  = label;
        // ⚠ 2026-10-25:不再在这里算「主图路径」。
        //   「主图是哪个」由 result_render_plan.h 说了算,
        //   需要时**现场算**(它依赖 fallbackPath,而 fallbackPath 是
        //   界面的状态,不属于缓存)。
        //   在这里存一份 = 又埋一份会过期的副本。
        return e;
    }

    const StepFrameResult* frameOf(const QString& nodeId, int frameIndex) const {
        const auto it = m_byNode.constFind(nodeId);
        if (it == m_byNode.constEnd()) return nullptr;
        const auto f = it.value().constFind(frameIndex);
        if (f == it.value().constEnd()) return nullptr;
        return &f.value();
    }

    static void evictOldest(QHash<int, StepFrameResult>& frames) {
        int oldest = -1;
        for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
            if (oldest < 0 || it.key() < oldest) oldest = it.key();
        }
        if (oldest >= 0) frames.remove(oldest);
    }

    QHash<QString, QHash<int, StepFrameResult>> m_byNode;
    int m_evicted = 0;
};

} // namespace w7
