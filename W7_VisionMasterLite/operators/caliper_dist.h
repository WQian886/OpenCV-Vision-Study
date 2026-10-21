#pragma once
// ============================================================
// caliper_dist — 卡尺距离算子(对标 VM "距离测量 / 标卡尺")
//
// 测量类第三个。卡尺是工业上用得最多的测量工具:
// 一条边当基准,量另一条边到它的距离。
//
// 本算子做的是卡尺的**几何核心**,不是 VM 那个交互式卡尺工具:
//   给三个点 A、B、C ——
//     A、B 定义基准线(相当于卡尺的一只脚卡在边上)
//     量 C 到 AB 的**垂直距离**(相当于另一只脚读数)
// 真 VM 的卡尺还要处理选边、亚像素拟合、点歪了怎么办,
// 那是交互工具的复杂度,不是测量的复杂度。先把测量的核心做对。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class CaliperDist : public OperatorBase {
public:
    QString name() const override { return "caliper_dist"; }
    QString displayName() const override { return "卡尺距离"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "三点卡尺:AB 定基准,量 C 到 AB 的垂直距离";
    }
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class CaliperDistForm : public ParamForm {
    Q_OBJECT
public:
    explicit CaliperDistForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    QSpinBox* m_p[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    // 旋钮名在 caliper_dist.cpp 的 kCoordKeys 里 —— 算子和面板共用一份,
    // 以前这里是 Form::kKeys,和 run() 里的字面量是**两份**。
};

} // namespace w7
