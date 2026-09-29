#include "pcb_defect_judge.h"
#include <QSpinBox>
#include <QJsonArray>
#include <opencv2/opencv.hpp>
#include <algorithm>

namespace w7 {

namespace {

// 站点期望信息(判定需要知道"这个元件本该长什么样")
struct Expect {
    const char* label;
    int roiCx, roiCy;   // ROI 内坐标的期望质心(judgeShifted 用)
};
// 与 W6 一致:R2 的 SITES 中心 (150,480),ROI 由 ±10 缓冲构造
// → ROI 内坐标 (60,24)
const Expect EXPECTS[6] = {
    {"R1",  0,  0},   // 不参与偏移判定
    {"C1",  0,  0},
    {"U1",  0,  0},
    {"R2", 60, 24},   // judgeShifted 的基准
    {"C2",  0,  0},
    {"R3",  0,  0},
};

void addDefect(QJsonArray& arr, const QString& label, const char* type,
               const QString& detail) {
    QJsonObject d;
    d["component"] = label;
    d["type"]      = QString::fromUtf8(type);
    d["detail"]    = detail;
    arr.append(d);
}

} // namespace

QJsonObject PcbDefectJudge::defaultParams() const {
    QJsonObject p;
    p["dark_thresh"]    = 50;    // missing
    p["yellow_thresh"]  = 200;   // wrong
    p["polarity_ratio"] = 60;    // polarity: 质心 x > width*0.6
    p["bridge_thresh"]  = 70;    // bridge: 最长金段 > 70px
    p["pad_drop"]       = 50;    // insufficient: min < median - 50
    p["shift_x"]        = 15;    // shifted
    p["shift_y"]        = 8;
    p["short_min_area"] = 100;   // short
    p["short_max_area"] = 800;
    return p;
}

QJsonObject PcbDefectJudge::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    QJsonArray comps = inputs.value("components").toArray();
    if (imgPath.isEmpty()) {
        output["error"] = "上游未提供 image_path";
        return output;
    }
    if (comps.isEmpty()) {
        output["error"] = "上游未提供 components(需要先接 pcb_inspect 算子)";
        return output;
    }
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    const int darkThresh    = params.value("dark_thresh").toInt(50);
    const int yellowThresh  = params.value("yellow_thresh").toInt(200);
    const int polarityRatio = params.value("polarity_ratio").toInt(60);
    const int bridgeThresh  = params.value("bridge_thresh").toInt(70);
    const int padDrop       = params.value("pad_drop").toInt(50);
    const int shiftX        = params.value("shift_x").toInt(15);
    const int shiftY        = params.value("shift_y").toInt(8);
    const int shortMinArea  = params.value("short_min_area").toInt(100);
    const int shortMaxArea  = params.value("short_max_area").toInt(800);

    // 金层 mask(bridge / insufficient 判定需要)
    cv::Mat hsv, goldMask;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(85, 80, 80), cv::Scalar(115, 255, 255), goldMask);
    cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(goldMask, goldMask, cv::MORPH_OPEN, k);

    QJsonArray defects;
    cv::Mat marked = img.clone();

    for (const auto& v : comps) {
        QJsonObject c = v.toObject();
        const QString label = c.value("label").toString();
        cv::Rect roi(c.value("x").toInt(), c.value("y").toInt(),
                     c.value("w").toInt(), c.value("h").toInt());
        roi &= cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0 || roi.height <= 0) continue;

        // ---------- missing: R2 本体暗像素过少 ----------
        if (label == "R2") {
            cv::Mat rgb, dark;
            cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
            cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
            int darkCount = cv::countNonZero(dark);
            if (darkCount < darkThresh) {
                addDefect(defects, label, "missing",
                          QString("暗像素 %1 < 阈值 %2(元件疑似被擦除)")
                          .arg(darkCount).arg(darkThresh));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- wrong: R3 本体变浅黄 ----------
        if (label == "R3") {
            cv::Mat yellowMask;
            cv::inRange(img(roi), cv::Scalar(150, 150, 0),
                        cv::Scalar(255, 255, 80), yellowMask);
            int yellowCount = cv::countNonZero(yellowMask);
            if (yellowCount > yellowThresh) {
                addDefect(defects, label, "wrong",
                          QString("浅黄像素 %1 > 阈值 %2(应为黑色电阻)")
                          .arg(yellowCount).arg(yellowThresh));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- polarity: C1 白色极性条偏右 ----------
        if (label == "C1") {
            cv::Mat whiteMask;
            cv::inRange(img(roi), cv::Scalar(200, 200, 200),
                        cv::Scalar(255, 255, 255), whiteMask);
            cv::Moments m = cv::moments(whiteMask, true);
            if (m.m00 >= 5) {   // 白色太少不判定
                double cx = m.m10 / m.m00;
                if (cx > roi.width * polarityRatio / 100.0) {
                    addDefect(defects, label, "polarity",
                              QString("极性条质心 x=%1 > %2% 宽度(极性反)")
                              .arg((int)cx).arg(polarityRatio));
                    cv::rectangle(marked, roi, cv::Scalar(255, 0, 255), 3);
                }
            }
        }

        // ---------- bridge: R1 焊盘间金条连片 ----------
        if (label == "R1") {
            cv::Mat roiMask = goldMask(roi);
            int maxWidth = 0, maxY = 0;
            for (int y = 0; y < roiMask.rows; ++y) {
                int w = cv::countNonZero(roiMask.row(y));
                if (w > maxWidth) { maxWidth = w; maxY = y; }
            }
            if (maxWidth > bridgeThresh) {
                addDefect(defects, label, "bridge",
                          QString("最长金段 %1px > 阈值 %2(焊桥)@y=%3")
                          .arg(maxWidth).arg(bridgeThresh).arg(maxY));
                cv::rectangle(marked, roi, cv::Scalar(0, 140, 255), 3);
            }
        }

        // ---------- insufficient: U1 某焊盘面积远小于中位数 ----------
        if (label == "U1") {
            cv::Mat roiMask = goldMask(roi);
            cv::Mat labels, stats, centroids;
            int n = cv::connectedComponentsWithStats(roiMask, labels, stats, centroids);
            std::vector<int> areas;
            for (int i = 1; i < n; ++i) {
                int a = stats.at<int>(i, cv::CC_STAT_AREA);
                if (a >= 80) areas.push_back(a);   // 过滤边缘噪声
            }
            if (areas.size() >= 4) {
                std::sort(areas.begin(), areas.end());
                int median = areas[areas.size() / 2];
                int minArea = areas.front();
                if (minArea < median - padDrop) {
                    addDefect(defects, label, "insufficient",
                              QString("最小焊盘 %1px² < 中位数 %2 - %3(焊少)")
                              .arg(minArea).arg(median).arg(padDrop));
                    cv::rectangle(marked, roi, cv::Scalar(255, 200, 0), 3);
                }
            }
        }

        // ---------- shifted: R2 质心偏离应站中心 ----------
        if (label == "R2") {
            const Expect* e = nullptr;
            for (const auto& ex : EXPECTS) {
                if (label == QString::fromUtf8(ex.label)) { e = &ex; break; }
            }
            if (e && (e->roiCx != 0 || e->roiCy != 0)) {
                cv::Mat rgb, dark;
                cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
                cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
                cv::Moments m = cv::moments(dark, true);
                if (m.m00 >= 50) {   // 没本体就交给 missing 判
                    double cx = m.m10 / m.m00, cy = m.m01 / m.m00;
                    double dx = cx - e->roiCx, dy = cy - e->roiCy;
                    if (std::abs(dx) > shiftX || std::abs(dy) > shiftY) {
                        addDefect(defects, label, "shifted",
                                  QString("质心(%1,%2) 偏离应站中心(%3,%4) 超容差(%5,%6)")
                                  .arg((int)cx).arg((int)cy).arg(e->roiCx).arg(e->roiCy)
                                  .arg(shiftX).arg(shiftY));
                        cv::rectangle(marked, roi, cv::Scalar(0, 255, 255), 3);
                    }
                }
            }
        }
    }

    // ---------- short: 蓝紫圆环(全局缺陷,不属单个元件) ----------
    {
        cv::Mat blueMask;
        cv::inRange(hsv, cv::Scalar(100, 80, 100), cv::Scalar(135, 255, 255), blueMask);
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(blueMask, labels, stats, centroids);
        int bestIdx = -1, bestArea = 0;
        for (int i = 1; i < n; ++i) {
            int a = stats.at<int>(i, cv::CC_STAT_AREA);
            if (a > bestArea) { bestArea = a; bestIdx = i; }
        }
        if (bestIdx > 0) {
            int x = stats.at<int>(bestIdx, cv::CC_STAT_LEFT);
            int y = stats.at<int>(bestIdx, cv::CC_STAT_TOP);
            int w = stats.at<int>(bestIdx, cv::CC_STAT_WIDTH);
            int h = stats.at<int>(bestIdx, cv::CC_STAT_HEIGHT);
            bool isRing = (bestArea > shortMinArea && bestArea < shortMaxArea
                        && w >= 30 && w <= 80 && h >= 20 && h <= 60);
            if (isRing) {
                // component 留空 —— 短路是全局缺陷,不归属某个元件
                addDefect(defects, QString(), "short",
                          QString("短路圆环 @(%1,%2) %3x%4 面积=%5")
                          .arg(x).arg(y).arg(w).arg(h).arg(bestArea));
                cv::circle(marked, cv::Point(x + w / 2, y + h / 2),
                           std::max(w, h), cv::Scalar(0, 0, 255), 3);
            }
        }
    }

    QString markedPath = "images/w7_pcb_defect.png";
    cv::imwrite(markedPath.toStdString(), marked);

    output["marked_path"]   = markedPath;
    output["defects"]       = defects;
    output["defect_count"]  = defects.size();
    output["component_count"] = comps.size();
    output["result"]        = defects.isEmpty() ? "PASS" : "FAIL";
    return output;
}

QWidget* PcbDefectJudge::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new PcbDefectJudgeForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// PcbDefectJudgeForm
// ============================================================
PcbDefectJudgeForm::PcbDefectJudgeForm(QWidget* parent) : ParamForm(parent) {}

void PcbDefectJudgeForm::buildUI() {
    auto mk = [](int lo, int hi, int val) {
        auto* s = new QSpinBox;
        s->setRange(lo, hi); s->setValue(val);
        return s;
    };
    m_darkThresh    = mk(0, 100000, 50);
    m_yellowThresh  = mk(0, 100000, 200);
    m_polarityR     = mk(0, 100, 60);
    m_bridgeThresh  = mk(0, 100000, 70);
    m_padDropThresh = mk(0, 100000, 50);
    m_shiftX        = mk(0, 10000, 15);
    m_shiftY        = mk(0, 10000, 8);
    m_shortMinArea  = mk(0, 100000, 100);
    m_shortMaxArea  = mk(0, 100000, 800);

    m_layout->addRow(tr("缺件:暗像素下限"),   m_darkThresh);
    m_layout->addRow(tr("错件:浅黄像素下限"), m_yellowThresh);
    m_layout->addRow(tr("极性:质心位置 %"),   m_polarityR);
    m_layout->addRow(tr("焊桥:最长金段 px"),  m_bridgeThresh);
    m_layout->addRow(tr("焊少:焊盘面积差"),   m_padDropThresh);
    m_layout->addRow(tr("偏移:x 容差 px"),    m_shiftX);
    m_layout->addRow(tr("偏移:y 容差 px"),    m_shiftY);
    m_layout->addRow(tr("短路:面积下限"),     m_shortMinArea);
    m_layout->addRow(tr("短路:面积上限"),     m_shortMaxArea);
}

void PcbDefectJudgeForm::setParams(const QJsonObject& params) {
    m_darkThresh->setValue(params.value("dark_thresh").toInt(50));
    m_yellowThresh->setValue(params.value("yellow_thresh").toInt(200));
    m_polarityR->setValue(params.value("polarity_ratio").toInt(60));
    m_bridgeThresh->setValue(params.value("bridge_thresh").toInt(70));
    m_padDropThresh->setValue(params.value("pad_drop").toInt(50));
    m_shiftX->setValue(params.value("shift_x").toInt(15));
    m_shiftY->setValue(params.value("shift_y").toInt(8));
    m_shortMinArea->setValue(params.value("short_min_area").toInt(100));
    m_shortMaxArea->setValue(params.value("short_max_area").toInt(800));
}

QJsonObject PcbDefectJudgeForm::collect() const {
    QJsonObject p;
    p["dark_thresh"]    = m_darkThresh->value();
    p["yellow_thresh"]  = m_yellowThresh->value();
    p["polarity_ratio"] = m_polarityR->value();
    p["bridge_thresh"]  = m_bridgeThresh->value();
    p["pad_drop"]       = m_padDropThresh->value();
    p["shift_x"]        = m_shiftX->value();
    p["shift_y"]        = m_shiftY->value();
    p["short_min_area"] = m_shortMinArea->value();
    p["short_max_area"] = m_shortMaxArea->value();
    return p;
}

} // namespace w7
