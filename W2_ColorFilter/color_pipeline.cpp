#include "color_pipeline.h"

#include "pcb_sim.h"

namespace w2 {
namespace {

// ------------------------------------------------------------
// 常量 —— **不在这里**。
//
// 2026-10-28 之前它们就在这个文件里（color_pipeline.cpp）。
// 同一天 W3 又抄了一份 HSV 窗口在自己的 main.cpp 里，
// 而那行代码的注释写着「复用 W2 思路」——
// 注释说复用，代码是抄的。
//
// 于是事实搬到 common/pcb_sim/pcb_sim.h：W2 和 W3 从同一份取。
// 下面的 w2::padRects() / w2::goldHsvLower() 这些入口**保留不变**
// （W2 的 demo、测试、以及 W2 的公开约定都指着它们），
// 它们现在只是 pcbsim 的转发。
//
// 换句话说：**名字还是那些名字，实现只剩一份。**
// ------------------------------------------------------------

}  // namespace

const std::vector<cv::Rect>& padRects() { return pcbsim::padGrid(); }
const cv::Scalar& padColor() { return pcbsim::padColor(); }
const cv::Scalar& backgroundColor() { return pcbsim::backgroundColor(); }
const cv::Scalar& goldHsvLower() { return pcbsim::goldHsvLower(); }
const cv::Scalar& goldHsvUpper() { return pcbsim::goldHsvUpper(); }

cv::Mat generatePcbLikeImage(int seed) {
    cv::Mat img(pcbsim::imageSize(), CV_8UC3, pcbsim::backgroundColor());

    for (const auto& p : pcbsim::padGrid()) {
        cv::rectangle(img, p, pcbsim::padColor(), -1);
    }

    cv::putText(img, "W2 Color Filter Test", cv::Point(150, 240),
                cv::FONT_HERSHEY_SIMPLEX, 0.8, pcbsim::textColor(), 2);

    // 噪声（模拟传感器噪声，让滤波有"事可做"）。
    // 种子是**参数** —— 理由和副作用见 common/pcb_sim/pcb_sim.h。
    return pcbsim::addNoise(img, seed);
}

cv::Mat toHsv(const cv::Mat& bgr) {
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    return hsv;
}

cv::Mat toGray(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

cv::Mat goldMask(const cv::Mat& bgr) {
    // 窗口在 common/pcb_sim/pcb_sim.h —— 和 W3 共用同一份。
    return pcbsim::goldMask(bgr);
}

cv::Mat goldOnly(const cv::Mat& bgr) {
    cv::Mat mask = goldMask(bgr);
    cv::Mat out;
    cv::bitwise_and(bgr, bgr, out, mask);
    return out;
}

cv::Mat gaussianBlur(const cv::Mat& src, int ksize) {
    cv::Mat out;
    cv::GaussianBlur(src, out, cv::Size(ksize, ksize), 0);
    return out;
}

cv::Mat medianBlur(const cv::Mat& src, int ksize) {
    cv::Mat out;
    cv::medianBlur(src, out, ksize);
    return out;
}

cv::Mat bilateralDenoise(const cv::Mat& src, int d,
                          double sigmaColor, double sigmaSpace) {
    cv::Mat out;
    cv::bilateralFilter(src, out, d, sigmaColor, sigmaSpace);
    return out;
}

cv::Mat thresholdFixed(const cv::Mat& gray, int thresh) {
    cv::Mat out;
    cv::threshold(gray, out, thresh, 255, cv::THRESH_BINARY);
    return out;
}

cv::Mat thresholdAdaptive(const cv::Mat& gray, int blockSize, int c) {
    cv::Mat out;
    cv::adaptiveThreshold(gray, out, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                          cv::THRESH_BINARY, blockSize, c);
    return out;
}

cv::Mat thresholdOtsu(const cv::Mat& gray, double* usedThresh) {
    cv::Mat out;
    const double t = cv::threshold(gray, out, 0, 255,
                                   cv::THRESH_BINARY | cv::THRESH_OTSU);
    if (usedThresh) *usedThresh = t;
    return out;
}

}  // namespace w2
