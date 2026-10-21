#include "angle.h"
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
const QStringList kCoordKeys = {"x1", "y1", "x2", "y2",
                                "x3", "y3", "x4", "y4"};
}   // namespace

namespace {
// 单条线段的角度(0~180°),与 line_fit 完全一致的归一化:
//   atan2 返回 (-180, 180],负数 +180,恰好的 180 -180
// 抽出来方便后续两个 angle 调用复用,避免把同一条约定写两遍
// 写两遍 = 哪天改约定只改了一边 → 静默不一致
double lineAngleDeg(double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    if (dx == 0.0 && dy == 0.0) return 0.0;   // 退化(两点重合) → 0
    double a = std::atan2(dy, dx) * 180.0 / CV_PI;
    if (a < 0.0)     a += 180.0;   // (-180,0) → (0,180)
    if (a >= 180.0)  a -= 180.0;   // 180 和 0 是同一条线
    return a;
}
} // namespace

QJsonObject Angle::defaultParams() const {
    QJsonObject p;
    // 默认两条垂直线段:水平线 AB 和垂直线 CD,夹角应 = 90°
    p["x1"] = AngleDefaults::kX1;  p["y1"] = AngleDefaults::kY1;   // 线 1 起点
    p["x2"] = AngleDefaults::kX2;  p["y2"] = AngleDefaults::kY2;   // 线 1 终点(水平)
    p["x3"] = AngleDefaults::kX3;  p["y3"] = AngleDefaults::kY3;   // 线 2 起点
    p["x4"] = AngleDefaults::kX4;  p["y4"] = AngleDefaults::kY4;   // 线 2 终点(垂直)
    return p;
}

QJsonObject Angle::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    // 坐标旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    // 兜底值来自 defaultParams() —— 声明值只有一份。
    QVector<int> c;
    QString err;
    if (!readIntKnobs(params, kCoordKeys, defaultParams(), &c, &err)) {
        output["error"] = err;
        return output;
    }
    const int x1 = c.at(0), y1 = c.at(1);
    const int x2 = c.at(2), y2 = c.at(3);
    const int x3 = c.at(4), y3 = c.at(5);
    const int x4 = c.at(6), y4 = c.at(7);

    const double a1 = lineAngleDeg(x1, y1, x2, y2);
    const double a2 = lineAngleDeg(x3, y3, x4, y4);

    output["line1_angle_deg"] = a1;
    output["line2_angle_deg"] = a2;

    // 夹角 = |a1 - a2|,然后折半到 0~90°
    // 例: 0° 和 30° → 夹角 30°
    //     0° 和 150° → 夹角 150° → 折成 30°
    //     10° 和 100° → 夹角 90°
    //     10° 和 170° → 夹角 160° → 折成 20°

    // 退化情形:任一线段两点重合 → 夹角定义为 0
    //
    // 必须在这里就把 diff 钉死成 0,不能指望 diff = |a1 - a2| 自然算对:
    // lineAngleDeg 退化时返回的 0 和"一条真实的 0° 水平线"在数值上完全一样,
    // 于是 |0 - a2| 会算出一个看起来完全正常的夹角(实测:线2 为 45° 时算出 45°),
    // 而不是约定的 0 —— 这比直接报错更难发现,下游拿到的是"假但合理"的数据。
    const bool line1Degenerate = (x1 == x2 && y1 == y2);
    const bool line2Degenerate = (x3 == x4 && y3 == y4);

    double diff;
    if (line1Degenerate || line2Degenerate) {
        diff = 0.0;
        output["warning"] = QObject::tr("线段两点重合,角度定义为 0");
    } else {
        diff = std::fabs(a1 - a2);
        if (diff > 90.0) diff = 180.0 - diff;   // 折半到 0~90°
    }
    output["angle_deg"] = diff;

    // 把四个端点回显给下游
    output["l1_p1"] = QJsonObject{{"x", x1}, {"y", y1}};
    output["l1_p2"] = QJsonObject{{"x", x2}, {"y", y2}};
    output["l2_p1"] = QJsonObject{{"x", x3}, {"y", y3}};
    output["l2_p2"] = QJsonObject{{"x", x4}, {"y", y4}};

    // ---- 标注图(可选) ----
    QString imgPath = inputs.value("image_path").toString();
    if (!imgPath.isEmpty()) {
        cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
        if (img.empty()) {
            output["error"] = "imread failed: " + imgPath;
            return output;
        }
        const cv::Point P1(x1, y1), P2(x2, y2);
        const cv::Point P3(x3, y3), P4(x4, y4);
        cv::line(img, P1, P2, cv::Scalar(0, 200, 255), 2);  // 线 1:黄
        cv::line(img, P3, P4, cv::Scalar(255, 200, 0), 2);  // 线 2:蓝
        cv::circle(img, P1, 4, cv::Scalar(0, 255, 0), -1);
        cv::circle(img, P2, 4, cv::Scalar(0, 0, 255), -1);
        cv::circle(img, P3, 4, cv::Scalar(255, 0, 255), -1);
        cv::circle(img, P4, 4, cv::Scalar(255, 255, 0), -1);
        // 标注角度值:放在两条线交点附近(粗略取中点)
        const cv::Point mid((std::min(std::min(x1, x2), std::min(x3, x4))
                          + std::max(std::max(x1, x2), std::max(x3, x4))) / 2,
                           (std::min(std::min(y1, y2), std::min(y3, y4))
                          + std::max(std::max(y1, y2), std::max(y3, y4))) / 2);
        cv::putText(img,
                    QString("angle=%.1f deg").arg(diff).toStdString(),
                    mid,
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);
        // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
        // 理由见 artifact_path.h 的说明。
        const QString outPath = w7::artifactPath(
            "angle",
            inputs.value("node_uuid").toString(),
            inputs.value("frame_index").toInt(-1));
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        cv::imwrite(outPath.toStdString(), img);
        output["image_path"] = outPath;
    }

    return output;
}

QWidget* Angle::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new AngleForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// === 参数面板(8 个坐标,沿用 line_fit 的"循环"模板) ===
// ============================================================

AngleForm::AngleForm(QWidget* parent) : ParamForm(parent) {}

void AngleForm::buildUI() {
    const QString labels[8] = {
        tr("线 1 起点 X"), tr("线 1 起点 Y"),
        tr("线 1 终点 X"), tr("线 1 终点 Y"),
        tr("线 2 起点 X"), tr("线 2 起点 Y"),
        tr("线 2 终点 X"), tr("线 2 终点 Y")};
    for (int i = 0; i < 8; ++i) {
        m_coord[i] = new QSpinBox;
        m_coord[i]->setRange(-20000, 20000);
        m_coord[i]->setSingleStep(5);
        m_layout->addRow(labels[i], m_coord[i]);
    }
    setParams(Angle().defaultParams());
}

void AngleForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**,不是 0。
    // ⚠ 以前是 toInt(0) —— 和声明值不一致的第二份副本。
    const QJsonObject d = Angle().defaultParams();
    for (int i = 0; i < 8; ++i) {
        if (params.contains(kCoordKeys.at(i)))
            m_coord[i]->setValue(knobValueForWidget(params.value(kCoordKeys.at(i)),
                                                    d, kCoordKeys.at(i)));
    }
}

QJsonObject AngleForm::collect() const {
    QJsonObject p;
    for (int i = 0; i < 8; ++i)
        p[kCoordKeys.at(i)] = m_coord[i]->value();
    return p;
}

} // namespace w7