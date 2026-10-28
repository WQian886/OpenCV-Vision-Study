#pragma once
// ============================================================
// 模拟 PCB 测试图 —— 全仓库**唯一**的一份「合成图的事实」
// ============================================================
//
// 📁 这个文件 2026-10-28 新增。
//
// ## 为什么要有这个文件
//
// W2 和 W3 都要造一张「模拟 PCB 图」，而这张图的事实是**同一批**：
//
//   · 焊点颜色     金黄 BGR(60,180,220)
//   · 板面底色     深绿 BGR(40,80,40)
//   · 噪声强度     σ = 25
//   · 图的尺寸     640 x 480
//   · 「金色」在 HSV 里是哪个窗口   H=10..30, S>=100, V>=100
//
// 2026-10-28 之前，W2 把这些写在 color_pipeline.cpp 里，
// W3 又在自己的 main.cpp 里**抄了一份**那个 HSV 窗口 ——
// 而 W3 那行代码的注释偏偏写着「复用 W2 思路」。
//
// **注释说复用，代码是抄的。**
//
// 这正是本仓库栽过五遍的那件事：两份定义慢慢漂开，
// 而**两边各自的测试都是绿的** ——
// 因为每一份都只对着自己那份图断言，谁也没跨过去看一眼。
//
// 所以事实只留这一份，两边都从这里取。
//
// ------------------------------------------------------------
// ⚠ 别把它和 common/pcb_core/pcb_color_ref.h 搞混
// ------------------------------------------------------------
//
// 那边也讲「金色」，但那是**真板子照片**上的金层：
// H=85..115，V 下界锚在**逐图估计**的板面基色上（去绝对化）。
// 那边是产线用的判据，这边是**合成图**专用的窗口 ——
// 合成图的焊点是 BGR(60,180,220)，实测色相 H=23，
// 拿真板子的窗口去框它，框不住。
//
// 两套窗口各有各的适用对象，**都不是对方的副本**。
// ============================================================

#include <opencv2/opencv.hpp>

#include <vector>

namespace pcbsim {

// ------------------------------------------------------------
// 颜色（BGR）
// ------------------------------------------------------------

// 焊点：金黄
inline const cv::Scalar& padColor() {
    static const cv::Scalar kPad(60, 180, 220);
    return kPad;
}

// 板面底色：深绿
inline const cv::Scalar& backgroundColor() {
    static const cv::Scalar kBg(40, 80, 40);
    return kBg;
}

// 中性文字（丝印白/灰）
inline const cv::Scalar& textColor() {
    static const cv::Scalar kText(200, 200, 200);
    return kText;
}

// ------------------------------------------------------------
// 「金色」的 HSV 窗口
// ------------------------------------------------------------
//
// ⚠ 这四个数是**量出来的**，不是拍脑袋定的：
// 焊点 BGR(60,180,220) 在 OpenCV 的 8bit HSV 里色相是 **H=23**
// （OpenCV 的公式是 (g-b)*30/delta，不是教科书上的 60*(g-b)/delta ——
//  差 2 倍，按教科书的算法去凑这个窗口，会凑出一个**选不中焊点**的窗口）。
// 实测（2026-10-26，见 notes/notes_W1W2_Tests_2026-10.md）：
// 这个窗口罩住焊点 87.0% 的像素，背景误选只有 0.60%。
inline const cv::Scalar& goldHsvLower() {
    static const cv::Scalar kLo(10, 100, 100);
    return kLo;
}

inline const cv::Scalar& goldHsvUpper() {
    static const cv::Scalar kHi(30, 255, 255);
    return kHi;
}

// ------------------------------------------------------------
// 尺寸
// ------------------------------------------------------------

inline int imageWidth()  { return 640; }
inline int imageHeight() { return 480; }
inline cv::Size imageSize() { return cv::Size(imageWidth(), imageHeight()); }

// 焊点是 80 x 80
inline int padSize() { return 80; }

// 噪声强度 σ
inline double noiseSigma() { return 25.0; }

// W2 那张图的 8 个焊点（4 列 x 2 行）
inline const std::vector<cv::Rect>& padGrid() {
    static const int s = padSize();
    static const std::vector<cv::Rect> kGrid = {
        cv::Rect(50,  100, s, s), cv::Rect(200, 100, s, s),
        cv::Rect(350, 100, s, s), cv::Rect(500, 100, s, s),
        cv::Rect(50,  300, s, s), cv::Rect(200, 300, s, s),
        cv::Rect(350, 300, s, s), cv::Rect(500, 300, s, s),
    };
    return kGrid;
}

// ------------------------------------------------------------
// 噪声
// ------------------------------------------------------------
//
// seed 是**参数**，不是内部随机数。
// 用全局 RNG 的话，同一份代码两次跑出来的图不一样 ——
// 测试就没有可比的基线，"这张图长这样"这句话也没法被引用。
//
// ⚠ 副作用：会重设 OpenCV 的**全局** RNG 状态（cv::theRNG()）。
// 这是为了拿到「带种子的 cv::randn」—— OpenCV 4.13 的 randn 没有
// RNG 参数版本，唯一的入口就是 theRNG()。demo 和测试都是单线程调用，
// 不受影响。
inline cv::Mat addNoise(const cv::Mat& img, int seed, double sigma = noiseSigma()) {
    cv::theRNG() = cv::RNG(seed);
    cv::Mat noise(img.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(sigma));
    cv::Mat out;
    cv::add(img, noise, out, cv::noArray(), CV_8UC3);
    return out;
}

// ------------------------------------------------------------
// 提金色
// ------------------------------------------------------------
//
// BGR → HSV → inRange。
//
// ⚠ hsv 必须是 BGR2HSV 之后的图。这里自己转，不要求调用方先转 ——
// 因为**转两次**和**转一次**的代价一样，但「调用方到底转了没有」
// 是一个没人读的事实：这个项目栽过「字段没人读」的亏。
inline cv::Mat goldMask(const cv::Mat& bgr) {
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, goldHsvLower(), goldHsvUpper(), mask);
    return mask;
}

}  // namespace pcbsim
