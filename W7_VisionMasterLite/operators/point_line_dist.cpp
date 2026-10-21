#include "point_line_dist.h"
#include "operator_defaults.h"
#include "param_read.h"
#include <QSpinBox>
#include <cmath>
#include <QDir>
#include <QFileInfo>
#include "artifact_path.h"
#include <opencv2/imgproc.hpp>

namespace w7 {

// 坐标旋钮名 —— **算子和面板共用这一份**。
// 以前是两份(Form::kKeys + run() 里的字面量),漏改一处就是
// "面板配的键"和"算子读的键"对不上,而且不报错。
namespace {
const QStringList kCoordKeys = {"ax", "ay", "bx", "by", "cx", "cy"};
}   // namespace

QJsonObject PointLineDist::defaultParams() const {
    QJsonObject p;
    p["ax"] = PointLineDistDefaults::kAx;  p["ay"] = PointLineDistDefaults::kAy;  // 基准线起点
    p["bx"] = PointLineDistDefaults::kBx;  p["by"] = PointLineDistDefaults::kBy;  // 基准线终点
    p["cx"] = PointLineDistDefaults::kCx;  p["cy"] = PointLineDistDefaults::kCy;  // 被测点
    return p;
}

QJsonObject PointLineDist::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    // 坐标旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    // 兜底值来自 defaultParams() —— 声明值只有一份。
    // ⚠ 以前读的是 toDouble(默认值),而面板上是 QSpinBox(整数) ——
    //   面板和算子对"这个旋钮是什么类型"本来就没对齐。现在统一成整数。
    QVector<int> c;
    QString err;
    if (!readIntKnobs(params, kCoordKeys, defaultParams(), &c, &err)) {
        output["error"] = err;
        return output;
    }
    const int ax = c.at(0), ay = c.at(1);
    const int bx = c.at(2), by = c.at(3);
    const int cx = c.at(4), cy = c.at(5);

    // 沿用 caliper_dist 的公式 + 退化处理
    const double abx = bx - ax, aby = by - ay;
    const double baseLen = std::sqrt(abx * abx + aby * aby);
    output["base_length"] = baseLen;

    // distance = |AB × AC| / |AB|
    if (baseLen <= 0.0) {
        // A、B 重合 → 基准线方向未定
        // 退化成"直接量 A→C",这在用户还没拉开基准线时
        // 至少给个数,比什么都不给好。给 warning 不给 error。
        output["distance"] = std::sqrt((cx - ax) * (cx - ax)
                                     + (cy - ay) * (cy - ay));
        output["warning"]  = QObject::tr("A、B 两点重合,基准线方向未定,"
                                          "已退化为直接量 A→C 的距离");
    } else {
        const double acx = cx - ax, acy = cy - ay;
        const double cross = std::fabs(abx * acy - aby * acx);
        output["distance"] = cross / baseLen;

        // 垂足:被测点 C 在基准线 AB 上的投影
        const double t = (acx * abx + acy * aby) / (baseLen * baseLen);
        output["foot_x"] = ax + abx * t;
        output["foot_y"] = ay + aby * t;
        output["ratio"]  = t;
    }

    output["a"] = QJsonObject{{"x", ax}, {"y", ay}};
    output["b"] = QJsonObject{{"x", bx}, {"y", by}};
    output["c"] = QJsonObject{{"x", cx}, {"y", cy}};

    // ---- 标注图(可选) ----
    QString imgPath = inputs.value("image_path").toString();
    if (!imgPath.isEmpty()) {
        cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
        if (img.empty()) {
            output["error"] = "imread failed: " + imgPath;
            return output;
        }
        const cv::Point A(cvRound(ax), cvRound(ay));
        const cv::Point B(cvRound(bx), cvRound(by));
        const cv::Point C(cvRound(cx), cvRound(cy));
        cv::line(img, A, B, cv::Scalar(255, 200, 0), 2);
        cv::line(img, C, C, cv::Scalar(0, 200, 255), 2);  // 点自身(无连线)
        if (baseLen > 0.0) {
            const cv::Point F(cvRound(output.value("foot_x").toDouble()),
                              cvRound(output.value("foot_y").toDouble()));
            cv::line(img, C, F, cv::Scalar(0, 255, 0), 1);
            cv::circle(img, F, 3, cv::Scalar(0, 255, 0), -1);
        }
        cv::circle(img, A, 4, cv::Scalar(255, 200, 0), -1);
        cv::circle(img, B, 4, cv::Scalar(255, 200, 0), -1);
        cv::circle(img, C, 4, cv::Scalar(0, 200, 255), -1);
        cv::putText(img,
                    QString("D=%.1f").arg(output.value("distance").toDouble()).toStdString(),
                    cv::Point(cvRound(cx) + 6, cvRound(cy) - 6),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 200, 255), 1);
        // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
        // 理由见 artifact_path.h 的说明。
        const QString outPath = w7::artifactPath(
            "point_line_dist",
            inputs.value("node_uuid").toString(),
            inputs.value("frame_index").toInt(-1));
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        cv::imwrite(outPath.toStdString(), img);
        output["image_path"] = outPath;
    }

    return output;
}

QWidget* PointLineDist::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new PointLineDistForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// === 参数面板(沿用 caliper_dist 的"6 个坐标循环"模板) ===
// ============================================================

PointLineDistForm::PointLineDistForm(QWidget* parent) : ParamForm(parent) {}

void PointLineDistForm::buildUI() {
    const QString labels[6] = {
        tr("A(基准起点) X"), tr("A(基准起点) Y"),
        tr("B(基准终点) X"), tr("B(基准终点) Y"),
        tr("C(被测点) X"),   tr("C(被测点) Y")};
    for (int i = 0; i < 6; ++i) {
        m_p[i] = new QSpinBox;
        m_p[i]->setRange(-20000, 20000);
        m_p[i]->setSingleStep(5);
        m_layout->addRow(labels[i], m_p[i]);
    }
    setParams(PointLineDist().defaultParams());
}

void PointLineDistForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**,不是 0。
    // ⚠ 以前是 toInt(0) —— 和声明值不一致的第二份副本。
    const QJsonObject d = PointLineDist().defaultParams();
    for (int i = 0; i < 6; ++i)
        if (params.contains(kCoordKeys.at(i)))
            m_p[i]->setValue(knobValueForWidget(params.value(kCoordKeys.at(i)),
                                                d, kCoordKeys.at(i)));
}

QJsonObject PointLineDistForm::collect() const {
    QJsonObject p;
    for (int i = 0; i < 6; ++i)
        p[kCoordKeys.at(i)] = m_p[i]->value();
    return p;
}

} // namespace w7