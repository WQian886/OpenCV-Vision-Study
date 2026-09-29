#pragma once
// ============================================================
// count_check — 数量判定算子(对标 VM "数量判定")
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class CountCheck : public OperatorBase {
public:
    QString name() const override { return "count_check"; }
    QString displayName() const override { return "数量判定"; }
    QString category() const override { return "detection"; }
    QString description() const override {
        return "判定数量是否在 [expected_min, expected_max] 区间内";
    }
    // 吃 count(数据),吐 PASS/FAIL(文本)
    //
    // 这条声明挡掉了一个会静默算错的连法:图像源直接接数量判定。
    // count_check 只读 inputs["count"],接图像源时拿不到 count,
    // actual 会是 0 → 判 FAIL,而日志上看不出任何异常。
    PinType inputPinType()  const override { return PinType::Data; }
    PinType outputPinType() const override { return PinType::Text; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class CountCheckForm : public ParamForm {
    Q_OBJECT
public:
    explicit CountCheckForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QSpinBox* m_min = nullptr;
    QSpinBox* m_max = nullptr;
};

} // namespace w7
