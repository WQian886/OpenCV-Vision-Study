#include "blob_count.h"
#include "operator_defaults.h"
#include <QSpinBox>

namespace w7 {

QJsonObject BlobCount::defaultParams() const {
    QJsonObject p;
    p["min_area"] = BlobCountDefaults::kMinArea;
    return p;
}

QJsonObject BlobCount::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString maskPath = inputs.value("mask_path").toString();
    cv::Mat mask = cv::imread(maskPath.toStdString(), cv::IMREAD_GRAYSCALE);
    if (mask.empty()) {
        output["error"] = "imread failed: " + maskPath;
        return output;
    }

    // 兜底以前是 50,和 defaultParams() 声明的 20 不是一回事 ——
    // 参数 JSON 缺 min_area 时,算子按 50 滤,面板上写的是 20。
    int minArea = params.value("min_area").toInt(BlobCountDefaults::kMinArea);

    cv::Mat labels, stats, centroids;
    int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids);

    int count = 0;
    int totalArea = 0;
    for (int i = 1; i < n; ++i) {
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area >= minArea) {
            ++count;
            totalArea += area;
        }
    }

    output["count"] = count;
    output["total_area"] = totalArea;
    return output;
}

QWidget* BlobCount::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new BlobCountForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

BlobCountForm::BlobCountForm(QWidget* parent) : ParamForm(parent) {}

void BlobCountForm::buildUI() {
    m_minArea = new QSpinBox;
    m_minArea->setRange(1, 100000);
    m_minArea->setValue(BlobCountDefaults::kMinArea);
    m_minArea->setSingleStep(10);
    m_layout->addRow(tr("最小面积"), m_minArea);
}

void BlobCountForm::setParams(const QJsonObject& params) {
    if (params.contains("min_area")) {
        m_minArea->setValue(params["min_area"].toInt());
    }
}

QJsonObject BlobCountForm::collect() const {
    QJsonObject p;
    p["min_area"] = m_minArea->value();
    return p;
}

} // namespace w7
