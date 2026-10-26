#include "color_pipeline.h"

namespace w2 {
namespace {

// ------------------------------------------------------------
// 常量 —— 全部只在这里定义一次
// ------------------------------------------------------------

const std::vector<cv::Rect> kPadRects = {
    cv::Rect(50,  100, 80, 80), cv::Rect(200, 100, 80, 80),
    cv::Rect(350, 100, 80, 80), cv::Rect(500, 100, 80, 80),
    cv::Rect(50,  300, 80, 80), cv::Rect(200, 300, 80, 80),
    cv::Rect(350, 300, 80, 80), cv::Rect(500, 300, 80, 80),
};

const cv::Scalar kPadColor(60, 180, 220);        // 金黄（BGR）
const cv::Scalar kBackgroundColor(40, 80, 40);    // 深绿 PCB 底色（BGR）
const cv::Scalar kTextColor(200, 200, 200);       // 中央文字

const cv::Scalar kGoldHsvLower(10, 100, 100);
const cv::Scalar kGoldHsvUpper(30, 255, 255);

const int kImageWidth = 640;
const int kImageHeight = 480;
const double kNoiseSigma = 25.0;

}  // namespace

const std::vector<cv::Rect>& padRects() { return kPadRects; }
const cv::Scalar& padColor() { return kPadColor; }
const cv::Scalar& backgroundColor() { return kBackgroundColor; }
const cv::Scalar& goldHsvLower() { return kGoldHsvLower; }
const cv::Scalar& goldHsvUpper() { return kGoldHsvUpper; }

cv::Mat generatePcbLikeImage(int seed) {
    cv::Mat img(kImageHeight, kImageWidth, CV_8UC3, kBackgroundColor);

    for (const auto& p : kPadRects) {
        cv::rectangle(img, p, kPadColor, -1);
    }

    cv::putText(img, "W2 Color Filter Test", cv::Point(150, 240),
                cv::FONT_HERSHEY_SIMPLEX, 0.8, kTextColor, 2);

    // 噪声（模拟传感器噪声，让滤波有"事可做"）。
    // OpenCV 4.13 的 cv::randn 没有 RNG 参数版本，唯一的入口是全局 theRNG()，
    // 所以"带种子"只能通过重设它来实现。
    cv::theRNG() = cv::RNG(seed);
    cv::Mat noise(img.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(kNoiseSigma));
    cv::add(img, noise, img, cv::noArray(), CV_8UC3);

    return img;
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
    cv::Mat hsv = toHsv(bgr);
    cv::Mat mask;
    cv::inRange(hsv, kGoldHsvLower, kGoldHsvUpper, mask);
    return mask;
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
