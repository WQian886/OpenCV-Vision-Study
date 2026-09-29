#pragma once
// ============================================================
// circle_fit — 圆形测量算子(对标 VM "圆查找 / 圆形测量")
//
// 测量类第二个。上一个 line_fit 量的是"一条线多长",
// 这个量的是"一个圆多大" —— 工业上量孔径、量圆形工件都靠它。
//
// 和 line_fit 的分工:
//   line_fit   给两个点 → 长度 + 角度   (纯几何,不需要图)
//   circle_fit 给一张二值图 → 找出里面所有圆 (要图,因为要"找")
//
// 所以 circle_fit 的输入是 mask(color_segment 的输出),
// 不是原图 —— 上一步已经分割好了,这一步只量形状。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;
class QDoubleSpinBox;

namespace w7 {

class CircleFit : public OperatorBase {
public:
    QString name() const override { return "circle_fit"; }
    QString displayName() const override { return "圆形测量"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "在二值图里找出圆形目标,量出圆心、半径和圆度";
    }
    // 吃图(mask),吐数据(圆清单)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class CircleFitForm : public ParamForm {
    Q_OBJECT
public:
    explicit CircleFitForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    QSpinBox*       m_minArea = nullptr;
    QDoubleSpinBox* m_minCircularity = nullptr;
};

} // namespace w7
