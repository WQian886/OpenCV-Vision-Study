#pragma once
// ============================================================
// color_segment — HSV 颜色分割算子(对标 VM "颜色分割")
// 输入:image (cv::Mat BGR)
// 输出:mask (cv::Mat 单通道二值)
// params: hsv_low[h,s,v], hsv_high[h,s,v]
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class ColorSegment : public OperatorBase {
public:
    QString name() const override { return "color_segment"; }
    QString displayName() const override { return "颜色分割"; }
    QString category() const override { return "detection"; }
    QString description() const override {
        return "在 HSV 空间按颜色范围提取像素,输出二值 mask(对应 W2 inRange)";
    }
    // 吃图、吐 mask(mask 仍是图,所以输出还是 Image;
    // 下游 blob_count 要的就是它)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Image; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === 颜色分割参数面板 ===
class ColorSegmentForm : public ParamForm {
    Q_OBJECT
public:
    explicit ColorSegmentForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QSpinBox* m_hLow = nullptr;
    QSpinBox* m_sLow = nullptr;
    QSpinBox* m_vLow = nullptr;
    QSpinBox* m_hHigh = nullptr;
    QSpinBox* m_sHigh = nullptr;
    QSpinBox* m_vHigh = nullptr;
};

} // namespace w7
