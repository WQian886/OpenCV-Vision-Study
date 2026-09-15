#pragma once
// ============================================================
// qr_decode — QR 解码算子(对标 VM "二维码识别")
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLabel;

namespace w7 {

class QRDecode : public OperatorBase {
public:
    QString name() const override { return "qr_decode"; }
    QString displayName() const override { return "QR 解码"; }
    QString category() const override { return "identification"; }
    QString description() const override {
        return "检测并解码图中的 QR 码";
    }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// QR 解码无参数,只显示一个说明
class QRDecodeForm : public ParamForm {
    Q_OBJECT
public:
    explicit QRDecodeForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
};

} // namespace w7
