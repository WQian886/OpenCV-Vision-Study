#include "count_check.h"
#include <QSpinBox>

namespace w7 {

QJsonObject CountCheck::defaultParams() const {
    QJsonObject p;
    p["expected_min"] = 1;
    p["expected_max"] = 100;
    return p;
}

QJsonObject CountCheck::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    int actual      = inputs.value("count").toInt();
    int expectedMin = params.value("expected_min").toInt(0);
    int expectedMax = params.value("expected_max").toInt(999);

    bool pass = (actual >= expectedMin) && (actual <= expectedMax);
    output["result"]       = pass ? "PASS" : "FAIL";
    output["actual"]       = actual;
    output["expected_min"] = expectedMin;
    output["expected_max"] = expectedMax;
    output["description"]  = pass
        ? QString("数量 %1 在 [%2, %3] 内").arg(actual).arg(expectedMin).arg(expectedMax)
        : QString("数量 %1 不在 [%2, %3] 内").arg(actual).arg(expectedMin).arg(expectedMax);
    return output;
}

QWidget* CountCheck::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new CountCheckForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

CountCheckForm::CountCheckForm(QWidget* parent) : ParamForm(parent) {}

void CountCheckForm::buildUI() {
    m_min = new QSpinBox;
    m_min->setRange(0, 99999);
    m_min->setValue(1);

    m_max = new QSpinBox;
    m_max->setRange(0, 99999);
    m_max->setValue(100);

    m_layout->addRow(tr("期望最小值"), m_min);
    m_layout->addRow(tr("期望最大值"), m_max);
}

void CountCheckForm::setParams(const QJsonObject& params) {
    if (params.contains("expected_min")) m_min->setValue(params["expected_min"].toInt());
    if (params.contains("expected_max")) m_max->setValue(params["expected_max"].toInt());
}

QJsonObject CountCheckForm::collect() const {
    QJsonObject p;
    p["expected_min"] = m_min->value();
    p["expected_max"] = m_max->value();
    return p;
}

} // namespace w7
