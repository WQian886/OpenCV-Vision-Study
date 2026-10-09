#include "pcb_inspect.h"
#include "hsv_params.h"
#include "pcb_color_ref.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
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
    //
    // ⚠ V 下界不再绝对。原来的 hsv_low=[85,80,80] 里那个 80 是**绝对 V**,
    //   压暗到亮度 0.4 倍时金层像素的 V 掉到 78,被这道线整个切掉 ——
    //   实测 R1 ROI 的 gold_px 从 168 掉到 144(相对式在 0.4~1.6 全档
    //   稳在 168)。所以 V 下界改成"板面基色 x mask_v_mul"。
    //
    //   参数名跟着换,理由和 dark_thresh -> dark_min_pct 一样:
    //   **沿用旧名字的话,老存档里的 80 会被当成 80 倍板面色**,
    //   判据永远不成立,金层全空。读不到新名字就退回默认,宁可用默认
    //   也不能让人以为是自己配的数在起作用。
    p["hsv_h_low"]  = pcbcolor::goldBand().hLo;
    p["hsv_h_high"] = pcbcolor::goldBand().hHi;
    p["hsv_s_low"]  = pcbcolor::goldBand().sLo;
    p["mask_v_mul"] = pcbcolor::kMaskVMul;
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
    // 守卫见 operators/hsv_params.h。以前这里是裸的 lo[0].toInt(85),
    // QJsonArray::operator[] 不检查越界,hsv_low 传成空数组就 0xC0000005。
    //
    // 参数形状变了:原来是两个三元数组(绝对 H/S/V),现在是三个标量
    // + 一个倍数。**故意不去读旧的 hsv_low/hsv_high** —— 老存档里的
    // 80 会被当成 80 倍板面色,金层恒空,不如退回默认让人一眼看出来
    // 没生效。理由和 dark_thresh -> dark_min_pct 完全一样。
    double hLo = pcbcolor::goldBand().hLo;
    double hHi = pcbcolor::goldBand().hHi;
    double sLo = pcbcolor::goldBand().sLo;
    double vMul = pcbcolor::kMaskVMul;
    QString hsvErr;
    if (!readHsvScalar(params.value("hsv_h_low"),  "hsv_h_low",  &hLo,  &hsvErr) ||
        !readHsvScalar(params.value("hsv_h_high"), "hsv_h_high", &hHi,  &hsvErr) ||
        !readHsvScalar(params.value("hsv_s_low"),  "hsv_s_low",  &sLo,  &hsvErr) ||
        !readHsvScalar(params.value("mask_v_mul"),  "mask_v_mul", &vMul, &hsvErr)) {
        output["error"] = QString("金层阈值不合法: %1").arg(hsvErr);
        return output;
    }
    if (hLo < 0 || hHi > 179 || hLo >= hHi) {
        output["error"] = QString(
            "金层 H 区间不合法: [%1, %2]。H 的取值范围是 0~179(OpenCV 按度/2 存),"
            "而且下界必须小于上界。").arg(hLo, 0, 'f', 0).arg(hHi, 0, 'f', 0);
        return output;
    }
    if (sLo < 0 || sLo > 255) {
        output["error"] = QString(
            "金层 S 下界 %1 越界,取值范围是 0~255。").arg(sLo, 0, 'f', 0);
        return output;
    }
    if (vMul < 0.0 || vMul > 4.0) {
        output["error"] = QString(
            "金层 V 下界的倍数 %1 不合理,取值范围 0~4。"
            "V 下界 = 倍数 x 板面基色的 V(逐图估计),调太大金层会被切空、"
            "调成 0 则相当于不设下界。").arg(vMul, 0, 'f', 2);
        return output;
    }

    // 板面基色**逐图估计**。相对判据的右端也必须带 k 才叫相对 ——
    // 拿一张参考图的板色当常量写进配置,等式右边就没有 k 了。
    const cv::Vec3b boardBase = pcbcolor::estimateBoardBase(img);

    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    // 色带/掩膜/开运算都走 pcb_color_ref.h 的共用实现,
    // 与 pcb_defect_judge 同一份定义(实测这两处原本已经漂开了)。
    pcbcolor::HsvBand band{static_cast<int>(std::lround(hLo)),
                           static_cast<int>(std::lround(hHi)),
                           static_cast<int>(std::lround(sLo)),
                           vMul};
    cv::Mat goldMask = pcbcolor::opened(pcbcolor::bandMask(hsv, boardBase, band),
                                        pcbcolor::kernelScale(img));

    const QString mode = params.value("mode").toString("known_site");
    // min_area 从绝对像素改成占全图面积的比例。
    //
    // 实测(只改分辨率、不改亮度,auto_contour 元件数):
    //            0.5x  0.75x  1.0x  1.25x  1.5x  2.0x
    //   绝对 80px     6      9    14     18    18    18
    //   占全图 0.05%  6      7    10     10    10    10
    // 绝对值下同一块板在不同分辨率上被切成 6 个或 18 个元件。
    //
    // 顺带说明 mergeDist:原来写死 90.0f,同样是绝对像素,
    // 而且**它不是参数**,界面上根本调不到。图放大时同元件焊盘间距
    // 一起放大,90 这个阈值相对就变紧,聚类散开 —— 上表 0.5x 的 6 个
    // 就是过度聚类(全板糊成几团),2.0x 的 18 个是聚类不足。
    // 改成跟着元件尺寸走,量纲不再是像素。
    const double minAreaPct = params.value("min_area_pct").toDouble(0.05);
    const int minArea = static_cast<int>(
        std::lround(minAreaPct / 100.0 * img.cols * img.rows));
    // 聚类半径:取元件本体最大尺寸的一半。跟着图像尺寸缩放,
    // 和 SITES 那边的归一化同一个道理。
    int maxBodyW = 0, maxBodyH = 0;
    for (const auto& s : SITES) {
        maxBodyW = std::max(maxBodyW, s.refBodyW);
        maxBodyH = std::max(maxBodyH, s.refBodyH);
    }
    const double mergeDist = std::max(
        1.0, 0.5 * std::max(maxBodyW * img.cols / static_cast<double>(kRefW),
                            maxBodyH * img.rows / static_cast<double>(kRefH)));

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
        // 阈值取元件最大尺寸的一半(已按图像尺寸归一化,见上面 mergeDist 处)
        const float mergeDistF = static_cast<float>(mergeDist);
        std::vector<bool> used(blobs.size(), false);
        int idx = 0;
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (used[i]) continue;
            cv::Rect merged = blobs[i].box;
            used[i] = true;
            for (size_t j = i + 1; j < blobs.size(); ++j) {
                if (used[j]) continue;
                if (cv::norm(blobs[i].center - blobs[j].center) < mergeDistF) {
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
            // ROI 内的金像素数。⚠ 这个字段**曾经是死的**:
            // 写了但全仓库没有任何消费者(2026-10-09 实测 grep),
            // 而它恰好是唯一能暴露"定位算子与判定算子的金层掩膜已经
            // 漂开"的字段 —— 判据那边早改成相对 V 下界了,这边的绝对
            // V>=80 没动,56/56 端到端测试照样全绿,因为没人读这个数。
            //
            // 现在给的是**占 ROI 面积的比例**,和下游所有阈值同一量纲;
            // 像素数留着只会被误当成能跨分辨率用的数。
            cj["gold_pct"] = 100.0 * cv::countNonZero(goldMask(roi))
                           / (roi.width * roi.height);
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
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179); m_hHigh->setValue(115);
    // V 下界不再是绝对值,改成"板面基色 x 多少倍",用双精度小数的框。
    m_vMul = new QDoubleSpinBox; m_vMul->setRange(0.0, 4.0);
    m_vMul->setDecimals(2); m_vMul->setSingleStep(0.05); m_vMul->setValue(0.8);
    m_minAreaPct = new QDoubleSpinBox; m_minAreaPct->setRange(0.0, 10.0);
    m_minAreaPct->setDecimals(3); m_minAreaPct->setSingleStep(0.001);
    m_minAreaPct->setValue(0.05);

    m_layout->addRow(tr("定位模式"),   m_mode);
    m_layout->addRow(tr("金层 H 下限"), m_hLow);
    m_layout->addRow(tr("金层 S 下限"), m_sLow);
    m_layout->addRow(tr("金层 H 上限"), m_hHigh);
    m_layout->addRow(tr("金层 V 下界 = 板面色 x"), m_vMul);
    m_layout->addRow(tr("最小连通域 占全图 %"), m_minAreaPct);
}

void PcbInspectForm::setParams(const QJsonObject& params) {
    const QString mode = params.value("mode").toString("known_site");
    const int idx = m_mode->findData(mode);
    if (idx >= 0) m_mode->setCurrentIndex(idx);

    // 读不出来就保持界面上的默认值,**不在这里报错**:
    // 界面控件自己 collect() 出来的永远是合法数字,只有手改 .sol.json
    // 才可能拿到怪东西 —— 那时 run() 会明确报错(见 run() 里的守卫),
    // 界面不必替它再吵一遍。
    double d = 0;
    QString err;
    if (readHsvScalar(params.value("hsv_h_low"),  "hsv_h_low",  &d, &err)) m_hLow->setValue(int(d));
    if (readHsvScalar(params.value("hsv_s_low"),  "hsv_s_low",  &d, &err)) m_sLow->setValue(int(d));
    if (readHsvScalar(params.value("hsv_h_high"), "hsv_h_high", &d, &err)) m_hHigh->setValue(int(d));
    if (readHsvScalar(params.value("mask_v_mul"),  "mask_v_mul", &d, &err)) m_vMul->setValue(d);
    if (readHsvScalar(params.value("min_area_pct"), "min_area_pct", &d, &err))
        m_minAreaPct->setValue(d);
}

QJsonObject PcbInspectForm::collect() const {
    QJsonObject p;
    p["mode"]        = m_mode->currentData().toString();
    p["hsv_h_low"]   = m_hLow->value();
    p["hsv_s_low"]   = m_sLow->value();
    p["hsv_h_high"]  = m_hHigh->value();
    p["mask_v_mul"]  = m_vMul->value();
    p["min_area_pct"] = m_minAreaPct->value();
    return p;
}

} // namespace w7
