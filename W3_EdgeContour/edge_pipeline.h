#pragma once
// ============================================================
// W3 的可测逻辑层：形态学 + 边缘 + 轮廓
// ============================================================
//
// ## 为什么要抽出这一层
//
// 2026-10-28 之前，W3 的整条处理链全写死在 main() 里：
// 生成图 → 转 HSV → inRange 提金色 → 开运算 → Canny → findContours
// → 面积过滤 → 标注 → 3 个 imwrite + 3 个 imshow。
//
// 那样写的后果不是「代码难看」，是**这一整层一条测试都写不了**：
// imshow/waitKey 要显示器，imwrite 要往磁盘写 —— 两个都不是无头环境能做的。
// 而 W3 在 2026-10-28 之前**一条自动化测试都没有**（欠账总账里的一条）。
//
// 所以照仓库里已有的先例做（W2 的 color_pipeline.h、W6 的 w6_cli.h/.cpp）：
// **把纯逻辑抽出来，main.cpp 调它，测试也调它** —— 一份实现，两个调用方。
//
// ## 这里暴露的是「语义」，不是「OpenCV 调用序列」
//
// 函数名说的是「提金色」「清噪」「找边缘」「找焊点」，
// 不是「先 cvtColor 再 inRange 再 bitwise_and」。
// 这样测试断的是**行为**，不是实现细节 ——
// 换一种 inRange 的写法不该让测试变红。
//
// ## ⚠ 常量只有这一份
//
// 焊点的位置、面积门槛、Canny 的双阈值、形态学核的大小 ——
// 全在 edge_pipeline.cpp 里，demo 和测试读的是**同一份**。
//
// 抄一份到测试里，就是本仓库栽过五遍的「抄副本」：
// 改了 demo 的面积门槛，测试还在按旧门槛断言，
// 于是测试「绿着」而 demo 已经变了 —— 绿灯什么也没证明。
//
// ## ⚠ 金色的 HSV 窗口**不在这个文件里**
//
// 它在 common/pcb_sim/pcb_sim.h —— W2 和 W3 用的是同一份。
// 两个项目各自抄一份窗口，是本仓库栽过五遍的那件事的第六次机会，
// 这里不接。
//
// ============================================================

#include <opencv2/opencv.hpp>

#include <vector>

namespace w3 {

// ------------------------------------------------------------
// 演示图（带缺陷的模拟 PCB）的布局
// ------------------------------------------------------------

// 5 个**完整**的金色焊点（其中第 2 个是偏移的，见 offsetPadRect）
const std::vector<cv::Rect>& solidPadRects();

// 偏移焊点：实际画在 (220,130)，不是它该在的 (200,100)
const cv::Rect& offsetPadRect();

// 偏移焊点**应该在**的位置 —— 这里只有一个红框，没有金色
const cv::Rect& offsetOriginalRect();

// 虚焊焊点：80x80 的金色，但左上角 50x50 被底色盖掉了
const cv::Rect& coldPadRect();
const cv::Rect& coldMissingRect();

// 缺失焊点：这里什么都没画
const cv::Rect& missingPadRect();

// ------------------------------------------------------------
// 参数 —— 只有这一份
// ------------------------------------------------------------

// 面积门槛：小于这个数的轮廓当成噪点丢掉
double minPadArea();

// Canny 的双阈值（经验值 1:3）
int cannyLowThreshold();
int cannyHighThreshold();

// 形态学核的边长（3 x 3）
int morphKernelSize();

// ------------------------------------------------------------
// 图像生成
// ------------------------------------------------------------

// 生成「带缺陷」的模拟 PCB 测试图：8 个焊点里故意混 3 种缺陷 ——
//   · 缺失焊点：位置 (500,100) 不画
//   · 偏移焊点：位置 (200,100) 改为 (220,130) 偏移
//   · 虚焊焊点：位置 (50,300) 内部挖掉 50x50
//
// seed 是**参数**，不是内部随机数 ——
// 2026-10-28 之前这个函数用 cv::randn 却**没有设种子**，
// 于是同一份代码两次跑出来的图不一样，
// 「这张图长这样」这句话根本没法被引用，测试也就无从写起。
//
// ⚠ 副作用：会重设 OpenCV 的**全局** RNG 状态（cv::theRNG()）。
// 理由和 common/pcb_sim/pcb_sim.h 里一样：OpenCV 4.13 的 randn
// 没有 RNG 参数版本，唯一的入口就是 theRNG()。
cv::Mat generatePcbDefectImage(int seed = 20261028);

// ------------------------------------------------------------
// 处理链
// ------------------------------------------------------------

// 提金色区域：HSV 窗口 → mask（0/255）
//
// 窗口本身在 common/pcb_sim/pcb_sim.h，和 W2 共用同一份。
cv::Mat goldMask(const cv::Mat& bgr);

// BGR → 灰度
//
// 为什么这里有它：demo 里的 Canny 吃的是**二值 mask**，
// 而在二值图上梯度只有 0 和 255 两档，**阈值几乎不起作用**
// （实测 10/30、50/150、100/300 给出的边缘像素数**完全一样**，都是 10084）。
//
// 所以要真去断「双阈值是不是真的在用」，得拿**灰度图**去断 ——
// 而灰度图正是 Canny 的正常输入。
// 这个入口就是给这件事用的。
cv::Mat toGray(const cv::Mat& bgr);

// 形态学开运算（erode 后 dilate）去小噪点。
//
// ksize < 0 表示用 morphKernelSize()。
cv::Mat openClean(const cv::Mat& mask, int ksize = -1);

// Canny 边缘检测。
//
// low < 0 表示用 cannyLowThreshold()，high < 0 表示用 cannyHighThreshold()。
cv::Mat cannyEdges(const cv::Mat& mask, int low = -1, int high = -1);

// 一个焊点
struct Pad {
    cv::Rect bbox;        // 外接矩形
    double area = 0.0;    // 轮廓面积
    cv::Point2d centroid; // 质心（moments 的 m10/m00, m01/m00）
};

// findContours 找到的轮廓总数（**未**做面积过滤）
//
// 单开一个入口，是为了能断「过滤器真的在过滤」——
// 如果只有 findPads，就没法区分「本来就没噪点」和「噪点被滤掉了」。
int countContours(const cv::Mat& mask);

// 按面积找焊点。
//
// minArea < 0 表示用 minPadArea()。
std::vector<Pad> findPads(const cv::Mat& mask, double minArea = -1.0);

// 把焊点标注到图上：红色外接框 + 黄色质心 + 白色编号
cv::Mat drawPads(const cv::Mat& bgr, const std::vector<Pad>& pads);

}  // namespace w3
