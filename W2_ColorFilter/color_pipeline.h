#pragma once
// ============================================================
// W2 的可测逻辑层：色彩空间 + 滤波 + 阈值
//
// ============================================================
//
// ## 为什么要抽出这一层
//
// 2026-10-26 之前，W2 的整条处理链全写死在 main() 里：
// 生成图 → 转 HSV → inRange 提金色 → 三种滤波 → 三种阈值，
// 外面还挂着 10 个 imwrite 和 3 个 imshow。
//
// 那样写的后果不是"代码难看"，是**这一整层一条测试都写不了**：
// imshow/waitKey 要显示器，imwrite 要往磁盘写 —— 两个都不是无头环境能做的。
// 而 W1–W5 在 2026-10-26 之前**一条自动化测试都没有**（欠账总账里的一条）。
//
// 所以照仓库里已有的先例做（W6 的 w6_cli.h/.cpp、W7 的 step_scope.h）：
// **把纯逻辑抽出来，main.cpp 调它，测试也调它** —— 一份实现，两个调用方。
//
// ## 这里暴露的是「语义」，不是「OpenCV 调用序列」
//
// 函数名说的是"提金色""找边缘""算阈值"，
// 不是"先 cvtColor 再 inRange 再 bitwise_and"。
// 这样测试断的是**行为**，不是实现细节 ——
// 换一种 inRange 的写法不该让测试变红。
//
// ## ⚠ 常量只有这一份
//
// 金色的 HSV 窗口、焊点的位置和颜色、滤波核大小 —— 全在 color_pipeline.cpp 里，
// demo 和测试读的是**同一份**。
//
// 抄一份到测试里，就是本仓库栽过五遍的「抄副本」：
// 改了 demo 的窗口，测试还在按旧窗口断言，
// 于是测试「绿着」而 demo 已经变了 —— 绿灯什么也没证明。
//
// ### 2026-10-28：颜色和窗口搬去了 common/pcb_sim/pcb_sim.h
//
// 「合成 PCB 图的事实」现在只有一份，W2 和 W3 共用 ——
// 起因是 W3 抄了一份 HSV 窗口，而那行代码的注释写着「复用 W2 思路」。
//
// 下面这些入口（padRects / padColor / backgroundColor /
// goldHsvLower / goldHsvUpper）**保留不变**，
// W2 的 demo、测试和公开约定都指着它们 ——
// 它们现在是 common/pcb_sim 的转发。**名字还是那些名字，实现只剩一份。**
//
// ============================================================

#include <opencv2/opencv.hpp>

#include <vector>

namespace w2 {

// ------------------------------------------------------------
// 演示用图的常量 —— demo 和测试共用这一份
// ------------------------------------------------------------

// 8 个金色焊点的位置（4 列 × 2 行）
const std::vector<cv::Rect>& padRects();

// 焊点颜色（BGR）：金黄
const cv::Scalar& padColor();

// 背景颜色（BGR）：深绿 PCB 底色
const cv::Scalar& backgroundColor();

// 金色在 HSV 里的窗口：H=10..30（黄到橙），S>=100（够饱和），V>=100（够亮）
//
// ⚠ 这三个数是**量出来的**，不是拍脑袋定的：
// 焊点 BGR(60,180,220) 在 OpenCV 的 8bit HSV 里色相是 **H=23**
// （OpenCV 的公式是 (g-b)*30/delta，不是教科书上的 60*(g-b)/delta ——
//  差 2 倍，按教科书的算法去凑这个窗口，会凑出一个**选不中焊点**的窗口）。
// 实测（2026-10-26，见 notes/notes_W1W2_Tests_2026-10.md）：
// 这个窗口罩住焊点 87.0% 的像素，背景误选只有 0.60%。
const cv::Scalar& goldHsvLower();
const cv::Scalar& goldHsvUpper();

// ------------------------------------------------------------
// 图像生成
// ------------------------------------------------------------

// 生成模拟 PCB 的测试图：深绿底 + 8 个金色焊点 + 文字 + 高斯噪声。
//
// seed 是**参数**，不是内部随机数。
// 用全局 RNG 的话，同一份代码两次跑出来的图不一样 ——
// 测试就没有可比的基线，"这张图长这样"这句话也没法被引用。
// 固定 seed 之后，这是**一张固定的图**，不是"一张像这样的图"。
//
// ⚠ 副作用：会重设 OpenCV 的**全局** RNG 状态（cv::theRNG()）。
// 这是为了拿到"带种子的 cv::randn" —— OpenCV 4.13 的 randn 没有 RNG 参数版本，
// 唯一的入口就是 theRNG()。demo 和测试都是单线程调用，不受影响。
cv::Mat generatePcbLikeImage(int seed = 20261026);

// ------------------------------------------------------------
// 色彩空间
// ------------------------------------------------------------

// BGR → HSV
cv::Mat toHsv(const cv::Mat& bgr);

// BGR → 灰度
cv::Mat toGray(const cv::Mat& bgr);

// ------------------------------------------------------------
// 颜色提取
// ------------------------------------------------------------

// 提金色区域：HSV 窗口 → mask（0/255）
cv::Mat goldMask(const cv::Mat& bgr);

// 提金色区域：只留下 mask 里的原图，mask 外全黑
cv::Mat goldOnly(const cv::Mat& bgr);

// ------------------------------------------------------------
// 滤波
// ------------------------------------------------------------

cv::Mat gaussianBlur(const cv::Mat& src, int ksize = 5);
cv::Mat medianBlur(const cv::Mat& src, int ksize = 5);
cv::Mat bilateralDenoise(const cv::Mat& src, int d = 9,
                          double sigmaColor = 75, double sigmaSpace = 75);

// ------------------------------------------------------------
// 阈值
// ------------------------------------------------------------

// 固定阈值（THRESH_BINARY）
cv::Mat thresholdFixed(const cv::Mat& gray, int thresh = 127);

// 自适应阈值（邻域均值，ADAPTIVE_THRESH_MEAN_C）
cv::Mat thresholdAdaptive(const cv::Mat& gray, int blockSize = 11, int c = 2);

// Otsu 自动阈值。返回二值图。
//
// 阈值本身通过 usedThresh 带出来 —— cv::threshold 的返回值就是它，
// 别在别处再抄一份（抄的那份会漂）。
cv::Mat thresholdOtsu(const cv::Mat& gray, double* usedThresh = nullptr);

}  // namespace w2
