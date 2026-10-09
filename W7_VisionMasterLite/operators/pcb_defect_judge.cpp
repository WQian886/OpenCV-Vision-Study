#include "pcb_defect_judge.h"
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

namespace w7 {

namespace {

void addDefect(QJsonArray& arr, const QString& label, const char* type,
               const QString& detail) {
    QJsonObject d;
    d["component"] = label;
    d["type"]      = QString::fromUtf8(type);
    d["detail"]    = detail;
    arr.append(d);
}

// 本算子真正写了判定规则的标签。run() 里按 label 分支的六处
// (R2 missing / R3 wrong / C1 polarity / R1 bridge / U1 insufficient /
//  R2 shifted)只认这些名字 —— 拿别的名字进来,一条规则都跑不了。
const char* const kJudgeableLabels[] = {"R1", "R2", "R3", "C1", "U1"};

bool isJudgeableLabel(const QString& label) {
    for (const char* l : kJudgeableLabels) {
        if (label == QString::fromUtf8(l)) return true;
    }
    return false;
}

} // namespace

// ============================================================
// 阈值:全部是"占某个基准的百分比",一个裸像素数都不留
// ============================================================
// 基准逐条不同,因为量出来的东西量纲不同:
//
//   面积类的量(暗像素 / 浅黄 / 焊盘)→ 该元件的 ROI 面积
//   长度类的量(最长金段 / 质心偏移)→ 该元件的 ROI 宽 或 高
//   全局缺陷(短路圆环,不属于任何元件)→ 全图面积 / 全图对角线
//
// 下面每个默认值都是**从实测中间量推出来的**,不是拍的。
// 括号里是"正常板实测值 ↔ 对应缺陷图实测值",新默认落在两者的几何中点,
// 两边都留最大余量。实测来自 %TEMP%\w7probe\pcb_calib.cpp 的标定报告。
//
//   polarity_ratio 60  正常 16.7% ↔ 缺陷 82.2%(这条本来就是比例,语义没变,
//                             所以名字也不变 —— 改了才没必要)
QJsonObject PcbDefectJudge::defaultParams() const {
    QJsonObject p;
    // --- ROI 面积 % ---
    // R2 正常板暗像素占 ROI 45.6%,缺件时 0%。原来写死 50px(=0.87%),
    // 离"缺陷侧"只有 0.87 个百分点;取 2% 两边都宽裕。
    p["dark_min_pct"]    = 2.0;
    // R3 正常浅黄 3.40% ↔ 错件 44.8%。原来 200px = 3.47%,
    // **离正常板只有 4 个像素**(196 vs 200)—— 噪声 σ=4 就能翻盘。
    // 几何中点 sqrt(3.40*44.8) = 12.3,取 12。
    p["yellow_min_pct"]  = 12.0;
    // C1 白色像素占 ROI 4.17%;这是"白色够不够,不够就别判极性"的门槛。
    p["white_min_pct"]   = 0.5;
    // U1 焊盘噪声门槛 —— **这一条我错过两次,都是因为只量了 1x**。
    //
    // 第一次:按"正常焊盘占 0.80%"想当然取 0.2%,噪声斑(0.38%)被放进来了,
    //         baseline 整块误报。是 12/12 回归测试当场抓住的。
    // 第二次:改成 0.44% 之后以为稳了 —— 其实那只是按 1x 的 (0.38%, 0.51%)
    //         取的,贴着噪声斑上沿。跨尺度一量,噪声斑会漂到 0.42%。
    //
    // 跨尺度实测(占 ROI 面积,量的是所有金层连通域):
    //         尺度   噪声斑    焊少那颗   正常板最小真焊盘
    //         0.5x    0.38%     0.60%      0.80%
    //         0.75x   0.42%     0.52%      0.85%
    //         1.0x    0.38%     0.51%      0.80%
    //         1.25x   0.41%     0.56%      0.87%
    //         2.0x    0.38%     0.51%      0.83%
    // 窗口 = (噪声斑最坏 0.42%, 焊少那颗最小 0.51%),几何中点 0.46。
    //
    // ⚠ 窗口只有 0.09pp,天生就窄。根子在样本数据:那颗"焊少"的焊盘
    //   跟噪声斑差不了多少。真实产线数据上大概率更宽,但换数据要重标。
    p["pad_min_pct"]     = 0.46;
    // U1 最小焊盘比中位数低多少 —— 同样必须跨尺度量。
    // 只量 1x 会看到"正常 5.9% / 焊少 40.4%",于是取 20;
    // 但降采样对小焊盘啃得比大焊盘狠(1x 最小/中位=94%,0.5x 掉到 74%),
    // 正常板的 drop 会被顶上去:
    //         尺度   正常板 drop   缺陷板 drop
    //         0.5x    25.6%        44.2%
    //         0.75x   12.4%        46.1%
    //         1.0x     5.9%        40.4%
    //         1.25x    7.9%        40.2%
    //         2.0x     2.0%        40.4%
    // 窗口 = (正常板最差 25.6%, 缺陷板最小 40.2%),中点 33。
    // 20 落在窗口**外面** —— 0.5x 下正常板因此误报 insufficient。
    p["pad_drop_pct"]    = 33.0;

    // --- ROI 宽度 % ---
    p["polarity_ratio"]  = 60.0;  // 见上
    // R1 最长金段占 ROI 宽度:正常 15.8% ↔ 焊桥 100%,几何中点 40。
    // 原来是 70px = 58.3%,能用但偏在缺陷侧。
    p["bridge_min_pct"]  = 40.0;
    // R2 质心横向偏移容差,占 ROI 宽:缺陷图实测 15.8%,取 8(约 2 倍余量)。
    p["shift_x_pct"]     = 8.0;
    // R2 质心纵向偏移容差,占 ROI 高:缺陷图实测 18.8%,取 10(约 1.9 倍余量)。
    p["shift_y_pct"]     = 10.0;

    // --- 全图 % ---
    // 短路圆环面积占全图:正常板最大的蓝斑 0.0107% ↔ 短路圆环 0.0411%,
    // 几何中点 0.021。上限只要高于 0.0411 就行,给到 0.08。
    p["short_min_pct"]   = 0.021;
    p["short_max_pct"]   = 0.08;
    // 圆环外框宽 / 高,占全图对角线(对角线跟着分辨率一起缩,是最省心的尺子):
    // 宽 0.246% ↔ 5.00%,高 2.05% ↔ 3.36%。
    // 宽这一条才是真正区分"环"和"斑"的(正常那块是 3x25 的实心小点);
    // 高这一条区分不了,窗口留宽即可,原来 20~60px 也一样区分不了。
    p["short_w_min_pct"] = 1.1;
    p["short_w_max_pct"] = 8.0;
    p["short_h_min_pct"] = 1.0;
    p["short_h_max_pct"] = 6.0;
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

    // ============================================================
    // 拿不到判定依据时,绝不能输出 PASS
    //
    // pcb_inspect 的"自动轮廓(换板型)"模式输出的是 C1/C2/C3...,
    // 而且编号来自**轮廓扫描顺序**,和真值模式的站点标签没有对应关系 ——
    // 自动模式的 C1 不是同一颗元件。于是下面按 label 分支的六条规则里,
    // 只有 "C1" 会命中,而且命中的是随便一颗元件;R1/U1/R2/R3 四个分支
    // 一次都进不去。
    //
    // 结果:defects 永远是空的 → result 永远是 PASS。
    // 判废算子在**什么都没检查**的情况下报"合格" —— 这是最坏的一种错。
    // 比崩了还难查:画布上状态点是绿的,日志一片正常,产线上直接漏检。
    //
    // 所以不能靠"恰好没报缺陷"反推出合格 —— 判定结果必须对应
    // 真正跑过的规则。kind 是可靠的区分依据:真值模式来自 SITES
    // (kind 是 R/C/U),自动模式写死 kind="AUTO"。
    // ============================================================
    int autoCount = 0;
    for (const auto& v : comps) {
        if (v.toObject().value("kind").toString() == "AUTO") ++autoCount;
    }
    if (autoCount > 0) {
        output["error"] = QString(
            "上游给的是 pcb_inspect 的【自动轮廓】模式产物(%1 个元件,标签是"
            "按轮廓扫描顺序编的 C1/C2/C3...,不是站点标签),而本算子的判定规则"
            "是按真值标签 R1/R2/R3/C1/U1 写的。拿自动标签去匹配只会误命中一颗"
            "元件,其余规则全部空转,最后报出一个没有依据的 PASS。"
            "请把 pcb_inspect 的模式切回真值模式再判定。").arg(autoCount);
        output["component_count"] = comps.size();
        // result 故意不给:没判定过就没有判定结果可给。
        return output;
    }

    // 兜底(手改过的 JSON 可能没有 kind 字段):一个 label 都对不上时同样
    // 不能判 PASS。宁可拒绝判定,也不能给一个没跑过规则的合格。
    int judgeable = 0;
    for (const auto& v : comps) {
        if (isJudgeableLabel(v.toObject().value("label").toString())) ++judgeable;
    }
    if (judgeable == 0) {
        output["error"] = QString(
            "%1 个元件的标签里没有一个是本算子认得的(R1/R2/R3/C1/U1),"
            "判定规则一条都跑不了 —— 不给 result,而不是报 PASS。")
                .arg(comps.size());
        output["component_count"] = comps.size();
        return output;
    }
    // 认得一部分:判定只覆盖了其中几个元件,必须说清楚覆盖了多少,
    // 否则"合格"会被当成整板都查过了。
    if (judgeable < comps.size()) {
        output["warning"] = QString(
            "%1 个元件里只有 %2 个有对应判定规则,另外 %3 个未判定 —— "
            "result 只覆盖已判定的那些。")
                .arg(comps.size()).arg(judgeable).arg(comps.size() - judgeable);
    }
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    // 老存档里存的是像素阈值(dark_thresh=50 之类)。
    // 这里**故意不去读那些旧名字**:50 被当成 50% 会比原来错得离谱,
    // 读不到就等于没传,退回下面这些新默认值 —— 宁可阈值不准,
    // 也不能让用户以为是自己配的数在起作用。
    const double darkMinPct   = params.value("dark_min_pct").toDouble(2.0);
    const double yellowMinPct = params.value("yellow_min_pct").toDouble(12.0);
    const double polarityRatio= params.value("polarity_ratio").toDouble(60.0);
    const double whiteMinPct  = params.value("white_min_pct").toDouble(0.5);
    const double bridgeMinPct = params.value("bridge_min_pct").toDouble(40.0);
    const double padMinPct    = params.value("pad_min_pct").toDouble(0.46);
    const double padDropPct   = params.value("pad_drop_pct").toDouble(33.0);
    const double shiftXPct    = params.value("shift_x_pct").toDouble(8.0);
    const double shiftYPct    = params.value("shift_y_pct").toDouble(10.0);
    const double shortMinPct  = params.value("short_min_pct").toDouble(0.021);
    const double shortMaxPct  = params.value("short_max_pct").toDouble(0.08);
    const double shortWMinPct = params.value("short_w_min_pct").toDouble(1.1);
    const double shortWMaxPct = params.value("short_w_max_pct").toDouble(8.0);
    const double shortHMinPct = params.value("short_h_min_pct").toDouble(1.0);
    const double shortHMaxPct = params.value("short_h_max_pct").toDouble(6.0);

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

        // 本 ROI 的三个"尺子"。所有阈值都拿它们当基准,不再出现像素数。
        const double roiArea = static_cast<double>(roi.width) * roi.height;
        if (roiArea <= 0) continue;

        // ---------- missing: R2 本体暗像素过少 ----------
        if (label == "R2") {
            cv::Mat rgb, dark;
            cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
            cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
            const double darkCount = cv::countNonZero(dark);
            const double darkPct   = 100.0 * darkCount / roiArea;
            if (darkPct < darkMinPct) {
                addDefect(defects, label, "missing",
                          QString("暗像素 %1(占 ROI %2%)< 阈值 %3%(元件疑似被擦除)")
                          .arg((int)darkCount).arg(darkPct, 0, 'f', 2).arg(darkMinPct));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- wrong: R3 本体变浅黄 ----------
        if (label == "R3") {
            cv::Mat yellowMask;
            cv::inRange(img(roi), cv::Scalar(150, 150, 0),
                        cv::Scalar(255, 255, 80), yellowMask);
            const double yellowPct =
                100.0 * cv::countNonZero(yellowMask) / roiArea;
            if (yellowPct > yellowMinPct) {
                addDefect(defects, label, "wrong",
                          QString("浅黄像素占 ROI %1% > 阈值 %2%(应为黑色电阻)")
                          .arg(yellowPct, 0, 'f', 2).arg(yellowMinPct));
                cv::rectangle(marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- polarity: C1 白色极性条偏右 ----------
        if (label == "C1") {
            cv::Mat whiteMask;
            cv::inRange(img(roi), cv::Scalar(200, 200, 200),
                        cv::Scalar(255, 255, 255), whiteMask);
            cv::Moments m = cv::moments(whiteMask, true);
            const double whitePct = 100.0 * m.m00 / roiArea;
            if (whitePct >= whiteMinPct) {   // 白色太少不判定
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
            const double runPct = 100.0 * maxWidth / roi.width;
            if (runPct > bridgeMinPct) {
                addDefect(defects, label, "bridge",
                          QString("最长金段占 ROI 宽 %1% > 阈值 %2%(焊桥)@y=%3")
                          .arg(runPct, 0, 'f', 1).arg(bridgeMinPct).arg(maxY));
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
                // 过滤边缘噪声。门槛是 ROI 面积的比例 —— 原来是裸的 80px,
                // 图一小,正常焊盘就掉到门槛以下被滤光,判定必然失效。
                if (100.0 * a / roiArea >= padMinPct) areas.push_back(a);
            }
            if (areas.size() >= 4) {
                std::sort(areas.begin(), areas.end());
                const int median = areas[areas.size() / 2];
                const int minArea = areas.front();
                const double dropPct =
                    median > 0 ? 100.0 * (median - minArea) / median : 0.0;
                if (median > 0 && dropPct > padDropPct) {
                    addDefect(defects, label, "insufficient",
                              QString("最小焊盘 %1px² 比中位数 %2px² 低 %3%% > 阈值 %4%(焊少)")
                              .arg(minArea).arg(median).arg(dropPct, 0, 'f', 1)
                              .arg(padDropPct));
                    cv::rectangle(marked, roi, cv::Scalar(255, 200, 0), 3);
                }
            }
        }

        // ---------- shifted: R2 质心偏离应站中心 ----------
        //
        // 期望质心就是 ROI 的几何中心,不需要一张"每个元件该长什么样"的表。
        // 以前 EXPECTS[6] 里 R2 写死的 (60,24) 正是 120x48 这个 ROI 的中心
        // —— 同一个数,只是被抄了一份,抄错了也不会有人发现。
        // ROI 是由元件本体框四周等量扩出来的,所以两者恒等。
        if (label == "R2") {
            cv::Mat rgb, dark;
            cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
            cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
            cv::Moments m = cv::moments(dark, true);
            const double darkPct = 100.0 * m.m00 / roiArea;
            // m.m00 > 0 必须单独判:dark_min_pct 被调到 0 时,
            // "暗像素占比 >= 0%" 对**一张全黑都没有的 ROI 也成立**,
            // 于是下面 m.m10/m.m00 变成 0/0 = NaN。
            // NaN 和任何数比较都是 false,判定会静默失效 —— 又是一个不报错的错。
            if (m.m00 > 0 && darkPct >= darkMinPct) {   // 没本体就交给 missing 判
                const double expCx = roi.width  * 0.5;
                const double expCy = roi.height * 0.5;
                const double cx = m.m10 / m.m00, cy = m.m01 / m.m00;
                const double dx = cx - expCx, dy = cy - expCy;
                const double tolX = roi.width  * shiftXPct / 100.0;
                const double tolY = roi.height * shiftYPct / 100.0;
                if (std::abs(dx) > tolX || std::abs(dy) > tolY) {
                    addDefect(defects, label, "shifted",
                              QString("质心(%1,%2) 偏离 ROI 中心(%3,%4) 超容差(%5,%6)")
                              .arg((int)cx).arg((int)cy)
                              .arg((int)expCx).arg((int)expCy)
                              .arg((int)tolX).arg((int)tolY));
                    cv::rectangle(marked, roi, cv::Scalar(0, 255, 255), 3);
                }
            }
        }
    }

    // ---------- short: 蓝紫圆环(全局缺陷,不属单个元件) ----------
    //
    // 它不属于任何元件,所以没有 ROI 可依,基准只能用整张图:
    // 面积对全图面积,宽高对全图对角线(对角线是唯一一个
    // "不管横竖怎么变都跟着图一起缩"的尺子)。
    {
        const double imgArea = static_cast<double>(img.cols) * img.rows;
        const double diag = std::sqrt(static_cast<double>(img.cols) * img.cols
                                    + static_cast<double>(img.rows) * img.rows);
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
            const double areaPct = 100.0 * bestArea / imgArea;
            const double wPct = 100.0 * w / diag;
            const double hPct = 100.0 * h / diag;
            bool isRing = (areaPct > shortMinPct && areaPct < shortMaxPct
                        && wPct >= shortWMinPct && wPct <= shortWMaxPct
                        && hPct >= shortHMinPct && hPct <= shortHMaxPct);
            if (isRing) {
                // component 留空 —— 短路是全局缺陷,不归属某个元件
                addDefect(defects, QString(), "short",
                          QString("短路圆环 @(%1,%2) %3x%4 面积=%5(占全图 %6%)")
                          .arg(x).arg(y).arg(w).arg(h).arg(bestArea)
                          .arg(areaPct, 0, 'f', 4));
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
    // 百分比输入框:两位小数够 ROI/全图类的用,
    // 短路那几行要三位(0.021%)—— 用整数表达不了。
    auto mk = [](double val, int decimals) {
        auto* s = new QDoubleSpinBox;
        s->setRange(0.0, 100.0);
        s->setDecimals(decimals);
        s->setSingleStep(decimals >= 3 ? 0.001 : 0.5);
        s->setValue(val);
        return s;
    };
    m_darkMinPct   = mk(2.0,   2);
    m_yellowMinPct = mk(12.0,  2);
    m_polarityR    = mk(60.0,  2);
    m_whiteMinPct  = mk(0.5,   2);
    m_bridgeMinPct = mk(40.0,  2);
    m_padMinPct    = mk(0.46,  2);
    m_padDropPct   = mk(33.0,  2);
    m_shiftXPct    = mk(8.0,   2);
    m_shiftYPct    = mk(10.0,  2);
    m_shortMinPct  = mk(0.021, 3);
    m_shortMaxPct  = mk(0.08,  3);
    m_shortWMinPct = mk(1.1,   2);
    m_shortWMaxPct = mk(8.0,   2);
    m_shortHMinPct = mk(1.0,   2);
    m_shortHMaxPct = mk(6.0,   2);

    m_layout->addRow(tr("缺件:暗像素占 ROI 下限 %"),   m_darkMinPct);
    m_layout->addRow(tr("错件:浅黄占 ROI 下限 %"),     m_yellowMinPct);
    m_layout->addRow(tr("极性:白条质心 占 ROI 宽 %"), m_polarityR);
    m_layout->addRow(tr("极性:白色像素占 ROI 下限 %"), m_whiteMinPct);
    m_layout->addRow(tr("焊桥:最长金段占 ROI 宽 %"),   m_bridgeMinPct);
    m_layout->addRow(tr("焊少:焊盘噪声门槛 占 ROI %"), m_padMinPct);
    m_layout->addRow(tr("焊少:比中位数低 %"),          m_padDropPct);
    m_layout->addRow(tr("偏移:x 容差 占 ROI 宽 %"),    m_shiftXPct);
    m_layout->addRow(tr("偏移:y 容差 占 ROI 高 %"),    m_shiftYPct);
    m_layout->addRow(tr("短路:面积下限 占全图 %"),      m_shortMinPct);
    m_layout->addRow(tr("短路:面积上限 占全图 %"),      m_shortMaxPct);
    m_layout->addRow(tr("短路:宽下限 占对角线 %"),      m_shortWMinPct);
    m_layout->addRow(tr("短路:宽上限 占对角线 %"),      m_shortWMaxPct);
    m_layout->addRow(tr("短路:高下限 占对角线 %"),      m_shortHMinPct);
    m_layout->addRow(tr("短路:高上限 占对角线 %"),      m_shortHMaxPct);
}

void PcbDefectJudgeForm::setParams(const QJsonObject& params) {
    m_darkMinPct->setValue(params.value("dark_min_pct").toDouble(2.0));
    m_yellowMinPct->setValue(params.value("yellow_min_pct").toDouble(12.0));
    m_polarityR->setValue(params.value("polarity_ratio").toDouble(60.0));
    m_whiteMinPct->setValue(params.value("white_min_pct").toDouble(0.5));
    m_bridgeMinPct->setValue(params.value("bridge_min_pct").toDouble(40.0));
    m_padMinPct->setValue(params.value("pad_min_pct").toDouble(0.46));
    m_padDropPct->setValue(params.value("pad_drop_pct").toDouble(33.0));
    m_shiftXPct->setValue(params.value("shift_x_pct").toDouble(8.0));
    m_shiftYPct->setValue(params.value("shift_y_pct").toDouble(10.0));
    m_shortMinPct->setValue(params.value("short_min_pct").toDouble(0.021));
    m_shortMaxPct->setValue(params.value("short_max_pct").toDouble(0.08));
    m_shortWMinPct->setValue(params.value("short_w_min_pct").toDouble(1.1));
    m_shortWMaxPct->setValue(params.value("short_w_max_pct").toDouble(8.0));
    m_shortHMinPct->setValue(params.value("short_h_min_pct").toDouble(1.0));
    m_shortHMaxPct->setValue(params.value("short_h_max_pct").toDouble(6.0));
}

QJsonObject PcbDefectJudgeForm::collect() const {
    QJsonObject p;
    p["dark_min_pct"]    = m_darkMinPct->value();
    p["yellow_min_pct"]  = m_yellowMinPct->value();
    p["polarity_ratio"]  = m_polarityR->value();
    p["white_min_pct"]   = m_whiteMinPct->value();
    p["bridge_min_pct"]  = m_bridgeMinPct->value();
    p["pad_min_pct"]     = m_padMinPct->value();
    p["pad_drop_pct"]    = m_padDropPct->value();
    p["shift_x_pct"]     = m_shiftXPct->value();
    p["shift_y_pct"]     = m_shiftYPct->value();
    p["short_min_pct"]   = m_shortMinPct->value();
    p["short_max_pct"]   = m_shortMaxPct->value();
    p["short_w_min_pct"] = m_shortWMinPct->value();
    p["short_w_max_pct"] = m_shortWMaxPct->value();
    p["short_h_min_pct"] = m_shortHMinPct->value();
    p["short_h_max_pct"] = m_shortHMaxPct->value();
    return p;
}

} // namespace w7
