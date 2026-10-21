#pragma once
// ============================================================
// angle — 两条直线夹角算子(对标 VM "角度测量")
//
// 测量类第六个。给四个点(A、B、C、D):
//   线段 1 = AB, 线段 2 = CD
//   量两线段之间的**夹角**,0~90°
//
// 角度约定(继承自 line_fit 的"无向"约定):
//   · 单条线段的角度归一化到 0~180°(沿用 line_fit 的 atan2 + 归一化)
//   · 两条线段的夹角取 0~90°(否则 30° 和 150° 算"不同"夹角,
//     但几何上"两条线的夹角"应该不分先后)
//   · 所以夹角 = |a1 - a2|,超过 90° 时折成 180° - x
//
// 公式短到一行,真正要测的是"无向约定"和"两点重合的退化"。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class Angle : public OperatorBase {
public:
    QString name() const override { return "angle"; }
    QString displayName() const override { return "两线夹角"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "给四个点(AB/CD 各定一条线),量两线之间的夹角(0~90°)";
    }
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class AngleForm : public ParamForm {
    Q_OBJECT
public:
    explicit AngleForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    QSpinBox* m_coord[8] = {nullptr};
    // 旋钮名在 angle.cpp 的 kCoordKeys 里 —— 算子和面板共用一份,
    // 以前这里是 Form::kKeys,和 run() 里的字面量是**两份**。
};

} // namespace w7