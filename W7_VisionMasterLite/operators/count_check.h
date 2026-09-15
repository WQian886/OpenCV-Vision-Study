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
