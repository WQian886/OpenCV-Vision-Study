// ============================================================
// 颜色判据去绝对化 —— 相对板面基色的那套量,只保留一份定义
// ============================================================
//
// 📁 这个文件原来在 W7_VisionMasterLite/operators/ 下,2026-10-20
//    移到 common/pcb_core/ —— 因为 W6 也要用同一份颜色判据。
//
//    理由和 pcb_core.h 一样:两边各写一份颜色判据,改一处忘一处,
//    判据就会悄悄漂开(这个项目已经栽过四遍)。
//    它本来就是无 Qt 依赖的纯 OpenCV 头文件,搬过来零成本。
//
//    命名空间从 w7 改成 pcbcore —— 它不再是"W7 专属"的了。
//    W7 里原来写 `pcbcolor::isDark(...)` 的地方不用改:
//    那些文件里加了 `using namespace pcbcore;`,`pcbcolor::` 照常解析。
// ============================================================
//
// 为什么要有这个文件:pcb_defect_judge 里原本有四处 inRange 写着
// 写死的颜色(RGB<=60 / (150,150,0)~(255,255,80) / >=200 ...)。
// 光照一变这些数就全废,而每一处都得单独重新标定 —— 这个项目里
// 「同一个东西抄一份」的亏已经吃过三次(EXPECTS[6]、lo[0] 守卫、
// 守卫本身),所以这里抽一份,判定和测试都从这里取。
//
// ------------------------------------------------------------
// 核心要求:板面基色必须**逐图估计**
// ------------------------------------------------------------
// 相对判据的右端也带 k,才叫相对。拿一张参考图的板色当常量写进配置,
// 等式右边就没有 k 了,"相对"是假的 —— 探针第一版就是这么栽的:
// 它把 k=1.0 的板色 (50,90,40) 传给每一档亮度,结果和绝对阈值
// 表现一模一样。所以下面的 estimateBoardBase 一律从**当前这张图**取。
//
// ------------------------------------------------------------
// 为什么用「逐通道中位数」而不是均值
// ------------------------------------------------------------
// 均值会被元件和丝印拉偏,而我们要的是"板面"这个主色。
// 逐通道中位数保序,所以对全局乘 k 是等变的;而且中位数抗削波 ——
// 实测哪怕 7% 像素削波(k=1.60),基色仍然稳在 (50,90,40)×k。
//
// ------------------------------------------------------------
// 实测依据(8 张样本 × 25 档亮度 k=0.40~1.60)
// ------------------------------------------------------------
// 全部量自 %TEMP%\w7probe\pcb_color_probe.cpp,不掺推测:
//
//   板面基色 BGR = (50,90,40),8 张图完全一致;各档色度漂移最大 0.0039,
//   且 k<=0.95(无削波)时**恰好为 0** —— 色度严格不变,漂移全部来自削波。
//
//   暗%      V < 0.6*Vb      正常 45.6 / 缺件 0.0 / 偏移图 33.7  —— 25 档全平
//   暗质心   x/ROI 宽        正常 0.509 / 偏移图 0.661              —— 25 档全平
//   白%      V>=1.3*Vb && 中性度<=1.15   4.2                      —— 25 档全平
//   白质心   x/ROI 宽        正常 0.179 / 极性反 0.820             —— 25 档全平
//   错色%    见 kOffColorMul 那一段的扫描                              —— 25 档全平
//
// 也就是说这四条判据在 k=0.40~1.60 上一个数都没动过,这才敢换。
// ============================================================

#pragma once
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

namespace pcbcore {
namespace pcbcolor {

// 错色判据的倍数。**不是**原来那个 1.8 —— 表11 实测的二维扫描:
//
//   m     baseline 假阳   wrong 检出   撞 8 位上限的 k
//   1.0        50.2%        91.9%      <=2.83   假阳过半,判据失效
//   1.2         4.3%        44.8%      <=2.36
//   1.4         4.3%        44.8%      <=2.02
//   1.6         3.4%        44.8%      <=1.77   全 25 档平,不塌    <-- 取这个
//   1.8         3.4%        44.8%      <=1.57   k=1.60 整列塌成 0
//   2.0         3.4%        44.8%      <=1.42   k=0.55/0.75/0.95/1.15/1.35 假性塌成 0
//
// m 太小 -> 假阳飙到 50%(倍数必须 >1,"比板面亮"这道门才有意义)。
// m 太大 -> 两种死法,而且都**不报错**:
//   (a) 撞 255 上限:1.8*板面G 在 k=1.6 时 = 259,uint8 根本到不了,
//       判据静默变成"永远假" -> 报 PASS(废板判合格)。
//       临界 k = 255/(m*板面G),m=1.8 时正好 1.57,和实测的塌点吻合。
//   (b) 贴边:实测青色 G/板面G ≈ 1.98,m=2.0 时 k=0.55 舍入到
//       99 vs 100,**差一个量化单位就被判掉**,而且随 k 随机翻车。
// 所以 m 必须严格落在谷内、两边都留余量 —— 1.6 离两边最远。
//
// 余量只有 0.17(k=1.77 vs 扫到的 1.60),所以下面还配了一道
// ceilingBreached 守卫:真越界时**拒绝判定**,而不是硬撑一个 PASS。
//
// ============================================================
// 这些 constexpr 只是**默认值**,不是唯一入口
// ============================================================
// pcb_defect_judge 把它们作为算子参数暴露出去(dark_v_mul 等),
// 运行期可以覆盖。原因有二:
//
//   ① 同一个算子里两套规矩的问题 —— dark_min_pct / yellow_min_pct 全都
//      可调,偏偏"本体够不够暗"这个更靠前的系数写死在头文件里,调参的
//      人根本不知道还有这么几个数。
//   ② 「这个系数上下动 20%,结论变不变」这件事**只能在测试里扫**。
//      写死就没法扫;复制一份判据出来扫就是第四次抄副本,而这个项目里
//      抄副本的账已经记了三笔(EXPECTS[6]、lo[0] 越界守卫、守卫本身)。
//
// 所以下面每个判据都把系数做成带默认值的形参:生产代码走默认值,
// 测试和调参走覆盖入口,两边**共用同一份定义**。
// 默认值全部来自上面这些实测,不是拍出来的。
inline constexpr double kOffColorMul   = 1.6;
inline constexpr double kOffColorRMax  = 1.3;   // 红通道不得超过板面的多少倍
inline constexpr double kDarkMul       = 0.6;   // 暗:V < 0.6*板面V
inline constexpr double kBrightMul     = 1.3;   // 白:V >= 1.3*板面V
inline constexpr double kNeutralMax    = 1.15;  // 中性度 max/min,越接近 1 越灰

// 像素的 V(HSV 里的 V):max(R,G,B)
inline int valOf(const cv::Vec3b& p) {
    return (std::max)(int(p[0]), (std::max)(int(p[1]), int(p[2])));
}

// 中性度 = max通道 / min通道。接近 1 = 灰白,远大于 1 = 有彩色。
// min 为 0 时返回极大值(明确判成"有彩色"),不去做 0 除法。
inline double neutralness(const cv::Vec3b& p) {
    const int mx = valOf(p);
    const int mn = (std::min)(int(p[0]), (std::min)(int(p[1]), int(p[2])));
    if (mn <= 0) return 1e9;
    return double(mx) / double(mn);
}

// 中性度判据的**数学地板**,不是标定出来的经验值。
//
// neutralness = max通道/min通道,而 max >= min 恒成立,所以它恒 >= 1.0。
// 于是 neutralMax < 1.0 时,isWhiteSilk 的第二个条件恒假 ——
// 不是"阈值偏严导致少判",是**判据被证伪,永远不可能成立**。
//
// 实测(polarity.png,C1 ROI 白色像素占 ROI 面积):
//     neutralMax = 1.15 -> 4.21%   1.02 -> 4.10%   1.00 -> 4.05%
//     neutralMax = 0.98 -> 0.00%   0.92 -> 0.00%   0.80 -> 0.00%
// 不是渐变,是 1.00 处的断崖。
//
// 断崖下面是本项目最阴险的那种失效:白色掩膜全空 -> whitePct = 0
// < white_min_pct -> **polarity 规则整条不跑** -> defects 为空
// -> result = PASS。极性反的板子被判合格,画布上状态点是绿的,
// 日志一片正常。和 commit c152701 堵掉的自动模式那次一模一样。
//
// 所以中性度上限在参数里必须被夹住,不能让人调到 1.0 以下。
inline constexpr double kNeutralFloor = 1.0;

// 逐图估计板面基色。隔行隔列抽样,8 张样本上逐通道中位都是 (50,90,40)。
inline cv::Vec3b estimateBoardBase(const cv::Mat& img) {
    std::vector<uchar> b, g, r;
    for (int y = 0; y < img.rows; y += 2)
        for (int x = 0; x < img.cols; x += 2) {
            const cv::Vec3b p = img.at<cv::Vec3b>(y, x);
            b.push_back(p[0]); g.push_back(p[1]); r.push_back(p[2]);
        }
    auto med = [](std::vector<uchar>& v) -> uchar {
        if (v.empty()) return 0;
        const size_t h = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + h, v.end());
        return v[h];
    };
    return cv::Vec3b(med(b), med(g), med(r));
}

// ---------- 三条相对判据 ----------
// 全部写成 double 比较,不先转成 int,所以不存在"转的时候就被截断"。
//
// 每个系数都留一个默认实参的覆盖入口,给敏感性测试用 ——
// 「这个系数上下动 30%,结论变不变」这件事只能在测试里扫,
// 而复制一份判据出来扫就又是一次抄副本(实测这么干过一次,后果是
// 两份定义慢慢漂开)。默认值就是上面的实测值,生产代码一律走默认。

// missing / shifted 用:本体够不够暗
inline bool isDark(const cv::Vec3b& p, const cv::Vec3b& base,
                   double mul = kDarkMul) {
    return double(valOf(p)) < mul * double((std::max)(1, valOf(base)));
}

// polarity 用:够亮,而且近中性(丝印白)
inline bool isWhiteSilk(const cv::Vec3b& p, const cv::Vec3b& base,
                        double brightMul = kBrightMul,
                        double neutralMax = kNeutralMax) {
    const int vb = (std::max)(1, valOf(base));
    return double(valOf(p)) >= brightMul * double(vb)
        && neutralness(p) <= (std::max)(kNeutralFloor, neutralMax);
}

// wrong 用:错色件 —— 蓝、绿都远超板面,红远低于板面。
// 判据实测:正常板 3.4%,错件 44.8%,25 档亮度不变。
inline bool isOffColor(const cv::Vec3b& p, const cv::Vec3b& base,
                       double mul = kOffColorMul,
                       double rMax = kOffColorRMax) {
    const double rb = (std::max)(1.0, double(base[0]));
    const double gb = (std::max)(1.0, double(base[1]));
    const double bb = (std::max)(1.0, double(base[2]));
    return double(p[0]) >= mul * bb
        && double(p[1]) >= mul * gb
        && double(p[2]) <= rMax * rb;
}

// 8 位色深的天花板。m*板面通道 > 255 时,上面的判据在物理上
// 就不可能成立 —— 它不会报错,只会永远返回 false,于是报出 PASS。
//
// kOffColorMul=1.6 时,这道线是 k = 255/(1.6*板面G) ≈ 1.77。
// 越线之后信息是真的没了(实测 k=1.55 时错色像素已 99.6% 削波,
// 削波后变成 (255,255,R),G/B=1.00 和暗本体重合,色相这条路也断了),
// 所以正确做法是**拒绝判定**并说明原因,不是放宽阈值硬撑。
//
// mul 也做形参:mul 是运行期参数,天花板必须跟着它动 ——
// 写死 kOffColorMul 会让"把倍数调大"这个动作**看起来安全**,
// 实际上守卫还按 1.6 在算,新阈值早就撞墙了。这是一处会静默失效的不一致。
inline bool ceilingBreached(const cv::Vec3b& base,
                            double mul = kOffColorMul) {
    const double mx = (std::max)({double(base[0]), double(base[1]), double(base[2]), 1.0});
    return mul * mx > 255.0;
}

// 越线时能报出多少 —— 给人看的,不是给机器判的。
inline double ceilingHeadroomK(const cv::Vec3b& base,
                               double mul = kOffColorMul) {
    const double mx = (std::max)({double(base[0]), double(base[1]), double(base[2]), 1.0});
    return 255.0 / (mul * mx);
}

// ---------- HSV 掩膜的 V 下界 ----------
//
// OpenCV 的 H 和 S 都是从 max/min 算出来的,全局乘 k **严格不变**;
// 只有 V(= max 通道)会跟着缩放。所以去绝对化只需要动 V 那一项,
// 而且不会像错色判据那样撞 255 上限(r < 1,越小离上限越远)。
//
// 实测(表12,r 从 0.5 到 1.0 全部逐档完全相同):
//   金层 U1 ROI  占 12.96%(insufficient 图 12.26%),25 档一格不差
//   蓝环 全图     baseline 0.04% / short.png 0.08%,25 档一格不差
//
// 也就是说 H∈[85,115] S>=80 这一类里**没有比板面还暗的像素**,
// 原来那个写死的 V>=80 / V>=100 本来就一道活都没干,H/S 已经把暗背景
// 全挡住了 —— 压暗时它们失效不是因为这条线太严,而是因为别的环节。
// 改成相对式之后,掩膜从此**可证明**与整体亮度无关。
//
// 既然在样本上不起作用,r 的取值对现有结论没有影响;但真产线数据上
// 别的板色可能让暗像素混进来,所以这道下限保留 —— 只是锚在板面上。
inline constexpr double kMaskVMul = 0.8;

inline uchar maskVLower(const cv::Vec3b& base, double mul = kMaskVMul) {
    const double v = mul * double((std::max)(1, valOf(base)));
    return static_cast<uchar>((std::min)(255.0, (std::max)(0.0, v)));
}

// ---------- HSV 掩膜本体 ----------
//
// 抽出来的原因不是"好看",是**实测出来两份已经漂开了**:
//   pcb_inspect   H[85,115] S>=80 V>=80   (绝对,2026-10-09 之前一直如此)
//   pcb_defect_judge H[85,115] S>=80 V>=0.8*板面V
// 上一轮只把判定那一份改成相对式,定位那一份没动。两份 H/S 相同、
// 只有 V 下界不同,而唯一能暴露这个差异的字段 gold_px **全仓库没人读** ——
// 于是 56/56 端到端测试照样全绿,漂开这件事没有任何测试能看见。
//
// 这就是"抄副本"最贵的一种:第四份抄出来了,而这次连测试都是绿的。
//
// 所以 H/S 区间和"怎么用板面算 V 下界"都收在这里,
// 两个算子只调这一个函数。H/S 保持可配(pcb_inspect 原来就是参数,
// 界面上的六个框还得留),但**默认值来自这份常量**,不是各处自己写一遍。
struct HsvBand {
    int hLo, hHi;
    int sLo;
    double vMul;
};

// 金层。实测 H 85-115:合成器的 GOLD 实际落在这段(W6 笔记记的坑 ——
// 用 W2 的常规金色范围会抓到 IC 的深蓝本体而不是金层)。
inline HsvBand goldBand(double vMul = kMaskVMul) { return {85, 115, 80, vMul}; }

// ⚠ 2026-10-20:这里原来还有一条 blueBand(H 100-135),给"短路圆环"判据用。
//
//   它已经**删掉了**,原因不是"用不上",是**它量的不是短路**:
//   那条判据找的是全图最大的蓝紫连通域,再看它像不像一个特定大小的
//   圆环 —— 而那个蓝紫圆环是**生成器画给人眼看的标记圈**。
//   实测:真短路图(power_short.png)整板 PASS(漏检),
//         焊桥图(mcu_bridge.png)反倒报了一条"短路"(误报)。
//   病根是量错了对象,不是阈值没调好,所以色带一起删 ——
//   留着一条没人用的色带,下次有人又会照着它写一条"蓝斑判据"。
//
//   短路现在按**端子间隙里的金属**判,只用金层这一条色带。

// 按色带生成掩膜。V 下界锚在板面基色上,理由见上面 maskVLower 那段。
//
// hsv 必须是 BGR2HSV 之后的图。这里不自己调 cvtColor ——
// 两个算子本来就都要转一次(judge 还要蓝环那一道),转换留在调用方,
// 别在这儿再转一遍变成第三份开销。
inline cv::Mat bandMask(const cv::Mat& hsv, const cv::Vec3b& base,
                        const HsvBand& band) {
    cv::Mat m;
    cv::inRange(hsv,
                cv::Scalar(band.hLo, band.sLo, maskVLower(base, band.vMul)),
                cv::Scalar(band.hHi, 255, 255),
                m);
    return m;
}

// 开运算去噪点。两个算子的金层掩膜都要,原来各写一遍
// getStructuringElement + morphologyEx。核尺寸跟着图缩放 ——
// 原来写死 3x3,在 2x 图上等于噪点比原来大一倍,0.5x 上又比原来小一倍。
inline cv::Mat opened(const cv::Mat& mask, double scale = 1.0) {
    int n = (std::max)(1, static_cast<int>(std::lround(3.0 * scale)));
    if (n % 2 == 0) ++n;               // 必须是奇数,才有中心
    cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(n, n));
    cv::Mat out;
    cv::morphologyEx(mask, out, cv::MORPH_OPEN, k);
    return out;
}

// 核尺寸该按多大 —— 相对图像短边的经验比例,不是像素数。
// 实测:baseline 1000x700 上 3x3 正好;换分辨率时固定 3x3 会让
// 噪点过滤的相对强度跟着变(0.5x 变强、2x 变弱)。
inline double kernelScale(const cv::Mat& img, int refW = 1000, int refH = 700) {
    return (std::min)(static_cast<double>(img.cols) / refW,
                    static_cast<double>(img.rows) / refH);
}

} // namespace pcbcolor
} // namespace pcbcore
