#include "pcb_defect_judge.h"
#include "pcb_color_ref.h"
#include "board_recipe.h"
#include "pcb_core.h"
#include <QDoubleSpinBox>
#include <QLabel>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include "artifact_path.h"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

// 共享判定核心不带 Qt。`using namespace pcbcore;` 之后,
// 原来写的 `pcbcolor::isDark(...)` 照常解析 —— 那边也是 pcbcore::pcbcolor。
using namespace pcbcore;

namespace w7 {

// ============================================================
// 这一段原来有一整个匿名 namespace,里面是:
//   addDefect / addMetric / knownRulesForKind /
//   missingAsksBodyPresence / rulesForComponent / maskBy
//
// 2026-10-20 起全部搬进共享核心 common/pcb_core/pcb_core.cpp ——
// W6 命令行也要用同一份。**这里不再留一份**:
// 同一批规则分派写两处,改一边忘一边就是下一次事故
// (这个项目已经在抄副本上栽过四遍)。
//
// 搬走之后,这个文件只剩两件事:
//   1. 算子外壳(Qt 的参数表单、QJson 进出)
//   2. 搬运(QJsonObject <-> 核心的纯 C++ 结构体)
// 语义全在核心那边。
// ============================================================

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
// ⚠ 2026-10-20:极性这条**不再是阈值判据**,所以没有旋钮了。
//   原来有 polarity_ratio(白丝印质心横向位置)和 white_min_pct(白色占比下限),
//   两个都删了 —— 留着就是界面上摆两个**调了不生效**的旋钮。
//   新判据:腐蚀出**极性条本身** → 定它在左还是在右 → 和配方的
//   polarity_side 比。"应该在哪一侧"是板的设计决定的,由配方给,不是阈值。
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

    // --- 短路:端子间隙 ---
    //
    // ⚠ 2026-10-20:这里原来有**六个**短路旋钮
    //   (short_min_pct / short_max_pct / short_w_min_pct /
    //    short_w_max_pct / short_h_min_pct / short_h_max_pct),
    //   量的是"全图最大的蓝紫连通域是不是一个特定大小的圆环"。
    //
    //   那条判据量的**不是短路**,是**生成器画给人眼看的标记圈**。
    //   实测两个方向都错:
    //     power_short.png(真短路,画的是一条金条)-> 整板 PASS(漏检)
    //     mcu_bridge.png(焊桥,画的是焊锡连片)  -> 多报一条"短路"(误报)
    //   病根是**量错了对象**,不是阈值没调好 —— 所以六个旋钮一起删,
    //   留着就是界面上摆六个**调了不生效**的旋钮。
    //
    // 现在的短路判据:量"元件两个端子之间的绝缘缝里有没有金属",
    // 端子位置由生成器登记、配方带下来(见 RecipeSite::pads)。
    // 读数 = 缝里有金属的采样点占比;好板 0%,短路板接近 100%,
    // 阈值取中点 50。
    p["short_gap_min_pct"] = 50.0;
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
    // ⚠ 2026-10-20:这段检查搬到共享核心(常见/pcb_core)去了 ——
    //   规则怎么分派只有那一份实现。**这里不再算一遍**,
    //   两处各算一次必然漂开(本项目栽过四遍)。
    //   核心在 refused 时会把原因带回来,下面统一处理。
    //
    // 为什么必须留这道闸:上面那句"规则一条都跑不了,跑下去只会报出
    // 一个没有依据的 PASS"是实测出来的,不是理论担忧 ——
    // 所以要在代码里挡住,不能指望上游永远配对。
    // ============================================================
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
    // ⚠ 这个 60 是上面 defaultParams 里那份的第二份副本(老毛病:同一个数
    //   在一个文件里抄多处,改一处忘一处,界面和实际跑的就会不一样)。
    //   改的时候两处一起改。
    const double bridgeMinPct = params.value("bridge_min_pct").toDouble(60.0);
    const double padMinPct    = params.value("pad_min_pct").toDouble(0.46);
    const double padDropPct   = params.value("pad_drop_pct").toDouble(33.0);
    const double shiftXPct    = params.value("shift_x_pct").toDouble(8.0);
    const double shiftYPct    = params.value("shift_y_pct").toDouble(10.0);
    const double shortGapMinPct = params.value("short_gap_min_pct").toDouble(50.0);

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

    // ============================================================
    // 交给共享判定核心跑
    //
    // 2026-10-20 起,逐条规则、阈值、配色、跳过/覆盖度的判定
    // 全部在 common/pcb_core/pcb_core.cpp 里 —— W6 命令行和 W7 算子
    // **共用同一份**。
    //
    // 以前这份逻辑只有 W7 有,而 W6 那边是七处 `if (label == "R2")`。
    // 两边各一份 = 抄副本 = 改一处忘一处(这个项目栽过四遍)。
    //
    // 这里剩下的活只有"搬运":QJsonObject -> 纯 C++ 结构体 -> QJsonObject。
    // 语义(判据、阈值、守卫)全在核心那边,两边不可能各有一份。
    // ============================================================

    // --- QJsonArray(上游给的元件清单) -> 核心的 InspectSite ---
    //
    // ⚠ checksExplicit 必须原样传下去:"checks 字段不存在"和
    //   "checks 是空数组"是**两件不同的事** ——
    //   前者按封装类别兜底,后者是配方明确说"这个元件我不管它"。
    std::vector<InspectSite> coreSites;
    coreSites.reserve(comps.size());
    for (const auto& v : comps) {
        const QJsonObject c = v.toObject();
        InspectSite s;
        s.label = c.value("label").toString().toStdString();
        s.kind  = c.value("kind").toString().toStdString();
        s.roi   = cv::Rect(c.value("x").toInt(), c.value("y").toInt(),
                           c.value("w").toInt(), c.value("h").toInt());
        if (c.contains("checks")) {
            s.checksExplicit = true;
            for (const auto& r : c.value("checks").toArray()) {
                s.checks.push_back(r.toString().toStdString());
            }
        }
        if (c.contains("shift_tol_pct")) {
            s.hasShiftTol  = true;
            s.shiftTolPct  = c.value("shift_tol_pct").toDouble();
        }
        // 极性条应该在哪一侧。上游(pcb_inspect)传下来,这里原样带走。
        //
        // ⚠ 漏了这一句,后果不是"少报一个缺陷",是**整条 polarity 规则
        //   变成"没查"**:core 那边拿不到基准就只能跳过,跳过的条目
        //   混在报告里没人看,好板照样 PASS —— 而极性反的废板也 PASS。
        //   这是"字段存在 ≠ 字段被验证"的又一次:W7 的回归测试红了
        //   才发现,核心和 W6 一直是对的,坏的是这一跳。
        if (c.contains("polarity_side"))
            s.polaritySide = c.value("polarity_side").toString().toStdString();
        // 端子(焊盘)位置。上游(pcb_inspect)传下来,这里原样带走。
        //
        // ⚠ 和 polarity_side 同一类坑:漏了这一句,短路规则就
        //   变成"没查"(skipped)—— 好板照样 PASS,真短路的废板也 PASS,
        //   而界面上看不出区别(少一条缺陷而已)。
        if (c.contains("pads")) {
            for (const auto& pv : c.value("pads").toArray()) {
                const QJsonArray one = pv.toArray();
                if (one.size() != 3) continue;
                Pad p;
                p.x = one.at(0).toInt();
                p.y = one.at(1).toInt();
                p.r = one.at(2).toInt();
                s.pads.push_back(p);
            }
        }
        if (c.contains("body_pct")) {
            s.hasBodyPct = true;
            s.bodyPct    = c.value("body_pct").toDouble();
        }
        s.goldPct = c.value("gold_pct").toDouble(0.0);
        coreSites.push_back(s);
    }

    // --- 参数打包 ---
    //
    // 上面那些局部变量就是算子参数读出来的值(带默认值),
    // 这里原样搬进核心的参数结构体 —— 不在这里再抄一份默认值。
    JudgeParams jp;
    jp.darkMinPct   = darkMinPct;
    jp.bodyMinPct   = bodyMinPct;
    jp.bodyRatioMin = bodyRatioMin;
    jp.bodyMinSites = bodyMinSites;
    jp.yellowMinPct = yellowMinPct;
    jp.padMinPct    = padMinPct;
    jp.padDropPct   = padDropPct;
    jp.bridgeMinPct = bridgeMinPct;
    jp.shiftXPct    = shiftXPct;
    jp.shiftYPct    = shiftYPct;
    jp.shortGapMinPct = shortGapMinPct;
    jp.darkVMul     = darkVMul;
    jp.brightVMul   = brightVMul;
    jp.neutralMax   = neutralMax;
    jp.offColorMul  = offColorMul;
    jp.offColorRMax = offColorRMax;
    jp.maskVMul     = maskVMul;

    const JudgeResult jr = judge(img, coreSites, jp, goldMask, boardBase);

    // 核心拒绝判定时**不给结论** —— 没判定过就没有判定结果,
    // 空字符串会被下游当成"判了但没值"。
    if (jr.refused) {
        output["error"] = QString::fromUtf8(jr.refuseReason.c_str());
        output["component_count"] = comps.size();
        return output;
    }

    // --- 核心结果 -> QJson ---
    QJsonArray defects;
    for (const auto& d : jr.defects) {
        defects.append(QJsonObject{
            {"component", QString::fromUtf8(d.component.c_str())},
            {"type",      QString::fromUtf8(d.type.c_str())},
            {"detail",    QString::fromUtf8(d.detail.c_str())}});
    }
    // 每条跑过的规则都记了实测值(含没触发的)。调阈值时看这个:
    // 只看 defects 的话,好板上那一堆"离阈值还差多少"全是隐形的。
    QJsonArray metrics;
    for (const auto& m : jr.metrics) {
        metrics.append(QJsonObject{
            {"component", QString::fromUtf8(m.component.c_str())},
            {"rule",      QString::fromUtf8(m.rule.c_str())},
            {"value",     m.value},
            {"threshold", m.threshold},
            {"unit",      QString::fromUtf8(m.unit.c_str())}});
    }
    // 被守卫跳过的规则。**和 defects 分开报** —— 混进去就成了"查出问题",
    // 而实际上是没查。报告里两个都得看得见。
    QJsonArray skipped;
    for (const auto& s : jr.skipped) {
        skipped.append(QJsonObject{
            {"component", QString::fromUtf8(s.component.c_str())},
            {"rule",      QString::fromUtf8(s.rule.c_str())},
            {"reason",    QString::fromUtf8(s.reason.c_str())}});
    }

    const cv::Mat& marked = jr.marked;
    // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
    // 理由见 artifact_path.h 的说明。
    QString markedPath = w7::artifactPath(
        "pcb_defect_judge",
        inputs.value("node_uuid").toString(),
        inputs.value("frame_index").toInt(-1));
    QDir().mkpath(QFileInfo(markedPath).absolutePath());
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

    // 只覆盖一部分:必须说清楚覆盖了多少,
    // 否则"合格"会被当成整板都查过了。
    //
    // ⚠ 这个统计数的是**配方里配了规则的元件**,不是**真的跑过的规则**。
    //   两者不等:规则可能在拿到读数之前就被守卫挡掉(见 skipped)。
    //   实测 mcu/power/signal 三块板按这个数算都是"全覆盖",
    //   而 insufficient 在 6 个站点上一次都没跑过。所以覆盖度只说明
    //   "配方写了什么",真正的执行情况看 skipped。
    //
    // 数字由共享核心算(那边只有一份规则分派实现,这里不再算一遍)。
    if (jr.withRules < jr.totalComponents) {
        output["warning"] = QString(
            "%1 个元件里只有 %2 个配了判定规则,另外 %3 个未判定 —— "
            "result 只覆盖已判定的那些。")
                .arg(jr.totalComponents).arg(jr.withRules)
                .arg(jr.totalComponents - jr.withRules);
    }
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
    m_bridgeMinPct = mk(dflt("bridge_min_pct", 60.0),  2);
    m_padMinPct    = mk(0.46,  2);
    m_padDropPct   = mk(33.0,  2);
    m_shiftXPct    = mk(8.0,   2);
    m_shiftYPct    = mk(10.0,  2);
    m_shortGapMinPct = mk(dflt("short_gap_min_pct", 50.0), 1);
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
    m_layout->addRow(tr("焊桥:最长金段占 ROI 宽 %"),   m_bridgeMinPct);
    m_layout->addRow(tr("焊少:焊盘噪声门槛 占 ROI %"), m_padMinPct);
    m_layout->addRow(tr("焊少:比中位数低 %"),          m_padDropPct);
    m_layout->addRow(tr("偏移:x 容差 占 ROI 宽 %"),    m_shiftXPct);
    m_layout->addRow(tr("偏移:y 容差 占 ROI 高 %"),    m_shiftYPct);
    m_layout->addRow(tr("短路:端子间隙里有金属的采样点占比下限 %"), m_shortGapMinPct);

    // 分组标题。上面 15 个是"占 ROI / 全图"的百分比,下面 6 个是
    // "占板面基色"的倍数 —— 量纲不同,混在一起排会让人以为它们可比。
    auto* sep = new QLabel(tr("—— 以下为「占板面基色」的倍数(自动按图估计板色)——"));
    m_layout->addRow(sep);
    m_layout->addRow(tr("缺件/偏移:暗 V < 板面 V x"),    m_darkVMul);
    m_layout->addRow(tr("极性:白 V >= 板面 V x"),         m_brightVMul);
    m_layout->addRow(tr("极性:白 中性度 上限"),           m_neutralMax);
    m_layout->addRow(tr("错件:B,G >= 板面 x"),            m_offColorMul);
    m_layout->addRow(tr("错件:R <= 板面 x"),              m_offColorRMax);
    m_layout->addRow(tr("金层掩膜 V 下界 x"),            m_maskVMul);
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
    m_bridgeMinPct->setValue(params.value("bridge_min_pct").toDouble(d.value("bridge_min_pct").toDouble()));
    m_padMinPct->setValue(params.value("pad_min_pct").toDouble(d.value("pad_min_pct").toDouble()));
    m_padDropPct->setValue(params.value("pad_drop_pct").toDouble(d.value("pad_drop_pct").toDouble()));
    m_shiftXPct->setValue(params.value("shift_x_pct").toDouble(d.value("shift_x_pct").toDouble()));
    m_shiftYPct->setValue(params.value("shift_y_pct").toDouble(d.value("shift_y_pct").toDouble()));
    // short_gap_min_pct 同样只认 defaultParams() 这一个入口 ——
    // 原来这六个旋钮各自带一个手写的兜底默认值,是抄的第二份账。
    m_shortGapMinPct->setValue(params.value("short_gap_min_pct").toDouble(d.value("short_gap_min_pct").toDouble()));
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
    p["bridge_min_pct"]  = m_bridgeMinPct->value();
    p["pad_min_pct"]     = m_padMinPct->value();
    p["pad_drop_pct"]    = m_padDropPct->value();
    p["shift_x_pct"]     = m_shiftXPct->value();
    p["shift_y_pct"]     = m_shiftYPct->value();
    p["short_gap_min_pct"] = m_shortGapMinPct->value();
    p["dark_v_mul"]      = m_darkVMul->value();
    p["bright_v_mul"]    = m_brightVMul->value();
    p["neutral_max"]     = m_neutralMax->value();
    p["off_color_mul"]   = m_offColorMul->value();
    p["off_color_r_max"] = m_offColorRMax->value();
    p["mask_v_mul"]      = m_maskVMul->value();
    return p;
}

} // namespace w7
