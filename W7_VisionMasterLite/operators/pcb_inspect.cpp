#include "pcb_inspect.h"
#include <QSpinBox>
#include <QComboBox>
#include <QJsonArray>
#include <QStringList>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

namespace w7 {

// ============================================================
// 板型真值(同步自 W6 main.cpp 的 SITES 数组)
// 只在 known_site 模式用。auto_contour 模式不依赖它。
//
// === 表里的数字是什么意思 ===
// 数字还是从 1000x700 那张图上量的,但**含义变了**:
// 它们是"这块板在整张图里占多大比例、中心在图上什么位置"的参考值。
// run() 里统一乘上 (imgW/1000, imgH/700),所以换分辨率、换工位相机时
// ROI 会跟着缩放,而不是死死钉在那几个像素上。
//
// 改之前是什么样(实测,不是推演):
//   图缩到 500x350 -> R2 的 ROI(90,456,120,48)整个掉到板外,
//   元件被 continue 跳过 -> 6 个元件只剩 2 个 -> 判定照样输出 PASS。
//   一条日志都没有。这是"什么都没检查却报合格",比判错更危险。
//
// ⚠ 归一化只解决"分辨率变了",不解决"板型变了"。
//    换一块布局不同的板子,这组比例照样是错的 —— 那是另一件事。
// ============================================================
namespace {
// 标定时的参考分辨率
constexpr int kRefW   = 1000;
constexpr int kRefH   =  700;
// 参考分辨率下 ROI 四周多留的余量(像素)
constexpr int kRefBuf =   10;

struct SiteDef {
    const char* label;
    const char* kind;
    int refCx, refCy;         // 参考分辨率下的中心点
    int refBodyW, refBodyH;   // 参考分辨率下的本体尺寸
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
        //
        // 站点几何按图像尺寸等比缩放(见上面"表里的数字是什么意思")。
        // 余量取**较小**的那个缩放比:ROI 撑大只会稀释下游的比例阈值,
        // 留窄一点最多是贴边,不会把元件切掉 —— 宁可保守。
        const double sx = static_cast<double>(img.cols) / kRefW;
        const double sy = static_cast<double>(img.rows) / kRefH;
        const int buf = std::max(1, static_cast<int>(
            std::lround(kRefBuf * std::min(sx, sy))));

        // 站点被丢掉的要记账。以前是"越界就 continue",于是
        // 上游少给几个元件,下游照样报 PASS,没人知道少算了。
        QStringList skipped;

        for (const auto& s : SITES) {
            const int cx = static_cast<int>(std::lround(s.refCx * sx));
            const int cy = static_cast<int>(std::lround(s.refCy * sy));
            const int bw = static_cast<int>(std::lround(s.refBodyW * sx));
            const int bh = static_cast<int>(std::lround(s.refBodyH * sy));
            cv::Rect roi(cx - bw / 2 - buf, cy - bh / 2 - buf,
                         bw + 2 * buf, bh + 2 * buf);
            // ROI 与图像边界求交,换分辨率也不会越界
            roi &= cv::Rect(0, 0, img.cols, img.rows);
            if (roi.width <= 0 || roi.height <= 0) {
                skipped << QString::fromUtf8(s.label);
                continue;
            }

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

        if (!skipped.isEmpty()) {
            output["warning"] = QString(
                "板型真值里的 %1 个站点(%2)在这张 %3x%4 的图上算不出有效 ROI,"
                "已经跳过 —— 下游只能判定剩下的 %5 个元件,"
                "不要把这次的结论当成整板都查过了。")
                .arg(skipped.size()).arg(skipped.join(","))
                .arg(img.cols).arg(img.rows).arg(compsJson.size());
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
