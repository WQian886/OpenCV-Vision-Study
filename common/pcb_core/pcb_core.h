#pragma once
// ============================================================
// pcb_core — W6 与 W7 共用的 PCB 判定核心(无 Qt 依赖)
//
// ## 这份代码为什么存在
//
// W6 是命令行项目,W7 是 Qt 流水线编辑器。PCB 判定这块能力两边都要,
// 但两边**各写一份**的后果已经实测过:
//   - W6 那边是 `if (label == "R2")` 七处硬编码,换块板规则全空转,
//     三块好板全被判废、两块废板输出与好板逐字相同;
//   - W7 那边把这些改成了配方驱动 + 逐条标定的判据,花了不少功夫。
//
// 也就是说:**能力在 W7,数据在 W6,而 W6 自己用不上。**
// 硬把 W7 的算法抄一份到 W6,就是本项目栽过四遍的"抄副本" ——
// 抄出来的那份会随 W7 的标定一起漂,漂了没人知道。
//
// 所以把判定逻辑抽到这里,**两边都调这一份**。
//
// ## 边界
//
// 这一层刻意不依赖 Qt:
//   - W6(命令行)不引 Qt,引了就是给一个学习项目加依赖
//   - W7 的算子只要做一层薄适配(QJsonObject <-> 这里的结构体),
//     Qt 的部分就退回到"搬运数据",判定逻辑不再被 Qt 绑架
//
// 只依赖 OpenCV + 标准库。
//
// ## 三块东西
//
//   1. 配方(recipe)  —— 一块板的真值:元件在哪 + 每个元件查哪几条规则
//   2. 定位(inspect) —— 按配方把元件框出来,并量出 body_pct
//   3. 判定(judge)   —— 逐元件跑规则,给出缺陷 / 读数 / 跳过的规则
//
// 三者的关系是:**配方说在哪、查什么;定位负责量;判定负责判。**
// 坐标和规则放在同一份数据里 —— 分开放就会各自漂开,
// 漂开之后上层看着"配好了",实际一条规则都没跑,还不报错。
// ============================================================

#include <opencv2/opencv.hpp>

#include <string>
#include <vector>

#include "pcb_color_ref.h"

namespace pcbcore {

// ============================================================
// 规则名 —— 算子与配方之间的接口
//
// ⚠ 这些字符串是**配方与判定之间的契约**:加一条规则要同时改两边,
//   所以每个名字都必须真有一条同名分支在跑 —— 见 knownChecks()。
// ============================================================
namespace rule {
inline constexpr char kMissing[]      = "missing";      // 本体像素过少(疑似被擦除)
inline constexpr char kShifted[]      = "shifted";      // 本体质心偏离应站中心
inline constexpr char kWrong[]        = "wrong";        // 本体错色(装错件)
inline constexpr char kPolarity[]     = "polarity";     // 极性条偏向反了
inline constexpr char kBridge[]       = "bridge";       // 焊盘间金条连片
inline constexpr char kInsufficient[] = "insufficient"; // 焊盘面积远低于同侪
} // namespace rule

// 判定真正实现了的规则。配方里出现表以外的规则名 = 拼错了,
// 必须报错而不是"安静地不跑" —— 后者会让缺陷图看着像合格。
const std::vector<std::string>& knownChecks();

// ============================================================
// 配方
// ============================================================
struct RecipeSite {
    std::string label;       // 位号,只用于显示和报告,判定不依赖它
    std::string kind;        // 封装类别(RES / CAP_SMALL / QFP64 ...)
    int cx = 0, cy = 0;      // 本体中心(参考分辨率下)
    int bodyW = 0, bodyH = 0;
    std::vector<std::string> checks;   // 该元件身上要跑的规则
    double shiftTolPct = 0;             // shifted 容差(% ROI 宽/高);<=0 表示用默认
    bool hasShiftTol() const { return shiftTolPct > 0; }

    // 极性条应该在哪一侧("left"/"right",相对**元件本体**)。
    //
    // ⚠ 为什么是配方给的,不是判据猜的:
    //   "反装"= 极性条挪到了另一边,而**哪边是对**取决于板的设计
    //   (电容的负极标在左、二极管的阴极标在右)。
    //   判据猜错的后果是**把好板判成反装** —— 静默误报,比漏检更难查。
    //
    //   配方的这一项由**生成器**带下来(画极性条的那一行代码自己登记),
    //   不是手抄的。
    //
    // 空串 = 这个元件没有极性条(陶瓷电容),或者配方没写 ——
    // 两种情况都判不了极性,必须报出来,不能装作查过了。
    std::string polaritySide;
};

struct BoardRecipe {
    std::string name;        // 板型标识
    std::string title;       // 给人看的板名
    int refW = 0, refH = 0;  // 标定时的参考分辨率
    int buffer = 10;         // ROI 在本体框四周多留的余量(参考分辨率下)
    std::vector<RecipeSite> sites;

    bool isEmpty() const { return sites.empty(); }
};

// 从 JSON 文本解析配方。失败时 reason 写成人话。
//
// 严格程度:**少一个必填字段就报错,不做部分加载**。
// 部分加载最阴险 —— 加载出 5 个站点、另外 3 个静默丢了,
// 判定环节照样输出结论,只是少查了三块板。
bool parseRecipeJson(const std::string& text, BoardRecipe* out, std::string* reason);

// 从文件读配方。
bool loadRecipeFile(const std::string& path, BoardRecipe* out, std::string* reason);

// ============================================================
// 定位
// ============================================================
struct InspectSite {
    std::string label;
    std::string kind;
    cv::Rect roi;
    std::vector<std::string> checks;
    // ⚠ checksExplicit 区分两件不同的事:
    //   false = 上游没给配方信息(老存档 / auto_contour 产物) -> 按封装类别兜底
    //   true  = 配方说了算(哪怕是空数组,意思是"这个元件我不管它")
    // 以前把这两件事当成同一件,好板 baseline.png 在所有缩放下都凭空报焊桥。
    bool checksExplicit = false;
    bool hasShiftTol = false;
    double shiftTolPct = 0.0;
    double bodyPct = 0.0;        // ROI 内"非板面基色"像素占比
    bool hasBodyPct = false;
    double goldPct = 0.0;        // ROI 内金层像素占比
    // 极性条应该在哪一侧("left"/"right")。从配方带下来,判定时和实测比。
    // 空 = 没有极性条 / 配方没写 -> polarity 规则不判定。
    //
    // ⚠ 这个字段**放在结构体末尾**是故意的:
    //   结构体中间插字段会让所有**位置聚合初始化**的调用点整体错位
    //   —— 而且错位不一定编译报错,可能把 bool 塞进 double,静默变味。
    //   放末尾,老调用点原样对得上;新字段请一律用具名赋值。
    std::string polaritySide;
};

struct InspectResult {
    std::vector<InspectSite> sites;
    std::vector<std::string> skippedSites;  // 算不出有效 ROI 的站点
    int sitesWithBody = 0;                  // 框里有东西的站点数
    cv::Mat marked;                         // 画了框的图
    cv::Mat goldMask;                       // 金层掩膜(判定要用,必须同一份)
    cv::Vec3b boardBase;                    // 板面基色
    bool boardBaseOk = false;
};

// 按配方在图上框出元件,并量出 body_pct / gold_pct。
//
// 站点几何按图像尺寸等比缩放,基准取配方自己的 ref_w/ref_h ——
// 换板型时基准也跟着换,否则一块 1200x900 的板会拿 1000x700 的坐标去切,全错。
//
// band 决定金层掩膜取哪一段色带。**定位与判定必须用同一份金层掩膜** ——
// 这两处曾经各自写了一份,漂开之后测试全绿(没人读那个字段)。
InspectResult inspect(const cv::Mat& img, const BoardRecipe& recipe,
                      const pcbcolor::HsvBand& band);

// ============================================================
// 判定
// ============================================================
struct JudgeParams {
    // --- ROI 面积 % ---
    double darkMinPct   = 1.0;    // 暗像素占 ROI 的百分比,低于它判 missing
    double bodyMinPct   = 42.0;   // 彩色本体 missing 的绝对版(站点不足时的兜底)
    double bodyRatioMin = 0.714;  // 彩色本体 missing 的比值版(除以同板中位数)
    int    bodyMinSites = 4;      // 站点数下限:低于此退回绝对判据
    double yellowMinPct = 32.0;   // 错色像素占 ROI 的百分比
    // ⚠ polarityRatio / whiteMinPct 已删(2026-10-20)。
    //   它们是**旧 polarity 判据**的旋钮:白丝印质心横向位置 + 白色占比下限。
    //   那个判据量的是"ROI 里所有白丝印的质心",而极性条只是其中一小条,
    //   旁边还有丝印外框、符号线、十字线在抢 —— 实测连"没极性条"的电容
    //   质心都正好 50.0%。新判据腐蚀出**极性条本身**再定侧,
    //   不再需要这两个旋钮。
    //   留着它们 = 界面上摆两个**调了不生效**的旋钮,骗人。
    //   "应该在哪一侧"改由配方的 polarity_side 给(见 RecipeSite)。
    double padMinPct    = 0.46;   // 焊盘噪声门槛(占 ROI 面积)
    double padDropPct   = 33.0;   // 最小焊盘比中位数低多少算焊少
    // --- ROI 宽度 % ---
    double bridgeMinPct  = 60.0;   // 最长金段占 ROI 宽度
    double shiftXPct     = 8.0;    // 质心横向偏移容差
    double shiftYPct     = 10.0;   // 质心纵向偏移容差
    // --- 全图 % ---
    double shortMinPct   = 0.021;  // 短路圆环面积占全图
    double shortMaxPct   = 0.08;
    double shortWMinPct = 1.1, shortWMaxPct = 8.0;   // 圆环外框宽 / 全图对角线
    double shortHMinPct = 1.0, shortHMaxPct = 6.0;   // 圆环外框高 / 全图对角线
    // --- 颜色判据的倍数(相对板面基色) ---
    double darkVMul     = 0.6;    // 暗: V < 0.6 x 板面V
    double brightVMul   = 1.3;    // 白: V >= 1.3 x 板面V
    double neutralMax    = 1.15;  // 丝印白的中性度上限
    double offColorMul   = 1.6;   // 错色: B,G >= 1.6 x 板面
    double offColorRMax  = 1.3;   // 错色: R <= 1.3 x 板面
    double maskVMul      = 0.8;   // 金层/蓝环 HSV 掩膜的 V 下界
};

// 一条规则的实测读数(**不管有没有触发都记**)
// 调阈值的人需要看到"好板上是多少、废板上是多少",而不是一句 FAIL。
struct Metric {
    std::string component;
    std::string rule;
    double      value = 0.0;
    double      threshold = 0.0;
    std::string unit;
};

struct Defect {
    std::string component;   // 全局缺陷(短路)留空
    std::string type;
    std::string detail;
};

// 一条"没查"的规则。
//
// 规则在拿到读数之前就被守卫挡掉时,既没有 metric 也没有 defect,
// 整块板照样输出 PASS,看上去就是"查过了,没问题"。
// 所以必须单独报出来 —— 和 defects 分开,混进去就成了"查出问题"。
struct Skipped {
    std::string component;
    std::string rule;
    std::string reason;
};

struct JudgeResult {
    std::vector<Defect>  defects;
    std::vector<Metric>  metrics;
    std::vector<Skipped> skipped;
    cv::Mat              marked;      // 画了缺陷框的图

    // 配了判定规则的元件数 / 参与判定的元件总数。
    //
    // ⚠ 这个数数的是**配方里配了规则的元件**,不是**真的跑过的规则**。
    //   两者不等:规则可能在拿到读数之前就被守卫挡掉(见 skipped)。
    //   所以覆盖度只说明"配方写了什么",真正的执行情况看 skipped。
    int withRules = 0;
    int totalComponents = 0;

    // 撞 8 位色深上限时**拒绝判定**,而不是硬撑一个 PASS。
    // 错色判据要"绿通道 >= 1.6 x 板面绿通道",板面被照亮到这个程度时
    // 1.6 倍已经超过 255,uint8 里根本不存在这么亮的像素,判据永远返回
    // false —— 不报错、不告警,直接报 PASS,废板判合格。
    bool        refused = false;
    std::string refuseReason;
};

// 判定。
//
// 拿不到判定依据时**绝不输出 PASS**:
//   - 一个元件都没有规则可跑 -> refused,不给结论
//   - 曝光撞上限             -> refused,不给结论
JudgeResult judge(const cv::Mat& img,
                  const std::vector<InspectSite>& sites,
                  const JudgeParams& params,
                  const cv::Mat& goldMask,
                  const cv::Vec3b& boardBase);

} // namespace pcbcore
