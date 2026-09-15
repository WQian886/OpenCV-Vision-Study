#include "template_match.h"
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <opencv2/imgproc.hpp>

namespace w7 {

QJsonObject TemplateMatch::defaultParams() const {
    QJsonObject p;
    p["template_path"] = "templates/R.png";
    p["method"]        = cv::TM_CCOEFF_NORMED;
    p["threshold"]     = 0.9;
    return p;
}

QJsonObject TemplateMatch::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    QString tplPath = inputs.value("template_path").toString();
    if (tplPath.isEmpty()) tplPath = params.value("template_path").toString();

    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    cv::Mat tpl = cv::imread(tplPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty() || tpl.empty()) {
        output["error"] = "imread failed (img or template)";
        return output;
    }

    int method = params.value("method").toInt(cv::TM_CCOEFF_NORMED);
    double threshold = params.value("threshold").toDouble(0.9);

    cv::Mat result;
    cv::matchTemplate(img, tpl, result, method);
    cv::Point minLoc, maxLoc;
    double minVal, maxVal;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    bool useMax = (method == cv::TM_CCOEFF_NORMED || method == cv::TM_CCOEFF
                || method == cv::TM_CCORR_NORMED || method == cv::TM_CCORR);
    double bestScore = useMax ? maxVal : -minVal;
    cv::Point bestLoc = useMax ? maxLoc : minLoc;

    output["position_x"] = bestLoc.x;
    output["position_y"] = bestLoc.y;
    output["score"]      = bestScore;
    output["pass"]       = (bestScore >= threshold);
    output["template_w"] = tpl.cols;
    output["template_h"] = tpl.rows;
    return output;
}

QWidget* TemplateMatch::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new TemplateMatchForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

TemplateMatchForm::TemplateMatchForm(QWidget* parent) : ParamForm(parent) {}

void TemplateMatchForm::buildUI() {
    m_tplPath = new QLineEdit("templates/R.png");

    m_method = new QComboBox;
    m_method->addItem("TM_CCOEFF_NORMED (推荐)", cv::TM_CCOEFF_NORMED);
    m_method->addItem("TM_CCORR_NORMED",          cv::TM_CCORR_NORMED);
    m_method->addItem("TM_SQDIFF_NORMED",         cv::TM_SQDIFF_NORMED);

    m_threshold = new QDoubleSpinBox;
    m_threshold->setRange(0.0, 1.0);
    m_threshold->setSingleStep(0.05);
    m_threshold->setValue(0.9);

    m_layout->addRow(tr("模板路径"), m_tplPath);
    m_layout->addRow(tr("匹配方法"), m_method);
    m_layout->addRow(tr("相似度阈值"), m_threshold);
}

void TemplateMatchForm::setParams(const QJsonObject& params) {
    if (params.contains("template_path")) m_tplPath->setText(params["template_path"].toString());
    if (params.contains("method")) {
        int method = params["method"].toInt();
        for (int i = 0; i < m_method->count(); ++i) {
            if (m_method->itemData(i).toInt() == method) {
                m_method->setCurrentIndex(i);
                break;
            }
        }
    }
    if (params.contains("threshold")) m_threshold->setValue(params["threshold"].toDouble());
}

QJsonObject TemplateMatchForm::collect() const {
    QJsonObject p;
    p["template_path"] = m_tplPath->text();
    p["method"]        = m_method->currentData().toInt();
    p["threshold"]     = m_threshold->value();
    return p;
}

} // namespace w7
