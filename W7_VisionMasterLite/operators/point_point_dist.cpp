#include "point_point_dist.h"
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
//
// ⚠ 以前这里是**两份**:Form::kKeys(面板 collect/setParams 用)
//   和 run() 里手写的 params.value("x1")(算子用)。
//   键名抄两份,迟早有一处漏改 —— 漏改的后果是
//   "面板配的键"和"算子读的键"不是同一个:
//   界面上调了不生效,而且**不报错**。
namespace {
const QStringList kCoordKeys = {"x1", "y1", "x2", "y2"};
}   // namespace

QJsonObject PointPointDist::defaultParams() const {
    QJsonObject p;
    p["x1"] = PointPointDistDefaults::kX1;  p["y1"] = PointPointDistDefaults::kY1;
    p["x2"] = PointPointDistDefaults::kX2;  p["y2"] = PointPointDistDefaults::kY2;
    return p;
}

QJsonObject PointPointDist::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    // 坐标旋钮:类型不对就报错,**不补默认值**。
    //
    // 兜底值来自 defaultParams() —— 声明值只有一份,
    // 不用再在这里手写第二份 .toInt(PointPointDistDefaults::kX1)。
    //
    // ⚠ 以前这里没有类型检查:params.value("x1").toInt(默认值) 在
    //   值是字符串 "100" 时返回**默认值**(Qt 不解析字符串),
    //   值是小数 25.7 时也返回默认值(不截断)。
    //   也就是说用户写进去的坐标被**整个丢掉**,算子拿默认值算,
    //   还报出一个看着正常的距离 —— 测量结果直接决定判废。
    QVector<int> c;
    QString err;
    if (!readIntKnobs(params, kCoordKeys, defaultParams(), &c, &err)) {
        output["error"] = err;
        return output;
    }
    const int x1 = c.at(0), y1 = c.at(1);
    const int x2 = c.at(2), y2 = c.at(3);

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
        // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
        // 理由见 artifact_path.h 的说明。
        const QString outPath = w7::artifactPath(
            "point_point_dist",
            inputs.value("node_uuid").toString(),
            inputs.value("frame_index").toInt(-1));
        QDir().mkpath(QFileInfo(outPath).absolutePath());
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
    // 兜底用**声明的默认值**,不是 0。
    //
    // ⚠ 以前是 toInt(0) —— 那是一份和声明值不一致的副本:
    //   存盘里 x1 存成了字符串时,面板显示 0 而不是声明的 100,
    //   用户看一眼不会觉得有问题,点一下"应用参数"就把 0 存进流水线,
    //   测量结果整个变了 —— 而界面上看不出任何异常。
    const QJsonObject d = PointPointDist().defaultParams();
    for (int i = 0; i < 4; ++i) {
        if (params.contains(kCoordKeys.at(i)))
            m_coord[i]->setValue(knobValueForWidget(params.value(kCoordKeys.at(i)),
                                                    d, kCoordKeys.at(i)));
    }
}

QJsonObject PointPointDistForm::collect() const {
    QJsonObject p;
    for (int i = 0; i < 4; ++i)
        p[kCoordKeys.at(i)] = m_coord[i]->value();
    return p;
}

} // namespace w7