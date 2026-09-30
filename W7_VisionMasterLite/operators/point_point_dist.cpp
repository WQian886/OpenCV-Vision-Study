#include "point_point_dist.h"
#include <QSpinBox>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace w7 {

QJsonObject PointPointDist::defaultParams() const {
    QJsonObject p;
    p["x1"] = 100;  p["y1"] = 100;
    p["x2"] = 200;  p["y2"] = 200;
    return p;
}

QJsonObject PointPointDist::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    const int x1 = params.value("x1").toInt(0);
    const int y1 = params.value("y1").toInt(0);
    const int x2 = params.value("x2").toInt(0);
    const int y2 = params.value("y2").toInt(0);

    const double dx = double(x2 - x1);
    const double dy = double(y2 - y1);
    const double distance = std::sqrt(dx * dx + dy * dy);

    output["distance"] = distance;
    output["p1"] = QJsonObject{{"x", x1}, {"y", y1}};
    output["p2"] = QJsonObject{{"x", x2}, {"y", y2}};

    // 两点重合 → 距离为 0,不算 error(沿用 line_fit / caliper_dist 约定):
    // 用户在参数面板还没拉开两个点,流水线本来就该照常跑完。
    if (distance <= 0.0) {
        output["warning"] = QObject::tr("两个点重合,距离为 0");
    }

    // ---- 标注图(可选,沿用 line_fit 约定:没有图照样给测量结果) ----
    QString imgPath = inputs.value("image_path").toString();
    if (!imgPath.isEmpty()) {
        cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
        if (img.empty()) {
            // 给了路径却读不出来 → 真问题(路径写错/文件被删),报 error
            output["error"] = "imread failed: " + imgPath;
            return output;
        }
        cv::line(img, cv::Point(x1, y1), cv::Point(x2, y2),
                 cv::Scalar(0, 200, 255), 2);
        cv::circle(img, cv::Point(x1, y1), 4, cv::Scalar(0, 255, 0), -1);
        cv::circle(img, cv::Point(x2, y2), 4, cv::Scalar(0, 0, 255), -1);
        cv::putText(img,
                    QString("D=%.1f").arg(distance).toStdString(),
                    cv::Point((x1 + x2) / 2 + 6, (y1 + y2) / 2 - 6),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 200, 255), 1);
        const QString outPath = "images/w7_point_point_dist.png";
        cv::imwrite(outPath.toStdString(), img);
        output["image_path"] = outPath;
    }

    return output;
}

QWidget* PointPointDist::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new PointPointDistForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// === 参数面板(沿用 line_fit 的"4 个坐标循环"模板) ===
// ============================================================

const char* PointPointDistForm::kKeys[4] = {"x1", "y1", "x2", "y2"};

PointPointDistForm::PointPointDistForm(QWidget* parent) : ParamForm(parent) {}

void PointPointDistForm::buildUI() {
    const QString labels[4] = {tr("点 A X"), tr("点 A Y"),
                               tr("点 B X"), tr("点 B Y")};
    for (int i = 0; i < 4; ++i) {
        m_coord[i] = new QSpinBox;
        m_coord[i]->setRange(-20000, 20000);
        m_coord[i]->setSingleStep(5);
        m_layout->addRow(labels[i], m_coord[i]);
    }
    setParams(PointPointDist().defaultParams());
}

void PointPointDistForm::setParams(const QJsonObject& params) {
    for (int i = 0; i < 4; ++i) {
        if (params.contains(kKeys[i]))
            m_coord[i]->setValue(params.value(kKeys[i]).toInt(0));
    }
}

QJsonObject PointPointDistForm::collect() const {
    QJsonObject p;
    for (int i = 0; i < 4; ++i)
        p[kKeys[i]] = m_coord[i]->value();
    return p;
}

} // namespace w7