#pragma once
// ============================================================
// pcb_defect_judge — PCB 缺陷判定(对标 VM "PCB 缺陷检测"方案)
//
// === 本算子的来历 ===
// 承接 W6 综合项目的 7 类缺陷判定:W6 把它写成了 main.cpp 里的
// 一堆 judgeXxx() 函数,只能靠命令行跑,还把阈值全硬编码在源码里。
// 搬成算子后:
//   - 阈值变成可调参数(不用重编译)
//   - 判定结果进 JSON,能和下游算子继续串
//   - 和 pcb_inspect 分离,换定位方式不影响判定
//
// 输入:image_path + components[](来自 pcb_inspect)
// 输出:defects[] + defect_count + ok_count + result(PASS/FAIL) + marked_path
// params: thresholds(各判定的阈值,全部可调)
//
// === 7 类缺陷(与 W6 一致) ===
//   missing      缺件     ROI 里"没有元件"。
//                          黑本体(RES/DIODE/TO220/QFP64/SOIC8)问暗像素占比,
//                          彩色本体(CAP*)问 body_pct(非板面像素占比)——
//                          后者是 2026-10-14 加的,原因见 .cpp 的
//                          missingAsksBodyPresence():电容的暗像素只剩 1px 描边,
//                          图宽千分之一的模糊就归零,好板集体误报。
//   wrong        错件     R3 本体变成浅黄而非黑
//   polarity     极性反   C1 白色极性条出现在错误的一侧(左/右由配方给定)
//   bridge       焊桥     R1 焊盘间金条连成一片
//   insufficient 焊少     U1 某个焊盘面积远小于中位数
//   shifted      偏移     R2 本体质心偏离应站中心
//   short        短路     蓝紫圆环(全局缺陷,不属单个元件)
//
// === 阈值可调的原因 ===
// W6 的阈值是针对合成数据调的,换真实 PCB 必然要改。
// 写死在源码里就得重编译 —— 这正是 W6 作为"作业"的局限。
//
// === 阈值为什么是"百分比"而不是"像素数" ===
// 像素阈值只在标定那张图上成立。图一换分辨率,量出来的东西全部变,
// 而阈值纹丝不动 —— 于是同一个 70px 从"占宽度 58%"变成"占宽度 29%",
// 焊桥就判不出来了。不报错、不崩,只是安静地漏检。
// 所以每条规则的阈值都改成"占某个基准的百分比",基准逐条不同
// (ROI 面积 / ROI 宽度 / 全图面积 / 全图对角线),理由见 .cpp。
//
// 参数名也跟着换了(dark_thresh → dark_min_pct 等):
// 老存档里的旧名字读不到就当没传,退回新默认值。
// 不能沿用旧名字 —— 那样老存档里的 50 会被当成 50%,比原来差得离谱。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QDoubleSpinBox;
class QSpinBox;

namespace w7 {

class PcbDefectJudge : public OperatorBase {
public:
    QString name() const override { return "pcb_defect_judge"; }
    QString displayName() const override { return "PCB 缺陷判定"; }
    QString category() const override { return "detection"; }
    QString description() const override {
        return "判定 PCB 的 7 类缺陷(缺件/错件/极性反/焊桥/焊少/偏移/短路)"
               ",阈值全部可调,且都是「占某个基准的百分比」而不是像素数";
    }
    // 吃元件清单(数据),吐缺陷清单 + PASS/FAIL(文本)
    //
    // 输入标成 Data 而不是 Image:这个算子读的是 components 数组,
    // 图像路径只是顺带用的(而且由 pcb_inspect 透传下来)。
    // 这样"图像源 → 缺陷判定"会在连线时就被挡下来 ——
    // 那种连法 Run 时会因为缺 components 直接报错,早拦一秒。
    PinType inputPinType()  const override { return PinType::Data; }
    PinType outputPinType() const override { return PinType::Text; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === PCB 缺陷判定参数面板 ===
class PcbDefectJudgeForm : public ParamForm {
    Q_OBJECT
public:
    explicit PcbDefectJudgeForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    // 全是百分比(用 QDoubleSpinBox,不是整数 SpinBox):
    // 短路判据量的是"端子间隙里有金属的采样点占比",要能敲 0.1 的步长。
    QDoubleSpinBox* m_darkMinPct   = nullptr;  // missing(黑本体): 暗像素占 ROI 面积下限
    QDoubleSpinBox* m_bodyMinPct   = nullptr;  // missing(彩色本体): 非板面像素占 ROI 面积下限(站点不足时的兜底)
    QDoubleSpinBox* m_bodyRatioMin = nullptr;  // missing(彩色本体): 占同板中位数的下限
    QSpinBox*       m_bodyMinSites = nullptr;  // 用比值所需的最少站点数
    QDoubleSpinBox* m_yellowMinPct = nullptr;  // wrong:   浅黄占 ROI 面积下限
    QDoubleSpinBox* m_bridgeMinPct = nullptr;  // bridge:  最长金段占 ROI 宽度
    // ⚠ 2026-10-21:m_padMinPct(焊盘噪声门槛)已删。
    //   新的 insufficient 判据逐个**登记过的端子**量它自己框里的金属,
    //   框是登记过的,量到的就是那个焊盘的铜 —— 不需要"噪声门槛"。
    //   留着这个旋钮 = 界面上摆一个**调了不生效**的旋钮,骗人。
    QDoubleSpinBox* m_padDropPct   = nullptr;  // insufficient:最小焊盘比同侪中位数低多少
    QDoubleSpinBox* m_shiftXPct    = nullptr;  // shifted: x 偏移容差(占 ROI 宽)
    QDoubleSpinBox* m_shiftYPct    = nullptr;  // shifted: y 偏移容差(占 ROI 高)
    QDoubleSpinBox* m_shortGapMinPct = nullptr;  // short: 端子间隙里有金属的采样点占比下限
    // 颜色判据的倍数(占板面基色,不是占 ROI)。range 和上面那些百分比
    // 一样是 0~100,步长不同 —— 0.6/1.15/1.6 这几个数要能精确敲进去。
    QDoubleSpinBox* m_darkVMul      = nullptr;  // 暗: V < 0.6 x 板面V
    QDoubleSpinBox* m_brightVMul    = nullptr;  // 白: V >= 1.3 x 板面V
    QDoubleSpinBox* m_neutralMax    = nullptr;  // 丝印白: 中性度 max/min 上限
    QDoubleSpinBox* m_offColorMul   = nullptr;  // 错色: B,G >= 1.6 x 板面
    QDoubleSpinBox* m_offColorRMax  = nullptr;  // 错色: R <= 1.3 x 板面
    QDoubleSpinBox* m_maskVMul      = nullptr;  // 金层 HSV 掩膜 V 下界
};

} // namespace w7
