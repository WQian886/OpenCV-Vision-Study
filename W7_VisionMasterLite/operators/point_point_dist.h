#pragma once
// ============================================================
// point_point_dist — 两点距离算子(对标 VM "点-点距离")
//
// 测量类第四个。给两个点,直接报出欧氏距离。
// 几何上最简单的一个:√((x2-x1)² + (y2-y1)²)。
//
// 沿用 line_fit / caliper_dist 的"不要图也能算"约定:
// 没有 image_path 也能报 distance,有图就画一条两端点连线
// 让人看见量的哪两个点。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class PointPointDist : public OperatorBase {
public:
    QString name() const override { return "point_point_dist"; }
    QString displayName() const override { return "两点距离"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "给两个点,量出它们的欧氏距离(像素)";
    }
    // 吃图(画标注用),吐数据(距离)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class PointPointDistForm : public ParamForm {
    Q_OBJECT
public:
    explicit PointPointDistForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    QSpinBox* m_coord[4] = {nullptr, nullptr, nullptr, nullptr};
    static const char* kKeys[4];
};

} // namespace w7