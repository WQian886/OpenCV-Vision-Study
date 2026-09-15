#include "color_segment.h"
#include <QSpinBox>
#include <QJsonArray>

namespace w7 {

QJsonObject ColorSegment::defaultParams() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{10, 80, 80};
    p["hsv_high"] = QJsonArray{40, 255, 255};
    return p;
}

QJsonObject ColorSegment::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    QJsonArray lo = params.value("hsv_low").toArray();
    QJsonArray hi = params.value("hsv_high").toArray();
    cv::Scalar low(lo[0].toInt(), lo[1].toInt(), lo[2].toInt());
    cv::Scalar high(hi[0].toInt(), hi[1].toInt(), hi[2].toInt());

    cv::Mat hsv, mask;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, low, high, mask);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);

    QString outPath = "images/w7_color_segment_mask.png";
    cv::imwrite(outPath.toStdString(), mask);

    output["mask_path"]  = outPath;
    output["pixel_count"] = static_cast<int>(cv::countNonZero(mask));
    return output;
}

QWidget* ColorSegment::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new ColorSegmentForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

ColorSegmentForm::ColorSegmentForm(QWidget* parent) : ParamForm(parent) {}

void ColorSegmentForm::buildUI() {
    m_hLow  = new QSpinBox;  m_hLow->setRange(0, 179);   m_hLow->setValue(10);
    m_sLow  = new QSpinBox;  m_sLow->setRange(0, 255);   m_sLow->setValue(80);
    m_vLow  = new QSpinBox;  m_vLow->setRange(0, 255);   m_vLow->setValue(80);
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179);  m_hHigh->setValue(40);
    m_sHigh = new QSpinBox;  m_sHigh->setRange(0, 255);  m_sHigh->setValue(255);
    m_vHigh = new QSpinBox;  m_vHigh->setRange(0, 255);  m_vHigh->setValue(255);

    m_layout->addRow(tr("H 下限"),  m_hLow);
    m_layout->addRow(tr("S 下限"),  m_sLow);
    m_layout->addRow(tr("V 下限"),  m_vLow);
    m_layout->addRow(tr("H 上限"),  m_hHigh);
    m_layout->addRow(tr("S 上限"),  m_sHigh);
    m_layout->addRow(tr("V 上限"),  m_vHigh);
}

void ColorSegmentForm::setParams(const QJsonObject& params) {
    QJsonArray lo = params.value("hsv_low").toArray();
    QJsonArray hi = params.value("hsv_high").toArray();
    if (lo.size() >= 3) {
        m_hLow->setValue(lo[0].toInt());
        m_sLow->setValue(lo[1].toInt());
        m_vLow->setValue(lo[2].toInt());
    }
    if (hi.size() >= 3) {
        m_hHigh->setValue(hi[0].toInt());
        m_sHigh->setValue(hi[1].toInt());
        m_vHigh->setValue(hi[2].toInt());
    }
}

QJsonObject ColorSegmentForm::collect() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{m_hLow->value(),  m_sLow->value(),  m_vLow->value()};
    p["hsv_high"] = QJsonArray{m_hHigh->value(), m_sHigh->value(), m_vHigh->value()};
    return p;
}

} // namespace w7
