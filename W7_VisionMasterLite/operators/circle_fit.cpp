#include "circle_fit.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include "artifact_path.h"
#include <opencv2/imgproc.hpp>

namespace w7 {

QJsonObject CircleFit::defaultParams() const {
    QJsonObject p;
    p["min_area"]        = 100;
    // 圆度下限:1.0 是完美圆,越小越"像圆"。
    // 默认 0.7 是经验值 —— 低于这个值的多边形/方块会被滤掉。
    p["min_circularity"] = 0.70;
    return p;
}

QJsonObject CircleFit::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    // 输入是 mask(上一级 color_segment 的输出),不是原图:
    // 上一步已经分割好了,这一步只量形状,不关心颜色。
    // 没有 mask_path 就退回读原图当 mask 用 —— 让这个算子
    // 单独接图像源也能跑,方便调试。
    QString maskPath = inputs.value("mask_path").toString();
    if (maskPath.isEmpty()) maskPath = inputs.value("image_path").toString();

    if (maskPath.isEmpty()) {
        output["error"] = QObject::tr("需要 mask_path 或 image_path,两个都没拿到");
        return output;
    }

    cv::Mat mask = cv::imread(maskPath.toStdString(), cv::IMREAD_GRAYSCALE);
    if (mask.empty()) {
        output["error"] = "imread failed: " + maskPath;
        return output;
    }

    const int    minArea = params.value("min_area").toInt(100);
    const double minCirc = params.value("min_circularity").toDouble(0.70);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    QJsonArray circles;
    cv::Mat canvas;          // 标注图,画完再写盘
    const bool wantDraw = !inputs.value("image_path").toString().isEmpty();

    for (const auto& ctr : contours) {
        const double area = std::fabs(cv::contourArea(ctr));
        if (area < minArea) continue;

        const double perim = cv::arcLength(ctr, true);
        // 周长为 0 的话 4π·area/perim² 会除零炸掉。
        // 什么情况会是 0?一个像素点的退化轮廓 —— 不是理论情况,
        // 小 min_area 时真的会遇到。
        if (perim <= 0.0) continue;

        // 圆度:正圆 = 1.0,越不像圆越小。
        // 这是"判断这团东西是不是圆"的唯一依据 —— 也正因如此
        // 它是最容易被搞错的地方,见 test_circle_fit.cpp 用例 2。
        const double circularity = 4.0 * CV_PI * area / (perim * perim);
        if (circularity < minCirc) continue;

        cv::Point2f center;
        float radius = 0.f;
        cv::minEnclosingCircle(ctr, center, radius);

        QJsonObject c;
        c["cx"]           = center.x;
        c["cy"]           = center.y;
        c["radius"]       = radius;
        c["diameter"]     = radius * 2.0;
        c["area"]         = area;
        c["circularity"]  = circularity;
        circles.append(c);

        if (wantDraw) {
            if (canvas.empty())
                canvas = cv::imread(inputs.value("image_path").toString().toStdString(),
                                    cv::IMREAD_COLOR);
            if (!canvas.empty()) {
                cv::circle(canvas, cv::Point(cvRound(center.x), cvRound(center.y)),
                           cvRound(radius), cv::Scalar(0, 200, 255), 2);
                cv::putText(canvas,
                            QString("R=%.1f C=%.2f").arg(radius).arg(circularity).toStdString(),
                            cv::Point(cvRound(center.x - 30), cvRound(center.y - 5)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 200, 255), 1);
            }
        }
    }

    output["circles"] = circles;
    output["count"]   = circles.size();
    if (circles.isEmpty()) {
        // 找 0 个圆不算 error:可能本来就没有,可能阈值调高了。
        // 给个 warning,让用户知道"跑了,但没找到"和"崩了"是两回事。
        output["warning"] = QObject::tr("没找到满足条件的圆(最小面积 %1,最小圆度 %2)")
                               .arg(minArea).arg(minCirc, 0, 'f', 2);
    }

    if (!canvas.empty()) {
        // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
        // 理由见 artifact_path.h 的说明。
        const QString outPath = w7::artifactPath(
            "circle_fit",
            inputs.value("node_uuid").toString(),
            inputs.value("frame_index").toInt(-1));
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        cv::imwrite(outPath.toStdString(), canvas);
        output["image_path"] = outPath;
    }
    return output;
}

QWidget* CircleFit::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new CircleFitForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

// ============================================================
// === 参数面板 ===
// ============================================================

CircleFitForm::CircleFitForm(QWidget* parent) : ParamForm(parent) {}

void CircleFitForm::buildUI() {
    m_minArea = new QSpinBox;
    m_minArea->setRange(1, 1000000);
    m_minArea->setValue(100);
    m_minArea->setSingleStep(10);
    m_layout->addRow(tr("最小面积"), m_minArea);

    m_minCircularity = new QDoubleSpinBox;
    m_minCircularity->setRange(0.0, 1.0);
    m_minCircularity->setDecimals(2);
    m_minCircularity->setSingleStep(0.05);
    m_minCircularity->setValue(0.70);
    m_layout->addRow(tr("最小圆度(1.0=正圆)"), m_minCircularity);
}

void CircleFitForm::setParams(const QJsonObject& params) {
    if (params.contains("min_area"))
        m_minArea->setValue(params.value("min_area").toInt(100));
    if (params.contains("min_circularity"))
        m_minCircularity->setValue(params.value("min_circularity").toDouble(0.70));
}

QJsonObject CircleFitForm::collect() const {
    QJsonObject p;
    p["min_area"]        = m_minArea->value();
    p["min_circularity"] = m_minCircularity->value();
    return p;
}

} // namespace w7
