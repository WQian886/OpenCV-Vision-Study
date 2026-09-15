#pragma once
// ============================================================
// template_match — 模板匹配算子(对标 VM "模板匹配")
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLineEdit;
class QDoubleSpinBox;
class QComboBox;

namespace w7 {

class TemplateMatch : public OperatorBase {
public:
    QString name() const override { return "template_match"; }
    QString displayName() const override { return "模板匹配"; }
    QString category() const override { return "positioning"; }
    QString description() const override {
        return "在大图滑动模板,找最相似位置";
    }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class TemplateMatchForm : public ParamForm {
    Q_OBJECT
public:
    explicit TemplateMatchForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QLineEdit* m_tplPath = nullptr;
    QComboBox* m_method = nullptr;
    QDoubleSpinBox* m_threshold = nullptr;
};

} // namespace w7
