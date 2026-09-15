#include "qr_decode.h"
#include <QJsonArray>
#include <QLabel>

namespace w7 {

QJsonObject QRDecode::defaultParams() const {
    QJsonObject p;   // 无参数
    return p;
}

QJsonObject QRDecode::run(const QJsonObject& /*params*/, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    cv::QRCodeDetector detector;
    std::vector<cv::Point2f> points;
    std::string decoded = detector.detectAndDecode(img, points);

    output["found"]  = !decoded.empty();
    output["decoded"] = QString::fromStdString(decoded);

    QJsonArray pts;
    for (const auto& p : points) {
        QJsonObject pt;
        pt["x"] = p.x;
        pt["y"] = p.y;
        pts.append(pt);
    }
    output["points"] = pts;
    return output;
}

QWidget* QRDecode::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new QRDecodeForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

QRDecodeForm::QRDecodeForm(QWidget* parent) : ParamForm(parent) {}

void QRDecodeForm::buildUI() {
    auto* label = new QLabel(tr("(QR 解码无需参数,自动检测图中的 QR 码)"));
    label->setWordWrap(true);
    label->setStyleSheet("color: gray;");
    m_layout->addRow(label);
}

QJsonObject QRDecodeForm::collect() const {
    return {};
}

} // namespace w7
