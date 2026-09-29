#include "pcb_inspect.h"
#include <QSpinBox>
#include <QComboBox>
#include <QJsonArray>
#include <opencv2/opencv.hpp>

namespace w7 {

// ============================================================
// 板型真值(同步自 W6 main.cpp 的 SITES 数组)
// 只在 known_site 模式用。auto_contour 模式不依赖它。
// ============================================================
namespace {
struct SiteDef {
    const char* label;
    const char* kind;
    int cx, cy;
    int bodyW, bodyH;
};

const SiteDef SITES[6] = {
    {"R1", "R",  150, 200, 100, 28},
    {"C1", "C",  400, 200,  70, 28},
    {"U1", "IC", 720, 200, 180, 60},
    {"R2", "R",  150, 480, 100, 28},
    {"C2", "C",  400, 480,  70, 28},
    {"R3", "R",  700, 480, 100, 28},
};
} // namespace

QJsonObject PcbInspect::defaultParams() const {
    QJsonObject p;
    p["mode"]     = "known_site";
    // === 金色范围沿用 W6 校准值,不要想当然改成 (10,80,80) ===
    // W6 笔记记录的坑:合成器的 GOLD 实际落在 H 85-115,
    // 用 W2 笔记里的常规金色范围会抓到 IC 的深蓝本体而不是金层。
    p["hsv_low"]  = QJsonArray{85, 80, 80};
    p["hsv_high"] = QJsonArray{115, 255, 255};
    p["min_area"] = 80;
    return p;
}

QJsonObject PcbInspect::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    if (imgPath.isEmpty()) {
        output["error"] = "上游未提供 image_path(需要先接图像源算子)";
        return output;
    }
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    // --- 金层提取 ---
    QJsonArray lo = params.value("hsv_low").toArray();
    QJsonArray hi = params.value("hsv_high").toArray();
    cv::Scalar low(lo[0].toInt(85),  lo[1].toInt(80), lo[2].toInt(80));
    cv::Scalar high(hi[0].toInt(115), hi[1].toInt(255), hi[2].toInt(255));

    cv::Mat hsv, goldMask;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, low, high, goldMask);
    cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(goldMask, goldMask, cv::MORPH_OPEN, k);

    const QString mode = params.value("mode").toString("known_site");
    const int minArea  = params.value("min_area").toInt(80);

    QJsonArray compsJson;
    cv::Mat marked = img.clone();

    if (mode == "auto_contour") {
        // --- 自动模式:从金层连通域发现元件 ---
        // 真实产线做法:不依赖板型真值,换板型也能用。
        // 代价:焊盘会被当成独立元件(一个 IC 有十几个焊盘),
        //      所以这里按"同一行/列的焊盘聚类"合并成元件框。
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(goldMask, contours, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);

        struct Blob { cv::Rect box; int area; cv::Point2f center; };
        std::vector<Blob> blobs;
        for (const auto& c : contours) {
            cv::Rect r = cv::boundingRect(c);
            int a = (int)cv::contourArea(c);
            if (a < minArea) continue;
            blobs.push_back({r, a, cv::Point2f((float)r.x + r.width / 2.0f,
                                               (float)r.y + r.height / 2.0f)});
        }

        // 聚类:中心距离 < 阈值的焊盘归为一个元件
        // 阈值取元件最大尺寸的一半 —— 同元件的焊盘一定挨得很近
        const float mergeDist = 90.0f;
        std::vector<bool> used(blobs.size(), false);
        int idx = 0;
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (used[i]) continue;
            cv::Rect merged = blobs[i].box;
            used[i] = true;
            for (size_t j = i + 1; j < blobs.size(); ++j) {
                if (used[j]) continue;
                if (cv::norm(blobs[i].center - blobs[j].center) < mergeDist) {
                    merged |= blobs[j].box;
                    used[j] = true;
                }
            }
            QJsonObject cj;
            cj["label"]  = QString("C%1").arg(++idx);   // 自动模式无真值标签
            cj["kind"]   = "AUTO";
            cj["x"]      = merged.x;
            cj["y"]      = merged.y;
            cj["w"]      = merged.width;
            cj["h"]      = merged.height;
            cj["area"]   = merged.width * merged.height;
            compsJson.append(cj);

            cv::rectangle(marked, merged, cv::Scalar(0, 200, 255), 2);
            cv::putText(marked, cj["label"].toString().toStdString(),
                        cv::Point(merged.x, std::max(12, merged.y - 4)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 200, 255), 1);
        }
    } else {
        // --- 真值模式:按 SITES 构造 ROI(W6 已验证路径) ---
        for (const auto& s : SITES) {
            // ROI 与图像边界求交,换分辨率也不会越界
            cv::Rect roi(s.cx - s.bodyW / 2 - 10, s.cy - s.bodyH / 2 - 10,
                         s.bodyW + 20, s.bodyH + 20);
            roi &= cv::Rect(0, 0, img.cols, img.rows);
            if (roi.width <= 0 || roi.height <= 0) continue;

            QJsonObject cj;
            cj["label"] = QString::fromUtf8(s.label);
            cj["kind"]  = QString::fromUtf8(s.kind);
            cj["x"]     = roi.x;
            cj["y"]     = roi.y;
            cj["w"]     = roi.width;
            cj["h"]     = roi.height;
            // ROI 内的金像素数,供下游判定焊少/焊桥用
            cj["gold_px"] = cv::countNonZero(goldMask(roi));
            compsJson.append(cj);

            cv::rectangle(marked, roi, cv::Scalar(0, 220, 0), 2);
            cv::putText(marked,
                        QString("%1(%2)").arg(QString::fromUtf8(s.label),
                                              QString::fromUtf8(s.kind)).toStdString(),
                        cv::Point(roi.x, std::max(12, roi.y - 4)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 220, 0), 1);
        }
    }

    QString markedPath = "images/w7_pcb_inspect.png";
    cv::imwrite(markedPath.toStdString(), marked);

    output["marked_path"] = markedPath;
    output["components"] = compsJson;
    output["count"]       = compsJson.size();
    output["mode"]        = mode;
    // 原图路径往下传:判定算子还要 imread 同一张图,
    // 不能让它依赖工具栏兜底(工具栏改了就对不上了)
    output["image_path"]  = imgPath;
    return output;
}

QWidget* PcbInspect::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new PcbInspectForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// PcbInspectForm
// ============================================================
PcbInspectForm::PcbInspectForm(QWidget* parent) : ParamForm(parent) {}

void PcbInspectForm::buildUI() {
    m_mode = new QComboBox;
    m_mode->addItem(tr("真值坐标(已知板型)"), "known_site");
    m_mode->addItem(tr("自动轮廓(换板型)"),   "auto_contour");

    m_hLow  = new QSpinBox;  m_hLow->setRange(0, 179);  m_hLow->setValue(85);
    m_sLow  = new QSpinBox;  m_sLow->setRange(0, 255);  m_sLow->setValue(80);
    m_vLow  = new QSpinBox;  m_vLow->setRange(0, 255);  m_vLow->setValue(80);
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179); m_hHigh->setValue(115);
    m_sHigh = new QSpinBox;  m_sHigh->setRange(0, 255); m_sHigh->setValue(255);
    m_vHigh = new QSpinBox;  m_vHigh->setRange(0, 255); m_vHigh->setValue(255);
    m_minArea = new QSpinBox; m_minArea->setRange(0, 100000); m_minArea->setValue(80);

    m_layout->addRow(tr("定位模式"),   m_mode);
    m_layout->addRow(tr("金层 H 下限"), m_hLow);
    m_layout->addRow(tr("金层 S 下限"), m_sLow);
    m_layout->addRow(tr("金层 V 下限"), m_vLow);
    m_layout->addRow(tr("金层 H 上限"), m_hHigh);
    m_layout->addRow(tr("金层 S 上限"), m_sHigh);
    m_layout->addRow(tr("金层 V 上限"), m_vHigh);
    m_layout->addRow(tr("最小连通域面积"), m_minArea);
}

void PcbInspectForm::setParams(const QJsonObject& params) {
    const QString mode = params.value("mode").toString("known_site");
    const int idx = m_mode->findData(mode);
    if (idx >= 0) m_mode->setCurrentIndex(idx);

    QJsonArray lo = params.value("hsv_low").toArray();
    QJsonArray hi = params.value("hsv_high").toArray();
    if (lo.size() >= 3) {
        m_hLow->setValue(lo[0].toInt());
        m_sLow->setValue(lo[1].toInt());
        m_vLow->setValue(lo[2].toInt());
    }
    if (hi.size() >= 3) {
        m_hHigh->setValue(hi[0].toInt());
        m_sHigh->setValue(hi[1].toInt());
        m_vHigh->setValue(hi[2].toInt());
    }
    m_minArea->setValue(params.value("min_area").toInt(80));
}

QJsonObject PcbInspectForm::collect() const {
    QJsonObject p;
    p["mode"]     = m_mode->currentData().toString();
    p["hsv_low"]  = QJsonArray{m_hLow->value(),  m_sLow->value(),  m_vLow->value()};
    p["hsv_high"] = QJsonArray{m_hHigh->value(), m_sHigh->value(), m_vHigh->value()};
    p["min_area"] = m_minArea->value();
    return p;
}

} // namespace w7
