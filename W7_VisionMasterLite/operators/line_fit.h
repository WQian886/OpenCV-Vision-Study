#pragma once
// ============================================================
// line_fit — 直线测量算子(对标 VM "线段测量 / line fit")
//
// 定位/测量/识别/检测 四大模块里,前三个 W7 都做了,
// 这个补的是"测量"这一类里最基础的一个:
// 给两个点,量出这条线的长度和角度。
//
// 为什么不用 cv::fitLine:
//   fitLine 是给一堆散点拟合一条线(最小二乘)。这里只有两个点,
//   两个点确定一条直线,没有"拟合"可言,直接算就够。
//   ——先把最简单的做对,再考虑要不要"拟合"。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QSpinBox;

namespace w7 {

class LineFit : public OperatorBase {
public:
    QString name() const override { return "line_fit"; }
    QString displayName() const override { return "直线测量"; }
    QString category() const override { return "measurement"; }
    QString description() const override {
        return "给两个点,量出直线的长度和角度(0~180°)";
    }
    // 吃图(画标注用),吐数据(长度/角度)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }

    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

class LineFitForm : public ParamForm {
    Q_OBJECT
public:
    explicit LineFitForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    void setParams(const QJsonObject& params) override;
    QJsonObject collect() const override;
private:
    // 4 个坐标输入框。存成数组,collect/setParams 里按 0..3 循环,
    // 免得四处各写一遍 x1/y1/x2/y2,改的时候漏掉一个
    QSpinBox* m_coord[4] = {nullptr, nullptr, nullptr, nullptr};
    static const char* kKeys[4];
};

} // namespace w7
