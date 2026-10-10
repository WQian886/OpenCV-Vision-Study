#pragma once
// ============================================================
// pcb_inspect — PCB 元件定位与缺陷检测(对标 VM "PCB 检测"方案)
//
// === 本算子的来历 ===
// 这是 W6 PCB 缺陷检测综合项目的能力,原样搬进 W7 算子体系。
// 之前 W6 是独立的命令行程序,和 W7 的流程图编辑器完全割裂 ——
// W6 改了阈值要重编,看不出它在流水线里的位置。
// 搬进来之后,PCB 检测就是流水线上的普通一环,可以和
// 颜色分割 / 模板匹配 / DL 推理任意组合。
//
// === 两种模式 ===
// known_site(默认):按**板型配方**的坐标直接构造 ROI
//   - 快、稳定,是 W6 验证过的路径
//   - 适合已知板型、固定工位的产线
//   - 板型真值在 recipes/*.json 里,换板子换一份文件,不改代码
// auto_contour:从金层 mask 做连通域自动发现元件
//   - 不依赖配方,但也**没有判定规则可跑**(元件没配 checks)
//   - 更接近真实产线的做法;要判定得先补一份配方
//
// 输入:image_path (上游图像源给)
// 输出:marked_path(画框图) + components[] + count + mode
// params: mode, recipe, hsv_*, mask_v_mul, min_area_pct
//
// === 配方里为什么既有坐标又有"查哪几条规则" ===
// 因为**这两件事必须同时换**。以前坐标写在 pcb_inspect 里、规则写在
// pcb_defect_judge 里(`if (label == "R2")` 七处),抄成两份 ——
// 换块板时坐标不对,那七处更是每一条都匹配不上,规则全空转。
// 两者放一份 JSON 里,它们就不可能各自漂开。
//
// === 注意 ===
// W6 的 7 类缺陷判定在 pcb_defect_judge 算子里,本算子只负责
// 「找到元件在哪」。拆成两个算子是刻意的:
//   - 定位和判定是两个独立关注点,阈值各自独立调
//   - VM 里定位/检测本来就是分开配的
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;
class QDoubleSpinBox;
class QComboBox;

namespace w7 {

class PcbInspect : public OperatorBase {
public:
    QString name() const override { return "pcb_inspect"; }
    QString displayName() const override { return "PCB 元件定位"; }
    QString category() const override { return "detection"; }
    QString description() const override {
        return "定位 PCB 上的元件(R/C/IC),支持真值坐标和自动轮廓两种模式"
               "(源自 W6 综合项目)";
    }
    // 吃图,吐元件清单(数据)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === PCB 元件定位参数面板 ===
class PcbInspectForm : public ParamForm {
    Q_OBJECT
public:
    explicit PcbInspectForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QComboBox* m_mode    = nullptr;
    QSpinBox* m_hLow     = nullptr;
    QSpinBox* m_sLow     = nullptr;
    QSpinBox* m_hHigh    = nullptr;
    // V 下界是"板面基色 x 多少倍",不是绝对 V,所以用小数框。
    // 原来这里还有 m_vLow / m_sHigh / m_vHigh 三个绝对值框 —— S 上限恒为 255
    // (inRange 的高端),V 上限也恒为 255,两个框从来调不出任何东西,
    // 一并去掉。V 下界改成倍数是实测逼出来的:绝对 V>=80 在亮度 0.4 倍时
    // 把金层切掉(实测 R1 的 gold_px 168 -> 144),相对式 0.4~1.6 全档平。
    QDoubleSpinBox* m_vMul      = nullptr;
    QDoubleSpinBox* m_minAreaPct = nullptr;
    // 板型配方路径。这是"换板型"的唯一开关 ——
    // 以前没有它,换板子只能改源码里那张写死的表。
    QComboBox* m_recipe = nullptr;
};

} // namespace w7
