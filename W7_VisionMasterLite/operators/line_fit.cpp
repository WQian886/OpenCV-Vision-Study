#include "line_fit.h"
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
const QStringList kCoordKeys = {"x1", "y1", "x2", "y2"};
}   // namespace

// ============================================================
// === 角度范围:本算子的核心设计决策 ===
//
// 一条直线**没有方向**。(10,0)→(0,0) 和 (0,0)→(10,0) 是同一条线,
// 角度报 0 还是 180?  选 0~180(无向),因为对标 VM 的 line fit
// 就是无向的,报有向角会让"量一条边"这件事出现两个答案。
//
// atan2 的返回范围是 (-180, 180],所以规整只要两步:
//   负数 +180  →  (-180, 0]  映射到 (0, 180]
//   恰好的 180 -180 → 0(和 0° 是同一条线)
// 这个归一化有专门的测试钉住(见 test_line_fit.cpp 用例 2),
// 改这里之前先去看那条测试 —— 它记的就是这个决策。
// ============================================================

QJsonObject LineFit::defaultParams() const {
    QJsonObject p;
    p["x1"] = LineFitDefaults::kX1;  p["y1"] = LineFitDefaults::kY1;
    p["x2"] = LineFitDefaults::kX2;  p["y2"] = LineFitDefaults::kY2;
    return p;
}

QJsonObject LineFit::run(const QJsonObject& params, const QJsonObject& inputs) {
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

    const double dx = double(x2 - x1);
    const double dy = double(y2 - y1);

    const double length = std::sqrt(dx * dx + dy * dy);

    double angle = 0.0;
    if (length > 0.0) {
        angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        if (angle < 0.0)     angle += 180.0;   // (-180,0) → (0,180)
        if (angle >= 180.0)  angle -= 180.0;   // 180 和 0 是同一条线
    }

    output["length"]    = length;
    output["angle_deg"] = angle;

    // 两点重合:长度 0、方向未定义。
    // 这不算 error —— 用户在参数面板上还没拉开距离,流水线本来
    // 就该照常跑完,只是量不出东西。给个 warning 让人知道就行。
    if (length <= 0.0) {
        output["warning"] = QObject::tr("两个点重合,无法确定直线方向(长度为 0)");
    }

    // 端点和中点回显:下游要用,标注图也要用
    output["p1"]   = QJsonObject{{"x", x1}, {"y", y1}};
    output["p2"]   = QJsonObject{{"x", x2}, {"y", y2}};
    output["mid"]  = QJsonObject{{"x", (x1 + x2) / 2.0},
                                 {"y", (y1 + y2) / 2.0}};

    // ---- 标注图(可选)----
    // 注意:image_path 为空时**不报错**,照样返回上面的测量结果。
    // 测量本身是纯几何,不需要图;图只是给人看的。
    // (测试用例 3 就靠这一点:不准备任何图片文件也能量线)
    QString imgPath = inputs.value("image_path").toString();
    if (!imgPath.isEmpty()) {
        cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
        if (img.empty()) {
            // 给了路径却读不出来 —— 这是真问题(路径写错/文件被删),
            // 跟"没给路径"不是一回事,所以这里才报 error
            output["error"] = "imread failed: " + imgPath;
            return output;
        }
        cv::line(img, cv::Point(x1, y1), cv::Point(x2, y2),
                 cv::Scalar(0, 200, 255), 2);
        cv::circle(img, cv::Point(x1, y1), 4, cv::Scalar(0, 255, 0), -1);
        cv::circle(img, cv::Point(x2, y2), 4, cv::Scalar(0, 0, 255), -1);
        cv::putText(img,
                    QString("L=%.1f  A=%.1f deg").arg(length).arg(angle).toStdString(),
                    cv::Point(x1, std::max(0, y1 - 8)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 200, 255), 1);
        // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
        // 理由见 artifact_path.h 的说明。
        const QString outPath = w7::artifactPath(
            "line_fit",
            inputs.value("node_uuid").toString(),
            inputs.value("frame_index").toInt(-1));
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        cv::imwrite(outPath.toStdString(), img);
        output["image_path"] = outPath;
    }

    return output;
}

QWidget* LineFit::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new LineFitForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

// ============================================================
// === 参数面板 ===
// ============================================================

LineFitForm::LineFitForm(QWidget* parent) : ParamForm(parent) {}
void LineFitForm::buildUI() {
    const QString labels[4] = {tr("起点 X"), tr("起点 Y"),
                               tr("终点 X"), tr("终点 Y")};
    for (int i = 0; i < 4; ++i) {
        m_coord[i] = new QSpinBox;
        // 上下给足:4K 图宽 3840,工业相机可能更大,留到 ±20000
        m_coord[i]->setRange(-20000, 20000);
        m_coord[i]->setSingleStep(5);
        m_layout->addRow(labels[i], m_coord[i]);
    }
    // 从 defaultParams 起手,不然四个框全是 0,量出来长度 0
    setParams(LineFit().defaultParams());
}

void LineFitForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**,不是 0。
    // ⚠ 以前是 toInt(0) —— 和声明值不一致的第二份副本:
    //   存盘里坐标存成了字符串时,面板显示 0 而不是声明的值,
    //   用户点一下"应用参数"就把 0 存进流水线,量出来的长度整个变了。
    const QJsonObject d = LineFit().defaultParams();
    for (int i = 0; i < 4; ++i) {
        if (params.contains(kCoordKeys.at(i)))
            m_coord[i]->setValue(knobIntForWidget(params.value(kCoordKeys.at(i)),
                                                    d, kCoordKeys.at(i)));
    }
}

QJsonObject LineFitForm::collect() const {
    QJsonObject p;
    for (int i = 0; i < 4; ++i)
        p[kCoordKeys.at(i)] = m_coord[i]->value();
    return p;
}

} // namespace w7
