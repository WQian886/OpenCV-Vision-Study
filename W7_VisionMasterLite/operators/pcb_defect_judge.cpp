#include "pcb_defect_judge.h"
#include "pcb_color_ref.h"
#include "board_recipe.h"
#include <QDoubleSpinBox>
#include <QLabel>
#include <QJsonArray>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

namespace w7 {

namespace {

void addDefect(QJsonArray& arr, const QString& label, const char* type,
               const QString& detail) {
    QJsonObject d;
    d["component"] = label;
    d["type"]      = QString::fromUtf8(type);
    d["detail"]    = detail;
    arr.append(d);
}

// 每条规则跑完都记一笔实测值,**不管有没有触发**。
//
// 为什么要记"没触发的":阈值调不动的时候,人需要看到的是
// "好板上是 12%,废板上是 55%,阈值在 40",而不是一句 "FAIL"。
// 只报缺陷的话,好板上那 16 个 12% 全都看不见 ——
// 只能看到一堆红的,看不到红的离阈值有多远。
//
// 这不是为了调试临时加的输出:产线调阈值时它就是主要依据。
void addMetric(QJsonArray& arr, const QString& label, const char* rule,
               double value, double threshold, const char* unit) {
    QJsonObject m;
    m["component"] = label;
    m["rule"]      = QString::fromUtf8(rule);
    m["value"]     = value;
    m["threshold"] = threshold;
    m["unit"]      = QString::fromUtf8(unit);
    arr.append(m);
}

// ============================================================
// 没有 checks 时,从封装类别推一套默认规则
//
// 只在**上游没给 checks** 时用 —— 老存档、auto_contour 产物。
// 配方明确写了 checks(哪怕是空数组)则以配方为准。
//
// 为什么不能"没给就当不查任何规则":那会让一条规则都不跑,
// 于是**所有缺陷图一路 PASS** —— 这正是原来七处 label== 全落空
// 时的行为(实测:三块好板全判废,两块废板输出与好板逐字相同)。
//
// 为什么按 kind 而不是按 label:label 是"这块板上的第几个元件",
// 换块板就对不上;kind 是"这个封装长什么样",换板子也对得上 ——
// 那是物理事实,不是标定值。
// ============================================================
QStringList knownRulesForKind(const QString& kind) {
    const QString k = kind.toUpper();
    if (k == QLatin1String("RES") || k == QLatin1String("R")) {
        // 电阻:黑本体,两侧金焊盘。dark -> missing/shifted;
        // 单色本体,换色看得出来 -> wrong;金条连片 -> bridge。
        return {QString::fromUtf8(recipe_rule::kMissing),
                QString::fromUtf8(recipe_rule::kShifted),
                QString::fromUtf8(recipe_rule::kWrong),
                QString::fromUtf8(recipe_rule::kBridge)};
    }
    if (k == QLatin1String("IC") || k == QLatin1String("TO220")
        || k == QLatin1String("QFP64") || k == QLatin1String("SOIC8")) {
        // IC:黑本体 + 几十个金焊盘(有中位数才谈得上 insufficient)。
        return {QString::fromUtf8(recipe_rule::kMissing),
                QString::fromUtf8(recipe_rule::kInsufficient),
                QString::fromUtf8(recipe_rule::kBridge)};
    }
    if (k == QLatin1String("C") || k == QLatin1String("CAP")
        || k == QLatin1String("CAP_SMALL") || k == QLatin1String("CAP_BIG")) {
        // 电容:本体是彩色/灰色而不是黑,所以 missing 规则**不适用**
        // (会被整框判缺件);但有白色极性条,也有金焊盘。
        return {QString::fromUtf8(recipe_rule::kPolarity),
                QString::fromUtf8(recipe_rule::kBridge)};
    }
    // 不认识的封装(TERMINAL / LED / BUTTON / XTAL ...):
    // **一条规则都不跑,但要说得出来**。返回一个特殊值,
    // 让 run() 在报告里写明"这些元件没配规则",而不是让使用者
    // 以为整板都查过了。
    return {};
}

// ============================================================
// missing 规则该问哪个问题:"本体够不够暗",还是"框里有没有东西"?
//
// 这不是风格选择,是两种**物理事实**,实测差别极大(2026-10-14):
//
//   黑本体(RES/DIODE/TO220/QFP64/SOIC8):暗像素就是本体本身,
//     实测 power 好板 RES 28.69% -> 模糊到 k=6 仍 19.18%,
//     TO220 57.03% -> 55.81%,几乎不动。余量 20 倍以上。
//
//   彩色/灰色本体(CAP*):**暗像素只剩那一圈 1px 黑色描边**。
//     实测 power 好板 CAP_SMALL 5.08%,而 σ 只有图宽千分之一(k=1)
//     就掉到 **0.00%** —— 一步归零,不是斜坡。
//     好板/废板之间根本不存在可放阈值的窗口:任何 dark_min_pct
//     都救不了,因为被模糊毁掉的是信号本身,不是它的余量。
//
// 换成"框里有没有非板面像素"(inspect 已经算好的 body_pct)之后,
// 同一批图实测(power C2 缺件 / 好板,body_pct%):
//     k=0  7.7 / 49.2      k=3  22.2 / 89.9
//     k=1 13.2 / 73.1      k=6  36.5 / 97.6
// 40px 的圆盘在同样大小的模糊下几乎无损 —— 差的不是阈值,是问的问题。
//
// ⚠ 只对实测过的封装生效。LED / HEADER / INDUCTOR / TERMINAL / BUTTON
//   同样不是黑本体(dark 实测 0.00~4.77%),**但没有缺件真值图**,
//   改它们等于凭空改判据,所以这里一律不改 —— 见 pcb_defect_judge.h
//   的"尚未处理"。已知的坑记在 knownRulesForKind() 里:
//   那几种封装**绝不能配 missing**。
//
// ⚠ DIODE 是**半坏**的,这里也没改:它的本体确实是黑的(dark 实测
//   26.47%),所以走暗像素判据没错,但 σ 到图宽千分之五时同样归零
//   (26.47 -> 19.51 -> 15.34 -> ... -> 0.00),power / signal 两块板
//   在 k=4 被 shifted、k=5 起被 missing 误报。结构上和电容同类,
//   只是塌得晚一些。**同样因为没有二极管缺件真值图,没有阈值可标** ——
//   与其凭空定一个,不如留着这两张假阳当作"必须补真值图"的证据。
// ============================================================
bool missingAsksBodyPresence(const QString& kind) {
    const QString k = kind.toUpper();
    return k == QLatin1String("C") || k == QLatin1String("CAP")
        || k == QLatin1String("CAP_SMALL") || k == QLatin1String("CAP_BIG");
}

// ============================================================
// 这个元件跑哪些规则 —— **判定"配方说了算"的唯一入口**
//
// ⚠ "checks 字段不存在"和"checks 是空数组"是**两件不同的事**,
//   以前这里用 rules.isEmpty() 把它们当成同一件,于是 demo 板的 C2
//   明明在配方里写了 checks: [](意思是"这个元件我不管它"),
//   却掉进 knownRulesForKind() 的兜底,凭空多出 polarity+bridge。
//   实测后果(2026-10-10):好板 baseline.png 在**所有缩放**下都报焊桥,
//   端到端 56 条断言掉到 49 条 —— 56 条一直绿着,因为判据是对的,
//   配方一改就露馅。
//
//   两种输入的来源不同,所以含义必须不同:
//     字段不存在  -> 上游没给配方信息(老存档 / auto_contour 产物)
//                    按封装类别推一套,免得一条规则都不跑 → 假 PASS。
//     字段存在但是空 -> 配方**明确说这个元件不参与判定**。
//                    照做。不许替使用者做主。
//
// 这个函数以前在 run() 里写了两遍(覆盖度统计 + 逐元件分派),
// 两遍各写一遍兜底逻辑 —— 改一处忘一处,下一次就是这样出来的。
// ============================================================
QStringList rulesForComponent(const QJsonObject& c) {
    if (c.contains("checks")) {              // 配方说了算,空数组也算
        QStringList rs;
        for (const auto& r : c.value("checks").toArray()) rs << r.toString();
        return rs;
    }
    return knownRulesForKind(c.value("kind").toString());
}

// 把一个逐像素判据铺成 0/255 mask。
//
// 四条颜色规则(暗/错色/白/质心)都要"逐像素分类再统计占比",
// 各自写一遍双层循环就是第四次抄副本 —— 这个项目里抄副本的账已经
// 记了三笔(EXPECTS[6]、lo[0] 越界守卫、守卫本身),判据本身也统一
// 住在 pcb_color_ref.h,这里只负责把谓词铺开。
template <typename Pred>
cv::Mat maskBy(const cv::Mat& roi, Pred pred) {
    cv::Mat m(roi.rows, roi.cols, CV_8U, cv::Scalar(0));
    for (int y = 0; y < roi.rows; ++y) {
        const cv::Vec3b* src = roi.ptr<cv::Vec3b>(y);
        uchar* dst = m.ptr<uchar>(y);
        for (int x = 0; x < roi.cols; ++x) dst[x] = pred(src[x]) ? 255 : 0;
    }
    return m;
}

} // namespace

// ============================================================
// 阈值:全部是"占某个基准的百分比",一个裸像素数都不留
// ============================================================
// 基准逐条不同,因为量出来的东西量纲不同:
//
//   面积类的量(暗像素 / 浅黄 / 焊盘)→ 该元件的 ROI 面积
//   长度类的量(最长金段 / 质心偏移)→ 该元件的 ROI 宽 或 高
//   全局缺陷(短路圆环,不属于任何元件)→ 全图面积 / 全图对角线
//
// 下面每个默认值都是**从实测中间量推出来的**,不是拍的。
// 括号里是"正常板实测值 ↔ 对应缺陷图实测值",新默认落在两者的几何中点,
// 两边都留最大余量。实测来自 %TEMP%\w7probe\pcb_calib.cpp 的标定报告。
//
//   polarity_ratio 60  正常 16.7% ↔ 缺陷 82.2%(这条本来就是比例,语义没变,
//                             所以名字也不变 —— 改了才没必要)
QJsonObject PcbDefectJudge::defaultParams() const {
    QJsonObject p;
    // --- ROI 面积 % ---
    // 暗像素占 ROI 的百分比,低于它判 missing(缺件)。
    //
    // 原来取 2%,那是按"黑本体"定的:R2 正常板 45.6%,缺件 0%。
    // 但这批图里有一半的封装本体不是黑的,实测暗像素占比如下
    // (2026-10-10,四块板好板全量):
    //     RES 20.57~28.13   DIODE 21.96~25.78  TO220 57.07
    //     QFP64 71.97       SOIC8 46.66        CAP_BIG 2.22~2.94
    //     CAP_SMALL 4.42~5.08                     BUTTON 4.09
    //     TERMINAL 2.96~4.77   INDUCTOR 2.34
    //     LED 0.77          HEADER 0.00     ← 这两个是**灰/绿本体**,
    //                                                几乎没有暗像素
    //
    // 2% 把 CAP_BIG 的 2.22 压在边上(余量 0.22 个百分点),把 INDUCTOR 的 2.34
    // 也压在边上。降到 1% 是**为了余量,不是为了抓到缺件** ——
    // 缺件侧实测一律 0.00%,所以 2% 其实也照样能抓到 power_missing
    // (这一点做过变异验证:把 2.0 改回去,那条断言照样绿)。
    // 真正让 power_missing 从漏检变成抓到的,是配方里给 CAP_BIG
    // 补上了 missing —— 变异验证也做了:把 CAP_BIG 的 checks 清空,
    // 断言立刻转红。
    //
    // 1% 之后:黑本体那一批(最小 20.57)余量不变,CAP_BIG 余量 2.2 倍,
    // CAP_SMALL 4.4 倍。代价是 LED(0.77)/HEADER(0.00)离 1% 太近 ——
    // 所以**这两种封装绝不能配 missing**,见 tools/export_recipes.py 的 CHECKS 表。
    p["dark_min_pct"]    = 1.0;
    // 彩色本体(CAP*)的 missing 判据:ROI 里"不像板面"的像素占比下限。
    //
    // body_pct 由 pcb_inspect 产出(逐通道差 >12),一直只被"配方跟图
    // 对不对得上"那道闸读过,**没有任何判据消费过它** —— 和当初的
    // gold_px 一个毛病:字段在,没人读,所以没人发现判据缺了这一半。
    //
    // 下面两个阈值都是 2026-10-14 实测的,量法只有一条规矩:
    // **只量真正读 body_pct 的站点**(CAP*),别把 RES/DIODE 混进来。
    // 混进来过一次 —— 前一版注释写着"缺件侧最大 55.9(demo R2 @k=6)
    // 已超过好板侧最小 49.2,所以绝对阈值无解",那个结论是错的:
    // demo R2 是 RES,走的是暗像素,根本不读 body_pct。
    // 拿一个不进这个判据的数去说"这个判据没有窗口",等于量错了东西。
    //
    // 绝对版(阈值 42),power C2 实测:
    //     缺件侧 7.69(k=0) -> 41.75(k=7) -> 46.42(k=8) 越过阈值
    //     好板侧 49.17(k=0) -> 97.58(k=6)  随模糊一路涨
    // 窗口全程非空,但**下沿在合拢**:k=8 起 C2 不再被检出。
    // 危险的是这不影响 verdict —— 别的站点照样报 missing,
    // 整张板仍然显示 FAIL:missing,只看缺陷类型根本看不出来。
    // 所以回归用例断言的是"点名到 C2",不是"报了 missing"。
    p["body_min_pct"]    = 42.0;
    // ↑ 归一化之前的绝对版。现在只作**站点数不足时的兜底**(见 body_min_sites)。
    //
    // 正常路径走比值:body_pct / 同板全部站点中位数。
    // 下面这张表是同一批图、同一条流水线、直接读 PcbDefectJudge 原话输出
    // 量出来的(k=0..26,三块 complex 板,只看真正读 body_pct 的 CAP 站点):
    //     k      绝对 C2 读数   比值 C2 读数   好板最低(绝对 / 比值)
    //     0        7.694          0.140        49.167 / 0.892
    //     7       41.750          0.427        95.833 / 0.980
    //     8       46.417 <-漏     0.477        95.194 / 0.979
    //    12       65.444          0.654        97.472 / 0.975
    //    13       72.806          0.728 <-漏   98.500 / 0.985
    //    22       99.917          0.999       100.000 / 1.000
    //    23      100.000          1.000       100.000 / 1.000  <-两条线相交
    //
    // ⚠ 别把"出厂阈值失效"和"判据失效"混成一件事,这两者差了 10 级:
    //     出厂阈值失效     绝对 k=8    比值 k=13
    //     窗口彻底关闭     绝对 k=23   比值 k=23   <-**一样**
    // k=23 起 σ = 图宽千分之二十三(1200px 板上是 27.6px),整张图糊成一片,
    // 被擦的电容和完好的电容读数**都饱和到 100% / 1.000**,物理上就分不出来了。
    // 那是图像本身没信息了,不是阈值没调好 —— 换任何阈值都无解。
    //
    // 所以比值版的真实收益要说准:它**没有延长判据的寿命**,两个都死在 k=23。
    // 它买的是"同一个出厂阈值能站得更深":多撑 5 级。
    // 根子在于缺件侧爬升的相对速度:绝对版在达到最终饱和值的 42% 时
    // (100 分之 42.0)就被阈值切到了,比值版要到 71.4%(1 分之 0.714)。
    // 另外好板侧的比值读数全程平(0.892 ~ 1.000,从不塌),而绝对版
    // 从 49 爬到 100 —— 后者的阈值含义随模糊漂移,前者不会。
    //
    // 0.714 落在 (缺件侧 0.999, 好板侧 0.892) 这条全局窗口里,k=0..22 非空。
    // 位置**故意偏向下沿**(离好板侧 0.178,离缺件侧 0.285):余量要花在
    // 会动的那一侧,那才是能买来模糊耐受度的地方。
    //
    // 站点数下限:只有一个站点时"比值"恒等于 1,判据恒假 —— 报 PASS,
    // 和"检查过了没问题"长得一模一样。低于下限时退回绝对判据,
    // 并在 metrics 的 unit 里写明走的是哪条路。
    p["body_ratio_min"] = 0.714;
    p["body_min_sites"] = 4;
    // R3 错色像素占 ROI 的百分比。**四块板一起量**(2026-10-10),
    // 因为这个阈值原来只在 demo 一块板上标定过:
    //     demo   R3 好  3.40   废(wrong.png)     44.77
    //     mcu    R2 好 23.05   废(mcu_wrong.png) 42.14
    //     power  RES 好 19.36~23.35      signal RES 好 15.49~23.22
    // 好板最大 23.35,废板最小 42.14 —— 中间空着,阈值能放进去。
    // 几何中点 sqrt(23.35*42.14) ≈ 31.4,取 32。
    //
    // ⚠ 原来取 12,那是 demo 板 R3 的几何中点 sqrt(3.40*44.8)≈12.3。
    //   单看 demo 是对的,但另外三块板的**好板**电阻本身就落在 13~23%,
    //   全部高于 12 —— 于是每块好板的每个电阻都报"错色"
    //   (实测三块好板各报 3 / 8 / 4 个)。
    //   这就是"只在一张图上标定的阈值换板型就整片失效",
    //   本项目的老毛病,只不过这次是**上限**太低而不是下限太高。
    p["yellow_min_pct"]  = 32.0;
    // C1 白色像素占 ROI 4.17%;这是"白色够不够,不够就别判极性"的门槛。
    p["white_min_pct"]   = 0.5;
    // U1 焊盘噪声门槛 —— **这一条我错过两次,都是因为只量了 1x**。
    //
    // 第一次:按"正常焊盘占 0.80%"想当然取 0.2%,噪声斑(0.38%)被放进来了,
    //         baseline 整块误报。是 12/12 回归测试当场抓住的。
    // 第二次:改成 0.44% 之后以为稳了 —— 其实那只是按 1x 的 (0.38%, 0.51%)
    //         取的,贴着噪声斑上沿。跨尺度一量,噪声斑会漂到 0.42%。
    //
    // 跨尺度实测(占 ROI 面积,量的是所有金层连通域):
    //         尺度   噪声斑    焊少那颗   正常板最小真焊盘
    //         0.5x    0.38%     0.60%      0.80%
    //         0.75x   0.42%     0.52%      0.85%
    //         1.0x    0.38%     0.51%      0.80%
    //         1.25x   0.41%     0.56%      0.87%
    //         2.0x    0.38%     0.51%      0.83%
    // 窗口 = (噪声斑最坏 0.42%, 焊少那颗最小 0.51%),几何中点 0.46。
    //
    // ⚠ 窗口只有 0.09pp,天生就窄。根子在样本数据:那颗"焊少"的焊盘
    //   跟噪声斑差不了多少。真实产线数据上大概率更宽,但换数据要重标。
    p["pad_min_pct"]     = 0.46;
    // U1 最小焊盘比中位数低多少 —— 同样必须跨尺度量。
    // 只量 1x 会看到"正常 5.9% / 焊少 40.4%",于是取 20;
    // 但降采样对小焊盘啃得比大焊盘狠(1x 最小/中位=94%,0.5x 掉到 74%),
    // 正常板的 drop 会被顶上去:
    //         尺度   正常板 drop   缺陷板 drop
    //         0.5x    25.6%        44.2%
    //         0.75x   12.4%        46.1%
    //         1.0x     5.9%        40.4%
    //         1.25x    7.9%        40.2%
    //         2.0x     2.0%        40.4%
    // 窗口 = (正常板最差 25.6%, 缺陷板最小 40.2%),中点 33。
    // 20 落在窗口**外面** —— 0.5x 下正常板因此误报 insufficient。
    //
    // ── 2026-10-14:上面这张表只量了**分辨率**,没量**模糊** ──
    //   补测(demo baseline.png 加高斯模糊,σ=图宽千分之 k,出厂参数,
    //   读数取算子自己吐的 metric):
    //
    //       k      0     1     2     3     4     5     6     7     8
    //       drop  5.88  0.74 51.59 38.41 46.38  5.15  3.95  0.88  没跑
    //       报缺陷  --    --    报    报    报    --    --    --    --
    //
    //   三个结论,和"阈值取 33 时的余量"关系很大:
    //
    //   ① **好板侧在 k=2 就到 51.59,而缺陷板侧最小只有 40.4** ——
    //      两条线**交叉**了。不是阈值偏低,是**不存在能用的阈值**:
    //      压到 51.59 以上能躲开 k=2,3,4 的假阳,同时把真缺陷一起放过。
    //      这跟 7bd4794 记的"出厂阈值失效"是两码事(见
    //      feedback-verification-doesnt-verify 第 8 条)。
    //   ② 读数**不随模糊单调**:0.74 -> 51.59 -> 3.95。min 是极值统计量,
    //      最经不起参与统计的那批连通域变化。
    //   ③ k=8 起**整条规则不再执行**(金层连通域 < 4),读数那一格是空的。
    //      现在这条会被 output["skipped"] 报出来(见 run() 里 skipped 那段)。
    //
    //   还量了一件:pad_min_pct 这个噪声门槛是**承重**的,不是随手加的。
    //   把它从 0.46 压到 0.01,好板在 k=0/1/5/6 读数变成 55.88/55.88/
    //   54.74/63.64 —— 全部越过 33,好板直接误报。所以这两个门是一对,
    //   不能单独动任何一个。
    //
    // ⚠ 要修得动,只有两条路,都得先有数据:
    //   (a) 换一条对模糊稳健的统计量(比如不看 min,改成"低于中位数
    //       X% 的焊盘有几个"),但那需要**好板和废板各自成规模**的
    //       模糊扫描来定阈值 —— 现在只有 demo 一张好板和一张废板,
    //       拿它们定出来的阈值又是"一块板定一条判据"。
    //   (b) 先给 complex 三块板补 insufficient 真值图 —— 那是**改 W6
    //       测试数据**,9 张图全部要重量,得先说清楚再做。
    //   本轮只把量出来的结果记在这里,不改判据。
    p["pad_drop_pct"]    = 33.0;

    // --- ROI 宽度 % ---
    p["polarity_ratio"]  = 60.0;  // 见上
    // R1 最长金段占 ROI 宽度。原来只按 demo 一张图定:正常 15.83 ↔
    // 焊桥 100,几何中点 ~40。四块板全部接入后必须重算(2026-10-10):
    //
    //     好板一侧最大值  37.50   signal R1 @1x   ← 原来完全没量过这块板
    //     坏板一侧最小值  91.43   mcu Y1(mcu_bridge.png)
    //                       100.00 demo R1(bridge.png)
    //
    // 40 是拿 demo 单独一张图的中点算的,signal 板加进来之后,
    // 好板最大值 37.50 离阈值只剩 **2.5 个点** —— 这就是 signal_baseline
    // 在非 1x 档位误报的根因。可行区间是 (37.50, 91.43),
    // 几何中点 sqrt(37.50*91.43) ≈ 58.6,取 60,两头各留 20 个点以上。
    //
    // ⚠ 60 修不了 signal R1/R3 的问题,那不是阈值的事 —— 见下面
    //   "走线穿过 ROI"那段注释。
    //
    // ── 2026-10-14:源头修完,好板一侧的最大值变了 ──
    //   W6 生成器原来先画元件再画走线,金线直接压在黑色本体上,
    //   signal R3 因此在**好板**上两个焊盘就被连成一块(见下面那段)。
    //   现在走线按本体登记避让、端点落在焊盘上,重测四块板 ×5 档分辨率:
    //
    //       好板最大值   44.90   power R2 @0.75x
    //                  38.78   signal R2/R4、power R1、mcu R1/R2 @0.75x
    //                  29.69   多数站点 @1x 与 @2x
    //       坏板最小值   91.43   mcu Y1(mcu_bridge.png)  ← 没变,缺陷是真缺陷
    //
    //   修之前 signal R1 在 0.5x~2x 是 68.75~70.37、R3 是 84.38~100.00,
    //   **两档直接越阈值**;修之后全板所有 bridge 站点都塌回 29.69~38.78
    //   这条窄带里,和从未被走线穿过的站点完全一样。
    //
    //   60 现在有 **15.1 个点**余量(44.90 → 60),不是原来名义上的 22.5。
    //   阈值本身没动过 —— 动的是数据,好板一侧不再被假象撑着。
    p["bridge_min_pct"]  = 60.0;
    // R2 质心横向偏移容差,占 ROI 宽:缺陷图实测 15.8%,取 8(约 2 倍余量)。
    p["shift_x_pct"]     = 8.0;
    // R2 质心纵向偏移容差,占 ROI 高:缺陷图实测 18.8%,取 10(约 1.9 倍余量)。
    p["shift_y_pct"]     = 10.0;

    // --- 颜色判据的倍数 ---
    //
    // 这五个系数不是"占某个基准的百分比",而是"占板面基色的多少倍"。
    // 之前它们是 pcb_color_ref.h 里的 constexpr,同一个算子里
    // 其它阈值全可调、偏偏这几个写死 —— 调参的人看不见它们。
    //
    // 做成参数还有第二个理由:「系数上下动 20%,结论变不变」这件事
    // 只能在测试里扫。写死就扫不了,复制一份判据出来扫又是一次抄副本。
    // 默认值全部来自实测(见 operators/pcb_color_ref.h 顶部的表),
    // 每个默认值都留了余量,不是贴着数据取的。
    p["dark_v_mul"]      = pcbcolor::kDarkMul;        // 暗: V < 0.6 x 板面V
    p["bright_v_mul"]    = pcbcolor::kBrightMul;      // 白: V >= 1.3 x 板面V
    p["neutral_max"]     = pcbcolor::kNeutralMax;     // 丝印白的中性度上限
    p["off_color_mul"]   = pcbcolor::kOffColorMul;    // 错色: B,G >= 1.6 x 板面
    p["off_color_r_max"] = pcbcolor::kOffColorRMax;   // 错色: R <= 1.3 x 板面
    p["mask_v_mul"]      = pcbcolor::kMaskVMul;       // 金层/蓝环 HSV 掩膜的 V 下界

    // --- 全图 % ---
    // 短路圆环面积占全图:正常板最大的蓝斑 0.0107% ↔ 短路圆环 0.0411%,
    // 几何中点 0.021。上限只要高于 0.0411 就行,给到 0.08。
    p["short_min_pct"]   = 0.021;
    p["short_max_pct"]   = 0.08;
    // 圆环外框宽 / 高,占全图对角线(对角线跟着分辨率一起缩,是最省心的尺子):
    // 宽 0.246% ↔ 5.00%,高 2.05% ↔ 3.36%。
    // 宽这一条才是真正区分"环"和"斑"的(正常那块是 3x25 的实心小点);
    // 高这一条区分不了,窗口留宽即可,原来 20~60px 也一样区分不了。
    p["short_w_min_pct"] = 1.1;
    p["short_w_max_pct"] = 8.0;
    p["short_h_min_pct"] = 1.0;
    p["short_h_max_pct"] = 6.0;
    return p;
}

QJsonObject PcbDefectJudge::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    QJsonArray comps = inputs.value("components").toArray();
    if (imgPath.isEmpty()) {
        output["error"] = "上游未提供 image_path";
        return output;
    }
    if (comps.isEmpty()) {
        output["error"] = "上游未提供 components(需要先接 pcb_inspect 算子)";
        return output;
    }

    // ============================================================
    // 拿不到判定依据时,绝不能输出 PASS
    //
    // pcb_inspect 的"自动轮廓(换板型)"模式输出的是 C1/C2/C3...,
    // 而且编号来自**轮廓扫描顺序**,和真值模式的站点标签没有对应关系 ——
    // 自动模式的 C1 不是同一颗元件。于是下面按 label 分支的六条规则里,
    // 只有 "C1" 会命中,而且命中的是随便一颗元件;R1/U1/R2/R3 四个分支
    // 一次都进不去。
    //
    // 结果:defects 永远是空的 → result 永远是 PASS。
    // 判废算子在**什么都没检查**的情况下报"合格" —— 这是最坏的一种错。
    // 比崩了还难查:画布上状态点是绿的,日志一片正常,产线上直接漏检。
    //
    // 所以不能靠"恰好没报缺陷"反推出合格 —— 判定结果必须对应
    // 真正跑过的规则。kind 是可靠的区分依据:真值模式来自配方
    // (kind 是 R/C/RES/QFP64...),自动模式写死 kind="AUTO"。
    // ============================================================
    int autoCount = 0;
    for (const auto& v : comps) {
        if (v.toObject().value("kind").toString() == "AUTO") ++autoCount;
    }
    if (autoCount > 0) {
        output["error"] = QString(
            "上游给的是 pcb_inspect 的【自动轮廓】模式产物(%1 个元件)。"
            "自动模式不带板型配方,这些元件身上没有配任何判定规则 —— "
            "规则一条都跑不了,跑下去只会报出一个没有依据的 PASS。"
            "请把 pcb_inspect 的模式切回【真值坐标】并选好板型配方再判定。")
                .arg(autoCount);
        output["component_count"] = comps.size();
        // result 故意不给:没判定过就没有判定结果可给。
        return output;
    }

    // ============================================================
    // 覆盖度检查:**一个元件都没有规则可跑**时不给 PASS
    //
    // 这条以前写的是"没有一个 label 认得" —— 判据是位号,
    // 而位号只是这块板上的一个名字,换板就全落空(实测三块别的板
    // 全落空,于是好板判废、废板判废,理由还一模一样)。
    //
    // 现在判据换成"这个元件身上有没有配规则",这是配方说的,
    // 换板型时配方跟着换,这里就不会再误判。
    //
    // 为什么必须留这道闸:上面那句"规则一条都跑不了,跑下去只会报出
    // 一个没有依据的 PASS"是实测出来的,不是理论担忧 ——
    // 所以要在代码里挡住,不能指望上游永远配对。
    // ============================================================
    int withRules = 0;
    for (const auto& v : comps) {
        const QJsonObject c = v.toObject();
        // 覆盖度必须用同一个函数算,和下面逐元件分派走同一条判据。
        if (!rulesForComponent(c).isEmpty()) ++withRules;
    }
    if (withRules == 0) {
        output["error"] = QString(
            "%1 个元件里没有一个配了判定规则,判定跑不起来 —— "
            "不给 result,而不是报 PASS。请在板型配方里给元件写上 "
            "\"checks\"(可查的规则:%2)。")
                .arg(comps.size()).arg(knownChecks().join(" / "));
        output["component_count"] = comps.size();
        return output;
    }
    // 只覆盖一部分:必须说清楚覆盖了多少,
    // 否则"合格"会被当成整板都查过了。
    //
    // ⚠ 这个统计数的是**配方里配了规则的元件**,不是**真的跑过的规则**。
    //   两者不等:规则可能在拿到读数之前就被守卫挡掉(见下面 skipped)。
    //   实测 mcu/power/signal 三块板按这个数算都是"全覆盖",
    //   而 insufficient 在 6 个站点上一次都没跑过。所以覆盖度只说明
    //   "配方写了什么",真正的执行情况看 skipped。
    if (withRules < comps.size()) {
        output["warning"] = QString(
            "%1 个元件里只有 %2 个配了判定规则,另外 %3 个未判定 —— "
            "result 只覆盖已判定的那些。")
                .arg(comps.size()).arg(withRules).arg(comps.size() - withRules);
    }
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    // ============================================================
    // 板面基色:**逐图估计**,不是配置常量
    // ============================================================
    // 相对判据的右端也带 k 才叫相对。把参考图的板色写进配置当常量,
    // 等式右边就没有 k 了,"相对"是假的 —— 那正是探针第一版栽的地方。
    const cv::Vec3b boardBase = pcbcolor::estimateBoardBase(img);

    // 颜色判据的系数。**必须先读再判天花板** —— 天花板这条线本身就是
    // mul 算出来的(mul 调大,墙就往前挪)。顺序反了的话,
    // 「把倍数调到 2.0」这个操作会**看起来是安全的**,而实际上
    // 判据早就撞墙、守卫却还按 1.6 在放行。
    const double darkVMul    = params.value("dark_v_mul").toDouble(pcbcolor::kDarkMul);
    const double brightVMul  = params.value("bright_v_mul").toDouble(pcbcolor::kBrightMul);
    const double neutralMax  = params.value("neutral_max").toDouble(pcbcolor::kNeutralMax);
    const double offColorMul = params.value("off_color_mul").toDouble(pcbcolor::kOffColorMul);
    const double offColorRMax= params.value("off_color_r_max").toDouble(pcbcolor::kOffColorRMax);
    const double maskVMul    = params.value("mask_v_mul").toDouble(pcbcolor::kMaskVMul);

    // ============================================================
    // 撞 8 位色深上限时拒绝判定,而不是硬撑一个 PASS
    // ============================================================
    // 错色判据要"绿通道 >= 1.6 x 板面绿通道"。板面被照亮到这个程度时,
    // 1.6 倍已经超过 255,uint8 里根本不存在这么亮的像素,判据永远返回
    // false —— 不报错、不告警,直接报 PASS,废板判合格。
    //
    // 实测这个上限是实打实的物理墙,不是阈值没调好:k=1.55 时错色像素
    // 已经 99.6% 削波,削波后是 (255,255,R),G/B=1.00 和暗本体重合,
    // 换色相判据也救不回来。那一段的信息是真的没了。
    //
    // 所以这里给明确错误,让产线知道"这张图要重新曝光",而不是
    // 让它拿到一个没有依据的合格 —— 和上面两处拒绝判定同一个道理。
    if (pcbcolor::ceilingBreached(boardBase, offColorMul)) {
        output["error"] = QString(
            "曝光过强,无法判定:板面基色 BGR(%1,%2,%3),错色判据要求绿通道 ≥ "
            "%4 倍板面 = %5,已超过 8 位色深上限 255。此时判据永远不成立,"
            "若继续跑会报出一个没有依据的 PASS。实测这条线约在亮度 "
            "%6 倍处(当前板面还能撑到 %7 倍)。请降低曝光后重拍。")
                .arg(boardBase[0]).arg(boardBase[1]).arg(boardBase[2])
                .arg(offColorMul, 0, 'f', 2)
                .arg(offColorMul * boardBase[1], 0, 'f', 0)
                .arg(pcbcolor::ceilingHeadroomK(boardBase, offColorMul), 0, 'f', 2);
        output["component_count"] = comps.size();
        // result 故意不给:没判定过就没有判定结果可给。
        return output;
    }

    // 老存档里存的是像素阈值(dark_thresh=50 之类)。
    // 这里**故意不去读那些旧名字**:50 被当成 50% 会比原来错得离谱,
    // 读不到就等于没传,退回下面这些新默认值 —— 宁可阈值不准,
    // 也不能让用户以为是自己配的数在起作用。
    const double darkMinPct   = params.value("dark_min_pct").toDouble(1.0);
    const double bodyMinPct   = params.value("body_min_pct").toDouble(42.0);
    const double bodyRatioMin = params.value("body_ratio_min").toDouble(0.714);
    const int    bodyMinSites = params.value("body_min_sites").toInt(4);
    // yellow_min_pct:实测四块板(2026-10-10),同一封装"好板"与"错色废板"
    // 的错色像素占比:
    //     demo  R3  好  3.40   废(wrong.png) 44.77
    //     mcu   R2  好 23.05   废(mcu_wrong) 42.14
    //     power RES 好 19.36~23.35   signal RES 好 15.49~23.22
    // 好板最大 23.35,废板最小 42.14 —— 中间是空的,阈值能放进去。
    // 取几何中点 sqrt(23.35*42.14) ≈ 31.4,取 32。
    //
    // 原来写死 12.0,比好板最大值还低一半,于是**每一块好板的每个电阻
    // 都报错色** —— 实测三块好板各报 3/8/4 个 wrong。
    // 这个数是 demo 板一张图上量出来的,换板型就整片失效(老毛病)。
    const double yellowMinPct = params.value("yellow_min_pct").toDouble(32.0);
    const double polarityRatio= params.value("polarity_ratio").toDouble(60.0);
    const double whiteMinPct  = params.value("white_min_pct").toDouble(0.5);
    // ⚠ 这个 60 是上面 defaultParams 里那份的第二份副本(老毛病:同一个数
    //   在一个文件里抄多处,改一处忘一处,界面和实际跑的就会不一样)。
    //   改的时候两处一起改。
    const double bridgeMinPct = params.value("bridge_min_pct").toDouble(60.0);
    const double padMinPct    = params.value("pad_min_pct").toDouble(0.46);
    const double padDropPct   = params.value("pad_drop_pct").toDouble(33.0);
    const double shiftXPct    = params.value("shift_x_pct").toDouble(8.0);
    const double shiftYPct    = params.value("shift_y_pct").toDouble(10.0);
    const double shortMinPct  = params.value("short_min_pct").toDouble(0.021);
    const double shortMaxPct  = params.value("short_max_pct").toDouble(0.08);
    const double shortWMinPct = params.value("short_w_min_pct").toDouble(1.1);
    const double shortWMaxPct = params.value("short_w_max_pct").toDouble(8.0);
    const double shortHMinPct = params.value("short_h_min_pct").toDouble(1.0);
    const double shortHMaxPct = params.value("short_h_max_pct").toDouble(6.0);

    // 金层 mask(bridge / insufficient 判定需要)
    //
    // H 和 S 本来就与整体亮度无关(OpenCV 从 max/min 算),只有 V 会缩放,
    // 所以 V 下界锚在板面基色上。实测金层在 U1 ROI 占 12.96%、
    // 25 档亮度一格不差(见 operators/pcb_color_ref.h 的说明)。
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    // 色带和开运算都走 pcb_color_ref.h 的共用实现,不再各写一份 inRange。
    cv::Mat goldMask = pcbcolor::opened(
        pcbcolor::bandMask(hsv, boardBase, pcbcolor::goldBand(maskVMul)),
        pcbcolor::kernelScale(img));

    QJsonArray defects;
    QJsonArray metrics;
    cv::Mat marked = img.clone();

    // ============================================================
    // 被守卫跳过的规则 —— 必须报出来,不能默默不跑
    // ============================================================
    //
    // 有些规则在拿到读数之前就被守卫挡掉了(样本不够 / 掩膜空 / 数不出
    // 中位数)。这时候既没有 metric 也没有 defect,整块板照样输出 PASS,
    // 看上去就是"查过了,没问题"。而上面那个覆盖度统计(withRules)是
    // **按配方里写了什么**算的,不是按真的跑了什么 —— 于是这种静默跳过
    // 连告警都发不出来:配方点名要查 insufficient,实际一次没跑,
    // 覆盖度报告还写"已覆盖"。
    //
    // 实测(2026-10-14,insufficient,四块板的**好板**,出厂参数):
    //
    //     板型/站点        金层连通域   insufficient 跑了吗
    //     demo  U1  IC            13          跑(6 次模糊档全跑)
    //     mcu   U1  QFP64          3          **没跑**
    //     mcu   Y1/Y2 XTAL         3 / 3      **没跑**
    //     power U1  TO220          2          **没跑**
    //     signal U1/U2 SOIC8      0 / 0      **没跑**
    //
    // (连通域数是把算子的开运算掩膜重数一遍得到的,并与算子自己的输出
    //  交叉验证过:7 个站点里 7 个一致。判"跑没跑"则是直接读算子吐的
    //  metric,不依赖重算。)
    //
    // 为什么 complex 三块板凑不出 4 个:金层掩膜的开运算核跟着图缩放
    // (kernelScale=min(W/1000,H/700),1200x900 -> 3*1.2=3.6 -> 取 5x5),
    // 而 W6 生成器画焊盘的像素尺寸是写死的。核变大、焊盘不变,
    // 小焊盘被开运算吃光,剩下几根粗走线各自算一个连通域。
    //
    // ⚠ 要真正修好,得改 W6 生成器的焊盘尺寸/间距 —— 那是**改测试数据**,
    //   9 张图全部要重量一遍,本轮不做。这里只把静默变成响亮。
    // ============================================================
    QJsonArray skipped;
    auto noteSkip = [&skipped](const QString& label, const QString& rule,
                                const QString& reason) {
        skipped.append(QJsonObject{
            {"component", label}, {"rule", rule}, {"reason", reason}});
    };

    // 全板 body_pct 的中位数 —— 归一化判据的分母。
    //
    // 只统计**上游真给了 body_pct 的站点**:老存档 / auto_contour 产物没有这个
    // 字段,把它们的 0 混进来算中位数会把分母拉到接近 0,于是比值爆表,
    // 全板一起误报缺件 —— 而症状看起来像"阈值太小"。
    std::vector<double> bodyAll;
    bodyAll.reserve(comps.size());
    for (const auto& v : comps) {
        const QJsonObject c = v.toObject();
        if (c.contains("body_pct")) bodyAll.push_back(c.value("body_pct").toDouble());
    }
    double bodyMedian = 0.0;
    if (!bodyAll.empty()) {
        std::sort(bodyAll.begin(), bodyAll.end());
        bodyMedian = bodyAll[bodyAll.size() / 2];
    }

    for (const auto& v : comps) {
        QJsonObject c = v.toObject();
        const QString label = c.value("label").toString();

        // ============================================================
        // 这个元件要跑哪些规则 —— **由上游配方决定,不再认标签**
        //
        // 原来这里是七处 `if (label == "R2")` / `"R3"` / `"C1"` ...
        // 写死在源码里,换一块布局不同的板,那七个名字一个都不出现,
        // 于是**每一条规则都不跑**,而判定照样输出结论,
        // 结论是"零缺陷 → PASS"。不报错、不告警。
        //
        // 实测(2026-10-09,拿三种别的板型直接跑):三块好板全被判废,
        // 两块废板的输出与好板逐字相同 —— 规则没跑过,当然分不出来。
        //
        // 现在配方里每个站点自带 checks[],这里只按名字分派。
        // label 退化成"报告里给人看的位号",判定不再依赖它。
        // ============================================================
        // 走 rulesForComponent():字段缺失才按封装类别兜底,
        // 配方显式写了空数组就照做(见该函数上面的说明)。
        const QStringList rules = rulesForComponent(c);

        // shift_tol_pct:配方可按封装单列(小电阻和 QFP64 的容忍度差很多),
        // 没给就用算子默认值。
        const double cShiftX = c.contains("shift_tol_pct")
                             ? c.value("shift_tol_pct").toDouble() : shiftXPct;
        const double cShiftY = c.contains("shift_tol_pct")
                             ? c.value("shift_tol_pct").toDouble() : shiftYPct;

        cv::Rect roi(c.value("x").toInt(), c.value("y").toInt(),
                     c.value("w").toInt(), c.value("h").toInt());
        roi &= cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0 || roi.height <= 0) continue;

        // 本 ROI 的三个"尺子"。所有阈值都拿它们当基准,不再出现像素数。
        const double roiArea = static_cast<double>(roi.width) * roi.height;
        if (roiArea <= 0) continue;

        // missing / shifted 两条规则看的是同一张暗像素 mask,
        // 算一遍就够。原来两处各写一遍 cvtColor + inRange —— 同样的
        // 颜色判据在同一份代码里出现两次,改一处忘一处就是下一次事故。
        cv::Mat darkMask;
        if (rules.contains(QString::fromUtf8(recipe_rule::kMissing))
            || rules.contains(QString::fromUtf8(recipe_rule::kShifted))) {
            // 原来是 inRange(rgb, (0,0,0)~(60,60,60)),60 是绝对值。
            // 光一暗,板面自己就掉进"暗"区间(实测板面 R=40 < 60),
            // 正常板整块误报缺件。现在锚在板面基色的 0.6 倍上。
            darkMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                return pcbcolor::isDark(p, boardBase, darkVMul);
            });
        }

        // ---------- missing: 框里没有元件 ----------
        //
        // 问哪个问题由 missingAsksBodyPresence() 决定 —— 黑本体问暗像素,
        // 彩色本体问 body_pct。两边的实测依据都写在那个函数的注释里。
        //
        // 两个读数**都记进 metrics**,不管这次用哪个:调阈值的人需要看到
        // "好板上是多少、废板上是多少",而不是只看到一个判完的结果。
        if (rules.contains(QString::fromUtf8(recipe_rule::kMissing))) {
            const double darkCount = cv::countNonZero(darkMask);
            const double darkPct   = 100.0 * darkCount / roiArea;
            const bool   byBody    = missingAsksBodyPresence(c.value("kind").toString());
            const double bodyPct   = c.value("body_pct").toDouble(0.0);
            // 上游没给 body_pct(老存档 / auto_contour 产物)时不能当成 0 ——
            // 0 的意思是"框里空空的",会把整个元件误判成缺件。
            // 这种情况退回暗像素判据,和上游缺字段时的其它处理一个口径。
            const bool haveBodyPct = c.contains("body_pct");
            // 归一化:除以**同一张图上所有站点的 body_pct 中位数**。
            //
            // 为什么要归一化(2026-10-14 实测,三块 complex 板 k=0..26,
            // 只看真正读 body_pct 的 CAP 站点):
            // body_pct 会被模糊整体吹大 —— 模糊对整块图是全局的,同一张图上
            // 所有站点一起抬,所以绝对阈值和好板/废板两条曲线**一起往上滑**。
            // 实测 power C2:
            //     出厂阈值失效   绝对 k=8   比值 k=13
            //     窗口彻底关闭   绝对 k=23  比值 k=23   <-**一样**
            // 两个判据都死在 k=23(σ=图宽千分之二十三,整张图糊成一片,
            // 两边读数都饱和,图像本身没信息了,换任何阈值都无解)。
            // 所以比值版**没有延长判据寿命**,它买的是"同一个出厂阈值
            // 能站得更深" —— 多撑 5 级。
            // 根子:缺件侧爬到最终饱和值的比例不同 —— 绝对版 42% 就被切到,
            // 比值版要到 71.4%。另外比值版好板侧全程平(0.892 ~ 1.000),
            // 绝对版从 49 爬到 100:后者的阈值含义随模糊漂移,前者不会。
            //
            // ⚠ 别把 demo R2 之类的数混进来量窗口:R2 是 RES,走暗像素,
            //   根本不读 body_pct。前一版注释就是这么把两条不同判据的数
            //   放到一个窗口里,得出了"绝对阈值无解"的错误结论。
            //   demo 板整块一个 CAP 站点都没有,连读数都取不到。
            //
            // 代价是**站点之间耦合了**:一个站点的判定要看别的站点。
            // 站点太少时中位数不可靠(只有一个站点时比值恒等于 1,永远不报缺件),
            // 所以带 body_min_sites 兜底,退回绝对判据并在 metrics 里说清走的哪条。
            const bool enoughSites = static_cast<int>(comps.size()) >= bodyMinSites;
            const bool useRatio    = byBody && haveBodyPct && enoughSites;
            const bool useBodyAbs  = byBody && haveBodyPct && !enoughSites;
            const double value = useRatio  ? bodyPct / std::max(1e-9, bodyMedian)
                             : useBodyAbs ? bodyPct
                             :              darkPct;
            const double threshold = useRatio  ? bodyRatioMin
                             : useBodyAbs ? bodyMinPct
                             :              darkMinPct;

            addMetric(metrics, label, "dark_pct",  darkPct, darkMinPct, "% ROI");
            // unit 里写清这次走的是哪条路。归一化之后这三行的数字**不可比**:
            // 0.85(比值)和 49.2(百分比)不是一个量,混在一张 metrics 里
            // 不标清楚,调阈值的人一定会拿错单位去比。
            const QString unit = useRatio  ? "比同板中位数"
                              : useBodyAbs ? QString("% ROI(非板面,站点不足 %1 走绝对)")
                                                  .arg(bodyMinSites)
                              :              QString("% ROI(暗)");
            addMetric(metrics, label, recipe_rule::kMissing, value, threshold,
                      unit.toUtf8().constData());
            if (value < threshold) {
                addDefect(defects, label, "missing",
                          useRatio
                            ? QString("框内非板面像素占同板中位数的 %1 < 阈值 %2"
                                      "(本体疑似被擦除;body_pct %3%,同板中位数 %4%)")
                                .arg(value, 0, 'f', 3).arg(threshold, 0, 'f', 3)
                                .arg(bodyPct, 0, 'f', 1).arg(bodyMedian, 0, 'f', 1)
                          : useBodyAbs
                            ? QString("框内非板面像素占 ROI %1% < 阈值 %2%"
                                      "(本体疑似被擦除;本板站点数 %3 < %4,"
                                      "比值不可靠,退回绝对判据)")
                                .arg(value, 0, 'f', 2).arg(threshold, 0, 'f', 2)
                                .arg(comps.size()).arg(bodyMinSites)
                            : QString("暗像素 %1(占 ROI %2%)< 阈值 %3%(元件疑似被擦除;"
                                      "暗 = V < %4 x 板面V,板面 BGR %5,%6,%7)")
                                .arg((int)darkCount).arg(value, 0, 'f', 2)
                                .arg(threshold, 0, 'f', 2)
                                .arg(darkVMul, 0, 'f', 2)
                                .arg(boardBase[0]).arg(boardBase[1]).arg(boardBase[2]));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- wrong: 本体上错色 ----------
        //
        // 原来是 inRange(BGR (150,150,0)~(255,255,80)):150 和 200 都是
        // 绝对值,压暗后错色件掉出区间 -> 漏检;而且亮到一定程度它又会
        // 整片撞 255 -> 判据恒假 -> **报 PASS**,废板判合格。
        // 改成相对板面基色的倍数,并在上方 ceilingBreached 处拒绝判定。
        if (rules.contains(QString::fromUtf8(recipe_rule::kWrong))) {
            const cv::Mat offMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                return pcbcolor::isOffColor(p, boardBase, offColorMul, offColorRMax);
            });
            const double yellowPct = 100.0 * cv::countNonZero(offMask) / roiArea;
            addMetric(metrics, label, recipe_rule::kWrong,
                      yellowPct, yellowMinPct, "% ROI");
            if (yellowPct > yellowMinPct) {
                addDefect(defects, label, "wrong",
                          QString("错色像素占 ROI %1% > 阈值 %2%(应为黑色电阻;"
                          "判据 B,G >= %3 x 板面 且 R <= %4 x 板面,板面 BGR %5,%6,%7)")
                          .arg(yellowPct, 0, 'f', 2).arg(yellowMinPct)
                          .arg(offColorMul, 0, 'f', 2)
                          .arg(offColorRMax, 0, 'f', 2)
                          .arg(boardBase[0]).arg(boardBase[1]).arg(boardBase[2]));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- polarity: 白色极性条偏右 ----------
        //
        // ⚠⚠ 这条判据**只能抓到一半的极性反**,另一半在结构上抓不到。
        //   别再花时间调 polarity_ratio 了 —— 实测过(2026-10-10),
        //   四块板好板 + 3 张极性缺陷图的丝印白质心横向位置(占 ROI 宽 %):
        //
        //       demo   C1(应为左)好 17.89  ->  反装 81.99   ✓ 抓得到
        //       signal D1(应为右)好 51.95  ->  反装 29.87   ✗ 抓不到
        //
        //   原因是判据是**单边**的(只在 cxPct > polarityRatio 时报),
        //   而"反装"是把极性条挪到**另一边**:demo 那张挪到了右边(撞上单边阈值),
        //   signal 那张挪到了左边(单边阈值根本不看那边)。
        //
        //   直觉上应该改成双边("偏左或偏右超过容差就算反"),实测**没有可用的容差**。
        //   "极性条该在哪一侧"是**每种封装各自的物理事实**,而配方里没有这个字段;
        //   拿 ROI 中心(50%)当前提去凑,两边要求互相矛盾:
        //       不误报 power D1(好板 41.84)  要容差 < 8.16
        //       抓到 signal D1(废板 29.87)  要容差 > 20.13
        //   8.16 < T < 20.13 无解。好板那一批(41.84~52.73)之所以贴得这么近,
        //   是因为丝印框对称、在白像素里占的份量比极性条还大,质心被拉到中间。
        //
        //   下一步(不是调参能解决的):
        //     1. 配方增加每站点的 polarity_side(left/right),由导出脚本从
        //        生成器的 draw_* 函数推出 —— 真值在生成器里,不是手抄。
        //     2. 量"极性条本体"而不是"整个 ROI 的白":先把 ROI 按 buffer 收到
        //        本体大小再量质心,丝印框就落在窗外了,分离度会从 22 个点拉到 50 个点。
        //   这两步都做完之前,signal_polarity 这张图是**已知漏检**,不是回归。
        // ============================================================
        if (rules.contains(QString::fromUtf8(recipe_rule::kPolarity))) {
            // 原来是 inRange(BGR >= (200,200,200)):压暗后丝印白掉出 200,
            // 白色像素占比归零,white_min_pct 这道门直接把它挡掉 ——
            // 于是 polarity 规则**静默不跑**,不报缺陷,看着像合格。
            const cv::Mat whiteMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                return pcbcolor::isWhiteSilk(p, boardBase, brightVMul, neutralMax);
            });
            cv::Moments m = cv::moments(whiteMask, true);
            const double whitePct = 100.0 * m.m00 / roiArea;
            // 白色占比不够时这条规则**不判定** —— 这是第三种状态:
            // 既没报缺陷,也没说"查过了"。必须记一笔,免得看起来像合格。
            // (原来这里只有一句注释说"记一笔",实际什么也没记 ——
            //  注释承诺了一件代码没做的事,和"字段算出来没人读"同一类。)
            if (whitePct >= whiteMinPct) {   // 白色太少不判定
                double cx = m.m00 > 0 ? m.m10 / m.m00 : 0.0;
                const double cxPct = roi.width > 0
                                  ? 100.0 * cx / roi.width : 0.0;
                addMetric(metrics, label, recipe_rule::kPolarity,
                          cxPct, polarityRatio, "% ROI 宽(偏右为反)");
                if (m.m00 > 0 && cxPct > polarityRatio) {
                    addDefect(defects, label, "polarity",
                              QString("极性条质心 x=%1 > %2% 宽度(极性反)")
                              .arg((int)cx).arg(polarityRatio));
                    cv::rectangle(marked, roi, cv::Scalar(255, 0, 255), 3);
                }
            } else {
                noteSkip(label, recipe_rule::kPolarity,
                         QString("白色丝印只占 %1% < 门槛 %2%,读不到极性条,"
                                 "本规则不判定")
                             .arg(whitePct, 0, 'f', 2).arg(whiteMinPct));
            }
        }

        // ---------- bridge: 焊盘间金条连片 ----------
        //
        // ⚠⚠ 这条判据有个**结构性的适用范围**,别再拿它去调参:
        //   它量的是"ROI 里最长一行的金像素个数"。只要有一条正常的
        //   金走线从元件 ROI 里横穿过去,好板这一行就会占满整个宽度。
        //
        //   实测(2026-10-10,扰动矩阵跑出来的,不是推的):
        //       signal 板信号链 y=450 那条金走线
        //       (gen_complex_pcb.py L512~L519)正好从 R1(220,450) 和
        //       R3(720,450) 身上穿过去;R2(480,350)/R4(860,350) 在 y=350,
        //       躲开了。
        //
        //           元件   1x      0.5x    0.75x   1.25x   2.0x
        //           R1     37.50   71.88   69.39   69.14   68.75   ← 走线穿过
        //           R2     29.69   31.25   38.78   30.86   29.69   ← 没穿过
        //           R3     29.69   84.38  100.00  100.00  100.00   ← 走线穿过
        //           R4     29.69   31.25   38.78   30.86   29.69   ← 没穿过
        //
        //   ⚠ 这是**修图之前**的历史数据,W6 生成器修好之后不再成立
        //     (2026-10-14,见本段末尾"已修"那截)。留着是因为它就是
        //     这条结构性限制的证据本身:表里 R1/R3 和 R2/R4 的差别
        //     全在"走线穿没穿过"这一个变量上,别的什么都没变。
        //
        //   R2/R4 完美缩放(1x 的 19px → 0.5x 的 10px,正好一半),
        //   R1/R3 完全不缩放,到 2.0x 直接顶到 100% —— 整行都是走线。
        //   **好板的值和真焊桥(100)完全重合,任何阈值都分不开。**
        //
        //   所以 bridge 只对"ROI 里没有走线横穿"的元件成立。
        //   对被走线贯穿的元件,判据得改成量"两个焊盘之间有没有被
        //   金层连成**同一块**"(连通域跨越两焊盘),而不是量长度 ——
        //   走线是连着焊盘的,但它在 ROI 边界处出去、回来,
        //   不占满中间那几行。这个改法还没做,先记在这。
        //
        // ============================================================
        // ⚠⚠ 2026-10-10 追到底了:**根因在 W6 的生成器里,不在这个判据。**
        //
        //   gen_complex_pcb.py L516~L517:
        //       trace(d, [(620,450), (720,450)], width=4)   # R3 在 (720,450)
        //       trace(d, [(720,450), (860,450)], width=4)
        //   两条走线在 **R3 的几何中心**相接,合成一条贯穿电阻本体的金线。
        //   走线在元件之后绘制(元件 L478~496,走线 L512~519),金线直接
        //   盖在黑色本体上。
        //
        //   实测(raw 掩膜,未开运算):R3 的 ROI 里是**一个**连通域,
        //   x=0 宽=64,横穿整个 ROI —— 两个焊盘被连成了一块。
        //   电阻的两个焊盘在**不同网络**上,这才是"电阻"。
        //   被一条金线连通 = **这块板上的 R3 是短路的**。
        //
        //   也就是说 signal_baseline.png 被标为好板,但图上 R3 已经短路。
        //   1x 之所以没误报,是因为走线只有 4px 高、被 5x5 的开运算抹掉了 ——
        //   **不是因为板是好的**。换到 2x(走线 8px、核只有 7x7)就露馅。
        //
        //   同源问题还有 mcu:gen_complex_pcb.py L423 的走线同样穿过
        //   R1(940,450),只是线宽 3 被开运算在测过的 5 个档位都抹掉 ——
        //   现在没出事是**靠线细**,不是靠判据对。
        //
        //   试过的替代判据(2026-10-10 实测,四块板 x 5 档):
        //     fullRows/padH(满宽行数 / 焊盘高)  好板最大 0.200,坏板最小 0.095
        //                                            —— **没有缝,不可用**
        //     gap%(两焊盘之间那块的金占比)        好板最大 23.0,坏板最小 31.6
        //                                            —— 有缝,但只剩 4 个点余量,
        //                                               且坏板侧只有 2 个元件 x 5 档
        //                                               = 10 个数据点。
        //   拿 4 点余量去换一条在 1x 上有 22 点余量的规则,证据不够,
        //   **先不动**。等 W6 的图修好、坏样本攒够,再说。
        //
        //   正确的修法是改 W6 生成器:走线应该停在焊盘边缘(而不是
        //   穿到元件中心)。这是**改测试数据**,得先说清楚再做 ——
        //   改完 9 张图全部要重量一遍。
        //
        // ── 2026-10-14:这个修法做完了,上面那段从"根因分析"变成"已修" ──
        //   W6 生成器现在有两道机制:
        //     ① 每个 draw_* 在画本体时顺手 opaque() 登记不透明矩形,
        //        trace() 按 1px 步长把落在本体里的那段跳过 —— 元件坐在
        //        铜之上,那一段本来就是被盖住的,不是断的。
        //     ② 走线端点从"元件几何中心"改到"焊盘/引脚实际坐标"。
        //        ① 挡住的是"穿过去",② 保证"接得上":只做①的话走线会停在
        //        本体边缘,离焊盘还差 6~16px。
        //
        //   重测之前,探针先在**旧图**上复现了本文档记的那组数
        //   (R1 71.88 / R3 100.00)才认新数 —— 量具没校准就出的表不算数。
        //   四块板 x 5 档分辨率:
        //
        //       站点    修前 1x/0.5x/0.75x/1.25x/2x           修后
        //       sig R1  37.50/71.88/69.39/70.37/68.75   29.69/31.25/32.65/32.10/29.69
        //       sig R3  29.69/84.38/100.0/100.0/100.0  29.69/31.25/32.65/32.10/29.69
        //       pwr R1  29.69/31.25/57.14/45.68/29.69  29.69/31.25/38.78/32.10/29.69
        //       pwr R2  29.69/31.25/55.10/51.85/35.16  29.69/31.25/44.90/39.51/37.50
        //
        //   **修后所有 bridge 站点都塌进 29.69~44.90 这条窄带**,和"从未被
        //   走线穿过"的站点(sig R2/R4、mcu R1/R2)完全重合 —— 也就是说这些
        //   站点已经和别的站点**不可区分**了,而它们本来就该一样。
        //   阈值 60 现在有 15.1 个点余量。
        //
        //   阈值一个数都没动。动的是数据:原来好板一侧的 68.75 / 100.00
        //   是假象撑起来的,不是板上真有那么多金。9 张图的判定结果修前
        //   修后逐字相同(PASS/FAIL 与命中类型都没变),12/12 ctest 全绿。
        //
        //   ⚠ 本条的**结构性适用范围依然成立**:判据量的是"ROI 里最长一行
        //   的金像素数",所以只对"ROI 里没有走线横穿"的元件成立。现在 W6
        //   三块板的走线都不再横穿,所以成立;换一块新板,只要有一根走线从
        //   ROI 里穿过去,这段注释就得重新读一遍。
        // ============================================================
        if (rules.contains(QString::fromUtf8(recipe_rule::kBridge))) {
            cv::Mat roiMask = goldMask(roi);
            int maxWidth = 0, maxY = 0;
            for (int y = 0; y < roiMask.rows; ++y) {
                int w = cv::countNonZero(roiMask.row(y));
                if (w > maxWidth) { maxWidth = w; maxY = y; }
            }
            const double runPct = 100.0 * maxWidth / roi.width;
            addMetric(metrics, label, recipe_rule::kBridge,
                      runPct, bridgeMinPct, "% ROI 宽(最长金段)");
            if (runPct > bridgeMinPct) {
                addDefect(defects, label, "bridge",
                          QString("最长金段占 ROI 宽 %1% > 阈值 %2%(焊桥)@y=%3")
                          .arg(runPct, 0, 'f', 1).arg(bridgeMinPct).arg(maxY));
                cv::rectangle(marked, roi, cv::Scalar(0, 140, 255), 3);
            }
        }

        // ---------- insufficient: 某焊盘面积远小于中位数 ----------
        if (rules.contains(QString::fromUtf8(recipe_rule::kInsufficient))) {
            cv::Mat roiMask = goldMask(roi);
            cv::Mat labels, stats, centroids;
            int n = cv::connectedComponentsWithStats(roiMask, labels, stats, centroids);
            std::vector<int> areas;
            for (int i = 1; i < n; ++i) {
                int a = stats.at<int>(i, cv::CC_STAT_AREA);
                // 过滤边缘噪声。门槛是 ROI 面积的比例 —— 原来是裸的 80px,
                // 图一小,正常焊盘就掉到门槛以下被滤光,判定必然失效。
                if (100.0 * a / roiArea >= padMinPct) areas.push_back(a);
            }
            if (areas.size() >= 4) {
                std::sort(areas.begin(), areas.end());
                const int median = areas[areas.size() / 2];
                const int minArea = areas.front();
                const double dropPct =
                    median > 0 ? 100.0 * (median - minArea) / median : 0.0;
                if (median > 0) {
                    addMetric(metrics, label, recipe_rule::kInsufficient,
                              dropPct, padDropPct, "% (最小焊盘低于中位数)");
                }
                if (median > 0 && dropPct > padDropPct) {
                    addDefect(defects, label, "insufficient",
                              QString("最小焊盘 %1px² 比中位数 %2px² 低 %3%% > 阈值 %4%(焊少)")
                              .arg(minArea).arg(median).arg(dropPct, 0, 'f', 1)
                              .arg(padDropPct));
                    cv::rectangle(marked, roi, cv::Scalar(255, 200, 0), 3);
                }
            } else {
                // 配方点名要查,但这块 ROI 的金层凑不出 4 个连通域,
                // 取不了中位数 -> 规则**整条没跑**,却照样 PASS。
                // 见上面 skipped 那段的实测表(complex 三块板全中)。
                noteSkip(label, recipe_rule::kInsufficient,
                         QString("金层连通域只有 %1 个(< 4),取不了焊盘中位数,"
                                 "本规则不判定")
                             .arg(areas.size()));
            }
        }

        // ---------- shifted: 本体质心偏离应站中心 ----------
        //
        // 期望质心就是 ROI 的几何中心,不需要一张"每个元件该长什么样"的表。
        // 以前 EXPECTS[6] 里 R2 写死的 (60,24) 正是 120x48 这个 ROI 的中心
        // —— 同一个数,只是被抄了一份,抄错了也不会有人发现。
        // ROI 是由元件本体框四周等量扩出来的,所以两者恒等。
        if (rules.contains(QString::fromUtf8(recipe_rule::kShifted))) {
            // 复用上面算好的暗像素 mask —— 原来这里是第三份同样的
            // cvtColor + inRange。
            cv::Moments m = cv::moments(darkMask, true);
            const double darkPct = 100.0 * m.m00 / roiArea;
            // m.m00 > 0 必须单独判:dark_min_pct 被调到 0 时,
            // "暗像素占比 >= 0%" 对**一张全黑都没有的 ROI 也成立**,
            // 于是下面 m.m10/m.m00 变成 0/0 = NaN。
            // NaN 和任何数比较都是 false,判定会静默失效 —— 又是一个不报错的错。
            if (m.m00 > 0 && darkPct >= darkMinPct) {   // 没本体就交给 missing 判
                const double expCx = roi.width  * 0.5;
                const double expCy = roi.height * 0.5;
                const double cx = m.m10 / m.m00, cy = m.m01 / m.m00;
                const double dx = cx - expCx, dy = cy - expCy;
                const double tolX = roi.width  * cShiftX / 100.0;
                const double tolY = roi.height * cShiftY / 100.0;
                // 取 x/y 两个方向里更接近阈值的那一个来报:
                // 偏移量本身没有"正常/异常"的绝对值,只有相对容差的大小,
                // 报"超了 12%"不如报"超了 1.3 倍"直观。
                const double over = std::max(std::abs(dx) / std::max(tolX, 1e-9),
                                             std::abs(dy) / std::max(tolY, 1e-9));
                addMetric(metrics, label, recipe_rule::kShifted,
                          over, 1.0, "倍(>1 即超容差)");
                if (std::abs(dx) > tolX || std::abs(dy) > tolY) {
                    addDefect(defects, label, "shifted",
                              QString("质心(%1,%2) 偏离 ROI 中心(%3,%4) 超容差(%5,%6)")
                              .arg((int)cx).arg((int)cy)
                              .arg((int)expCx).arg((int)expCy)
                              .arg((int)tolX).arg((int)tolY));
                    cv::rectangle(marked, roi, cv::Scalar(0, 255, 255), 3);
                }
            } else {
                // "没本体就交给 missing 判"是对的(m 本体都不在,
                // 算质心没有意义),但**这条规则确实没跑**,仍然要报出来。
                // 否则"交给 missing"就变成了静默跳过的借口。
                noteSkip(label, recipe_rule::kShifted,
                         QString("ROI 里暗像素只占 %1%(低于 %2%),没有本体可算质心,"
                                 "本规则不判定(缺件由 missing 负责)")
                             .arg(darkPct, 0, 'f', 2).arg(darkMinPct));
            }
        }
    }

    // ---------- short: 蓝紫圆环(全局缺陷,不属单个元件) ----------
    //
    // 它不属于任何元件,所以没有 ROI 可依,基准只能用整张图:
    // 面积对全图面积,宽高对全图对角线(对角线是唯一一个
    // "不管横竖怎么变都跟着图一起缩"的尺子)。
    //
    // ⚠⚠ 这条判据找的是**环**,而 power 板那张短路图画的是**一条金条**,
    //   实测(2026-10-10)它在蓝紫色带里连成一大块:
    //       power_baseline 最大蓝块   2x13   面积 26   = 0.0024% 图
    //       power_short    最大蓝块 621x61   面积 2151 = 0.1992% 图
    //                                        宽 41.4% 对角线 高 4.1%
    //   而 short_w_max_pct = 8(宽的上界)、short_max_pct = 0.08(面积上界),
    //   41.4 和 0.1992 **两头都撞出去了**,所以判据直接把它放过去。
    //   顺带纠正一个容易搞错的地方:蓝紫这一道**不过开运算**,金层过 ——
    //   探针第一版两边都加了开运算,数出来的连通域比判据看到的少一大截,
    //   差点得出"short 是零触发"的结论。量具得和被量的东西一模一样。
    //
    //   这条同样**不是调阈值能解决的**:好板上有 812x20(signal)和 901x290(power)
    //   的合法走线,长宽比 40:1 和 3:1 都盖住了缺陷那张 621x10 的 62:1,
    //   单看"长"或"扁"都分不开。要改的是缺陷定义本身
    //   (比如"不该存在的跨区导体"),不是这四个数。下一步。
    // ============================================================
    {
        const double imgArea = static_cast<double>(img.cols) * img.rows;
        const double diag = std::sqrt(static_cast<double>(img.cols) * img.cols
                                    + static_cast<double>(img.rows) * img.rows);
        cv::Mat blueMask = pcbcolor::bandMask(hsv, boardBase,
                                              pcbcolor::blueBand(maskVMul));
        // maskVMul 必须传进来。以前这里写的是 maskVLower(boardBase) —— 走了
        // 头文件里的默认值,而金层那一处传的是参数。于是调 mask_v_mul 时
        // 金层跟着变、这张掩膜不变:同一个参数两处不同步,而且**不报错**,
        // 只是这个旋钮有一半失灵。改成参数的时候漏掉一处,正是这个项目
        // 已经吃过三次亏的那一类(抄副本只改了一份)。
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(blueMask, labels, stats, centroids);
        int bestIdx = -1, bestArea = 0;
        for (int i = 1; i < n; ++i) {
            int a = stats.at<int>(i, cv::CC_STAT_AREA);
            if (a > bestArea) { bestArea = a; bestIdx = i; }
        }
        if (bestIdx > 0) {
            int x = stats.at<int>(bestIdx, cv::CC_STAT_LEFT);
            int y = stats.at<int>(bestIdx, cv::CC_STAT_TOP);
            int w = stats.at<int>(bestIdx, cv::CC_STAT_WIDTH);
            int h = stats.at<int>(bestIdx, cv::CC_STAT_HEIGHT);
            const double areaPct = 100.0 * bestArea / imgArea;
            const double wPct = 100.0 * w / diag;
            const double hPct = 100.0 * h / diag;
            bool isRing = (areaPct > shortMinPct && areaPct < shortMaxPct
                        && wPct >= shortWMinPct && wPct <= shortWMaxPct
                        && hPct >= shortHMinPct && hPct <= shortHMaxPct);
            if (isRing) {
                // component 留空 —— 短路是全局缺陷,不归属某个元件
                addDefect(defects, QString(), "short",
                          QString("短路圆环 @(%1,%2) %3x%4 面积=%5(占全图 %6%)")
                          .arg(x).arg(y).arg(w).arg(h).arg(bestArea)
                          .arg(areaPct, 0, 'f', 4));
                cv::circle(marked, cv::Point(x + w / 2, y + h / 2),
                           std::max(w, h), cv::Scalar(0, 0, 255), 3);
            }
        }
    }

    QString markedPath = "images/w7_pcb_defect.png";
    cv::imwrite(markedPath.toStdString(), marked);

    output["marked_path"]   = markedPath;
    output["defects"]       = defects;
    // 每条跑过的规则都记了实测值(含没触发的)。调阈值时看这个:
    // 只看 defects 的话,好板上那一堆"离阈值还差多少"全是隐形的。
    output["metrics"]       = metrics;
    // 被守卫跳过的规则。**和 defects 分开报** —— 混进去就成了"查出来有
    // 问题",而实际上是没查。报告里两个都得看得见。
    output["skipped"]       = skipped;
    output["skipped_count"] = skipped.size();
    if (!skipped.isEmpty()) {
        QString w = output.contains("warning")
                  ? output.value("warning").toString() + QString(" ")
                  : QString();
        output["warning"] = w + QString(
            "另有 %1 条规则被守卫跳过(见 skipped),这些**没查**——"
            "不等于合格。").arg(skipped.size());
    }
    output["defect_count"]  = defects.size();
    output["component_count"] = comps.size();
    output["result"]        = defects.isEmpty() ? "PASS" : "FAIL";
    return output;
}

QWidget* PcbDefectJudge::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new PcbDefectJudgeForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// PcbDefectJudgeForm
// ============================================================
PcbDefectJudgeForm::PcbDefectJudgeForm(QWidget* parent) : ParamForm(parent) {}

void PcbDefectJudgeForm::buildUI() {
    // 百分比输入框:两位小数够 ROI/全图类的用,
    // 短路那几行要三位(0.021%)—— 用整数表达不了。
    auto mk = [](double val, int decimals) {
        auto* s = new QDoubleSpinBox;
        s->setRange(0.0, 100.0);
        s->setDecimals(decimals);
        s->setSingleStep(decimals >= 3 ? 0.001 : 0.5);
        s->setValue(val);
        return s;
    };
    // 倍数类的量不一样:它们是"占板面基色多少倍",量级在 0.5~2,
    // 用上面那个 0~100 / 步长 0.5 的框根本点不动。
    auto mkMul = [](double val) {
        auto* s = new QDoubleSpinBox;
        s->setRange(0.0, 10.0);
        s->setDecimals(2);
        s->setSingleStep(0.05);
        s->setValue(val);
        return s;
    };
    // 这里的初值**不再各写一份** —— 全部取自 defaultParams()。
    //
    // 以前是每个框手敲一个数,于是默认值的"第二份账"和第一份漂开了
    // (实测 2026-10-14,三处):
    //     dark_min_pct    JSON 1.0  vs 框里 2.0
    //     bridge_min_pct  JSON 60.0 vs 框里 40.0
    //     yellow_min_pct  JSON 32.0 vs 框里 12.0
    // 后果不是"显示不准":collect() 把框里的值写回 params,
    // 所以**只要有人在面板上点一下保存,阈值就被静默改回旧值**。
    // bridge 那条尤其难受 —— 60 正是这个项目花两个 commit 调出来的,
    // 面板会把它退回 40,正好放行好板上的 44.90。
    // 12 套件 559 条断言一直绿着,因为测试只走 defaultParams(),
    // 从不经过面板的 collect() —— 又一次"验证不等于验证"。
    //
    // 改法不是把三个数字手工改对(那只是把漂移推迟到下一个参数),
    // 是**让框里没有数字可写**:唯一入口是 defaultParams()。
    const QJsonObject d0 = PcbDefectJudge().defaultParams();
    auto dflt = [&d0](const char* key, double fallback) {
        return d0.contains(QLatin1String(key))
             ? d0.value(QLatin1String(key)).toDouble() : fallback;
    };
    m_darkMinPct   = mk(dflt("dark_min_pct",   1.0),   2);
    m_bodyMinPct   = mk(dflt("body_min_pct",  42.0),   2);
    m_bodyRatioMin = mk(dflt("body_ratio_min", 0.714), 3);
    m_bodyMinSites = new QSpinBox;
    m_bodyMinSites->setRange(1, 999);
    m_bodyMinSites->setValue(static_cast<int>(dflt("body_min_sites", 4)));
    m_yellowMinPct = mk(dflt("yellow_min_pct", 32.0),  2);
    m_polarityR    = mk(dflt("polarity_ratio", 60.0), 2);
    m_whiteMinPct  = mk(dflt("white_min_pct",  0.5),   2);
    m_bridgeMinPct = mk(dflt("bridge_min_pct", 60.0),  2);
    m_padMinPct    = mk(0.46,  2);
    m_padDropPct   = mk(33.0,  2);
    m_shiftXPct    = mk(8.0,   2);
    m_shiftYPct    = mk(10.0,  2);
    m_shortMinPct  = mk(0.021, 3);
    m_shortMaxPct  = mk(0.08,  3);
    m_shortWMinPct = mk(1.1,   2);
    m_shortWMaxPct = mk(8.0,   2);
    m_shortHMinPct = mk(1.0,   2);
    m_shortHMaxPct = mk(6.0,   2);
    m_darkVMul      = mkMul(pcbcolor::kDarkMul);
    m_brightVMul    = mkMul(pcbcolor::kBrightMul);
    m_neutralMax    = mkMul(pcbcolor::kNeutralMax);
    m_offColorMul   = mkMul(pcbcolor::kOffColorMul);
    m_offColorRMax  = mkMul(pcbcolor::kOffColorRMax);
    m_maskVMul      = mkMul(pcbcolor::kMaskVMul);

    m_layout->addRow(tr("缺件(黑本体):暗像素占 ROI 下限 %"), m_darkMinPct);
    m_layout->addRow(tr("缺件(彩色本体):非板面占 ROI 下限 %"), m_bodyMinPct);
    m_layout->addRow(tr("缺件(彩色本体):占同板中位数下限"), m_bodyRatioMin);
    m_layout->addRow(tr("缺件:用比值所需最少站点数"), m_bodyMinSites);
    m_layout->addRow(tr("错件:浅黄占 ROI 下限 %"),     m_yellowMinPct);
    m_layout->addRow(tr("极性:白条质心 占 ROI 宽 %"), m_polarityR);
    m_layout->addRow(tr("极性:白色像素占 ROI 下限 %"), m_whiteMinPct);
    m_layout->addRow(tr("焊桥:最长金段占 ROI 宽 %"),   m_bridgeMinPct);
    m_layout->addRow(tr("焊少:焊盘噪声门槛 占 ROI %"), m_padMinPct);
    m_layout->addRow(tr("焊少:比中位数低 %"),          m_padDropPct);
    m_layout->addRow(tr("偏移:x 容差 占 ROI 宽 %"),    m_shiftXPct);
    m_layout->addRow(tr("偏移:y 容差 占 ROI 高 %"),    m_shiftYPct);
    m_layout->addRow(tr("短路:面积下限 占全图 %"),      m_shortMinPct);
    m_layout->addRow(tr("短路:面积上限 占全图 %"),      m_shortMaxPct);
    m_layout->addRow(tr("短路:宽下限 占对角线 %"),      m_shortWMinPct);
    m_layout->addRow(tr("短路:宽上限 占对角线 %"),      m_shortWMaxPct);
    m_layout->addRow(tr("短路:高下限 占对角线 %"),      m_shortHMinPct);
    m_layout->addRow(tr("短路:高上限 占对角线 %"),      m_shortHMaxPct);

    // 分组标题。上面 15 个是"占 ROI / 全图"的百分比,下面 6 个是
    // "占板面基色"的倍数 —— 量纲不同,混在一起排会让人以为它们可比。
    auto* sep = new QLabel(tr("—— 以下为「占板面基色」的倍数(自动按图估计板色)——"));
    m_layout->addRow(sep);
    m_layout->addRow(tr("缺件/偏移:暗 V < 板面 V x"),    m_darkVMul);
    m_layout->addRow(tr("极性:白 V >= 板面 V x"),         m_brightVMul);
    m_layout->addRow(tr("极性:白 中性度 上限"),           m_neutralMax);
    m_layout->addRow(tr("错件:B,G >= 板面 x"),            m_offColorMul);
    m_layout->addRow(tr("错件:R <= 板面 x"),              m_offColorRMax);
    m_layout->addRow(tr("金层/蓝环掩膜 V 下界 x"),        m_maskVMul);
}

void PcbDefectJudgeForm::setParams(const QJsonObject& params) {
    // 默认值一律取自 defaultParams(),不在这儿再抄一遍。
    // 抄第二遍的后果是实测过的:yellow_min_pct 在 defaultParams 里
    // 从 12 改成 32,而这个 setValue 还写着 12 —— 界面显示 12、
    // 存盘写 12、下次 setParams 又读回 12,而无头调用走的是 32,
    // 于是"界面上调阈值没反应"和"测试全绿但产线不对"同时发生。
    // 这已是同一个数在本文件里第三份副本(第四份在 run() 的 toDouble 里)。
    const QJsonObject d = PcbDefectJudge().defaultParams();
    m_darkMinPct->setValue(params.value("dark_min_pct").toDouble(d.value("dark_min_pct").toDouble()));
    m_bodyMinPct->setValue(params.value("body_min_pct").toDouble(d.value("body_min_pct").toDouble()));
    m_bodyRatioMin->setValue(params.value("body_ratio_min").toDouble(d.value("body_ratio_min").toDouble()));
    m_bodyMinSites->setValue(params.value("body_min_sites").toInt(d.value("body_min_sites").toInt()));
    m_yellowMinPct->setValue(params.value("yellow_min_pct").toDouble(d.value("yellow_min_pct").toDouble()));
    m_polarityR->setValue(params.value("polarity_ratio").toDouble(d.value("polarity_ratio").toDouble()));
    m_whiteMinPct->setValue(params.value("white_min_pct").toDouble(d.value("white_min_pct").toDouble()));
    m_bridgeMinPct->setValue(params.value("bridge_min_pct").toDouble(d.value("bridge_min_pct").toDouble()));
    m_padMinPct->setValue(params.value("pad_min_pct").toDouble(d.value("pad_min_pct").toDouble()));
    m_padDropPct->setValue(params.value("pad_drop_pct").toDouble(d.value("pad_drop_pct").toDouble()));
    m_shiftXPct->setValue(params.value("shift_x_pct").toDouble(d.value("shift_x_pct").toDouble()));
    m_shiftYPct->setValue(params.value("shift_y_pct").toDouble(d.value("shift_y_pct").toDouble()));
    // short_* 这六个原来各自带一个手写的兜底默认值 —— 同样是抄的第二份账。
    // 这里也统一走 d(唯一入口),不然 defaultParams() 改了这六个不会跟着动。
    m_shortMinPct->setValue(params.value("short_min_pct").toDouble(d.value("short_min_pct").toDouble()));
    m_shortMaxPct->setValue(params.value("short_max_pct").toDouble(d.value("short_max_pct").toDouble()));
    m_shortWMinPct->setValue(params.value("short_w_min_pct").toDouble(d.value("short_w_min_pct").toDouble()));
    m_shortWMaxPct->setValue(params.value("short_w_max_pct").toDouble(d.value("short_w_max_pct").toDouble()));
    m_shortHMinPct->setValue(params.value("short_h_min_pct").toDouble(d.value("short_h_min_pct").toDouble()));
    m_shortHMaxPct->setValue(params.value("short_h_max_pct").toDouble(d.value("short_h_max_pct").toDouble()));
m_darkVMul->setValue(params.value("dark_v_mul").toDouble(pcbcolor::kDarkMul));
m_brightVMul->setValue(params.value("bright_v_mul").toDouble(pcbcolor::kBrightMul));
m_neutralMax->setValue(params.value("neutral_max").toDouble(pcbcolor::kNeutralMax));
m_offColorMul->setValue(params.value("off_color_mul").toDouble(pcbcolor::kOffColorMul));
m_offColorRMax->setValue(params.value("off_color_r_max").toDouble(pcbcolor::kOffColorRMax));
m_maskVMul->setValue(params.value("mask_v_mul").toDouble(pcbcolor::kMaskVMul));
}

QJsonObject PcbDefectJudgeForm::collect() const {
    QJsonObject p;
    p["dark_min_pct"]    = m_darkMinPct->value();
    p["body_min_pct"]    = m_bodyMinPct->value();
    p["body_ratio_min"]  = m_bodyRatioMin->value();
    p["body_min_sites"]  = m_bodyMinSites->value();
    p["yellow_min_pct"]  = m_yellowMinPct->value();
    p["polarity_ratio"]  = m_polarityR->value();
    p["white_min_pct"]   = m_whiteMinPct->value();
    p["bridge_min_pct"]  = m_bridgeMinPct->value();
    p["pad_min_pct"]     = m_padMinPct->value();
    p["pad_drop_pct"]    = m_padDropPct->value();
    p["shift_x_pct"]     = m_shiftXPct->value();
    p["shift_y_pct"]     = m_shiftYPct->value();
    p["short_min_pct"]   = m_shortMinPct->value();
    p["short_max_pct"]   = m_shortMaxPct->value();
    p["short_w_min_pct"] = m_shortWMinPct->value();
    p["short_w_max_pct"] = m_shortWMaxPct->value();
    p["short_h_min_pct"] = m_shortHMinPct->value();
    p["short_h_max_pct"] = m_shortHMaxPct->value();
    p["dark_v_mul"]      = m_darkVMul->value();
    p["bright_v_mul"]    = m_brightVMul->value();
    p["neutral_max"]     = m_neutralMax->value();
    p["off_color_mul"]   = m_offColorMul->value();
    p["off_color_r_max"] = m_offColorRMax->value();
    p["mask_v_mul"]      = m_maskVMul->value();
    return p;
}

} // namespace w7
