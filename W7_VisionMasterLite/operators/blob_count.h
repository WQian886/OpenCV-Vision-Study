#pragma once
// ============================================================
// blob_count — Blob 计数算子(对标 VM "Blob 分析")
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class BlobCount : public OperatorBase {
public:
    QString name() const override { return "blob_count"; }
    QString displayName() const override { return "Blob 计数"; }
    QString category() const override { return "detection"; }
    QString description() const override {
        return "对二值 mask 数连通域,过滤小于 min_area 的噪点";
    }
    // 吃 mask(是图),吐 count/total_area(是数据)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class BlobCountForm : public ParamForm {
    Q_OBJECT
public:
    explicit BlobCountForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QSpinBox* m_minArea = nullptr;
};

} // namespace w7
