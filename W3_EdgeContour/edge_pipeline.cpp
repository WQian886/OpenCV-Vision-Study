#include "edge_pipeline.h"

#include "pcb_sim.h"

namespace w3 {
namespace {

// ------------------------------------------------------------
// 常量 —— 全部只在这里定义一次
// ------------------------------------------------------------

// 偏移焊点：实际画在这里，而不是它该在的 (200,100)
const cv::Rect kOffsetPadRect(220, 130, 80, 80);

// 偏移焊点应该在的位置（这里只画一个红框）
const cv::Rect kOffsetOriginalRect(200, 100, 80, 80);

// 虚焊焊点：整块是金色，但左上角 50x50 被底色盖掉
const cv::Rect kColdPadRect(50, 300, 80, 80);
const cv::Rect kColdMissingRect(50, 300, 50, 50);

// 缺失焊点：这里什么都没画
const cv::Rect kMissingPadRect(500, 100, 80, 80);

// 5 个**完整**的金色焊点。
// 注意 (50,300) 不在里面 —— 那个位置是虚焊的，单独画。
const std::vector<cv::Rect> kSolidPadRects = {
    cv::Rect(50,  100, 80, 80),   // 0: 正常
    kOffsetPadRect,               // 1: 偏移（原 200,100）
    cv::Rect(350, 100, 80, 80),   // 2: 正常
    cv::Rect(200, 300, 80, 80),   // 3: 正常
    cv::Rect(350, 300, 80, 80),   // 4: 正常
};

// 缺陷标记用的红色
const cv::Scalar kMarkerColor(0, 0, 255);

const double kMinPadArea = 1000.0;
const int kCannyLow = 50;
const int kCannyHigh = 150;
const int kMorphKernel = 3;

}  // namespace

const std::vector<cv::Rect>& solidPadRects() { return kSolidPadRects; }
const cv::Rect& offsetPadRect() { return kOffsetPadRect; }
const cv::Rect& offsetOriginalRect() { return kOffsetOriginalRect; }
const cv::Rect& coldPadRect() { return kColdPadRect; }
const cv::Rect& coldMissingRect() { return kColdMissingRect; }
const cv::Rect& missingPadRect() { return kMissingPadRect; }

double minPadArea() { return kMinPadArea; }
int cannyLowThreshold() { return kCannyLow; }
int cannyHighThreshold() { return kCannyHigh; }
int morphKernelSize() { return kMorphKernel; }

cv::Mat generatePcbDefectImage(int seed) {
    cv::Mat img(pcbsim::imageSize(), CV_8UC3, pcbsim::backgroundColor());

    // 5 个完整的金色焊点
    for (const auto& p : kSolidPadRects) {
        cv::rectangle(img, p, pcbsim::padColor(), -1);
    }

    // 偏移标记：在原位置画一个红色「L 形角标」+ 一根指向实际位置的线 + 文字。
    //
    // ⚠ 这里**不能**画一个完整的红色空框。
    //
    // 2026-10-28 实测：完整的空框画在 (200,100,80,80) 上，而偏移后的焊点在
    // (220,130,80,80) —— 两个矩形**有 60x50 的重叠**。
    // 空框的右边和下边因此**压在焊点上面**，把那块金色切成了三块：
    //
    //     1602（角上那块，58x48）  +  918  +  876
    //
    // 后两块**刚好卡在 1000 的面积门槛下面**，于是
    // 「找到 6 个焊点」这个数其实是**门槛的产物**，不是图的事实 ——
    // 门槛改成 800 就变成 8 个。
    //
    // 而且那个偏移的缺陷，在轮廓这一层**根本认不出来**：
    // 活下来的只是角上 58x48 的一块。
    //
    // ⇒ **标注不该毁掉它要标注的东西。**
    // 所以这里只画原位置左上角的两条边（L 形角标），
    // 它们和焊点**不相交**（实测焊点保持完整的一块）。
    const int fx = kOffsetOriginalRect.x, fy = kOffsetOriginalRect.y;
    const int fw = kOffsetOriginalRect.width, fh = kOffsetOriginalRect.height;
    cv::line(img, cv::Point(fx, fy), cv::Point(fx + fw, fy), kMarkerColor, 2);
    cv::line(img, cv::Point(fx, fy), cv::Point(fx, fy + fh), kMarkerColor, 2);
    // 指向实际位置的短线（**收在焊点外面**：线头不许碰到焊点）
    cv::line(img, cv::Point(fx, fy),
             cv::Point(kOffsetPadRect.x - 2, kOffsetPadRect.y - 2),
             kMarkerColor, 1);
    cv::putText(img, "offset", cv::Point(285, 110),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, kMarkerColor, 1);

    // 虚焊焊点：先画完整金色，再用底色挖掉 50x50（缺 40%）
    cv::rectangle(img, kColdPadRect, pcbsim::padColor(), -1);
    cv::rectangle(img, kColdMissingRect, pcbsim::backgroundColor(), -1);
    cv::putText(img, "cold", cv::Point(60, 395),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, kMarkerColor, 1);

    // 中央文字 + 缺陷说明
    cv::putText(img, "W3 Defect Test: 1 missing + 1 offset + 1 cold",
                cv::Point(60, 240), cv::FONT_HERSHEY_SIMPLEX,
                0.5, pcbsim::textColor(), 1);

    // 噪声（最后加，和 W2 一样）
    return pcbsim::addNoise(img, seed);
}

cv::Mat goldMask(const cv::Mat& bgr) {
    return pcbsim::goldMask(bgr);
}

cv::Mat toGray(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

cv::Mat openClean(const cv::Mat& mask, int ksize) {
    const int n = (ksize > 0) ? ksize : kMorphKernel;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(n, n));
    cv::Mat out;
    cv::morphologyEx(mask, out, cv::MORPH_OPEN, kernel);
    return out;
}

cv::Mat cannyEdges(const cv::Mat& mask, int low, int high) {
    const int lo = (low >= 0) ? low : kCannyLow;
    const int hi = (high >= 0) ? high : kCannyHigh;
    cv::Mat edges;
    cv::Canny(mask, edges, lo, hi);
    return edges;
}

int countContours(const cv::Mat& mask) {
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(mask, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    return static_cast<int>(contours.size());
}

std::vector<Pad> findPads(const cv::Mat& mask, double minArea) {
    const double limit = (minArea >= 0.0) ? minArea : kMinPadArea;

    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    // RETR_EXTERNAL: 只找最外层轮廓（焊点是实心的，不需要找内部洞）
    // CHAIN_APPROX_SIMPLE: 压缩水平/垂直/对角线段，只保留关键点
    cv::findContours(mask, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<Pad> pads;
    for (const auto& c : contours) {
        const double area = cv::contourArea(c);
        if (area <= limit) continue;

        Pad p;
        p.area = area;
        p.bbox = cv::boundingRect(c);
        const cv::Moments m = cv::moments(c);
        if (m.m00 > 0) {
            p.centroid = cv::Point2d(m.m10 / m.m00, m.m01 / m.m00);
        }
        pads.push_back(p);
    }
    return pads;
}

cv::Mat drawPads(const cv::Mat& bgr, const std::vector<Pad>& pads) {
    cv::Mat marked = bgr.clone();
    for (size_t i = 0; i < pads.size(); ++i) {
        cv::rectangle(marked, pads[i].bbox, cv::Scalar(0, 0, 255), 2);
        cv::circle(marked,
                   cv::Point(static_cast<int>(pads[i].centroid.x),
                             static_cast<int>(pads[i].centroid.y)),
                   4, cv::Scalar(0, 255, 255), -1);
        cv::putText(marked, std::to_string(i + 1),
                    cv::Point(pads[i].bbox.x + 6, pads[i].bbox.y + 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(255, 255, 255), 2);
    }
    return marked;
}

}  // namespace w3
