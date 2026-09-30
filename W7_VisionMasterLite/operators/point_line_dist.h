#pragma once
// ============================================================
// point_line_dist — 点到线距离算子(对标 VM "点-线距离")
//
// 测量类第五个。给三个点 A、B、C:
//   A、B 定义一条基准线
//   量 C 到 AB 的垂直距离
//
// 公式与 caliper_dist 一样(叉积/模),但语义独立:
// caliper_dist 是"工业卡尺",要在图上画卡尺脚 + 垂足 + 标注;
// point_line_dist 是"几何测量",不需要图、只吐数值。
// 复用同一条公式 → 复用同一条测试(见 test_measurement.cpp 用例 5)。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class PointLineDist : public OperatorBase {
public:
    QString name() const override { return "point_line_dist"; }
    QString displayName() const override { return "点到线距离"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "给三点(A、B、C):AB 定基准,量 C 到 AB 的垂直距离(像素)";
    }
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class PointLineDistForm : public ParamForm {
    Q_OBJECT
public:
    explicit PointLineDistForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    QSpinBox* m_p[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    static const char* kKeys[6];
};

} // namespace w7