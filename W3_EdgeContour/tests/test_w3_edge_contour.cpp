// ============================================================
// W3（形态学 + 边缘 + 轮廓）的无头回归测试
// ============================================================
//
// ## 这个套件在补什么
//
// 2026-10-28 之前，W3 **一条自动化测试都没有** ——
// 和 W4、W5 一起，是欠账总账里"W3–W5 零测试缺口"那一条。
//
// 原因不是"没时间"，是**写不了**：整条处理链写在 main() 里，
// 外面挂着 imshow / waitKey（要显示器）和 3 个 imwrite（要写盘）。
// 无头环境里这两样都做不了，于是这一层就永远只能靠人眼看。
//
// 现在把纯逻辑抽到 edge_pipeline.{h,cpp}（main.cpp 和测试调的是同一份），
// 这个套件就是那一层的回归网。
//
// ## ⚠ 这里的期望值都是**量出来的**，不是拍脑袋定的
//
// 2026-10-28 一次跑完 52 条断言，把该量的都量了（数字见每条断言后面）：
//
//   · 金色窗口罩住焊点 86.3%~87.2% 的像素，背景误选 0.56%
//   · 开运算后轮廓总数 48，过滤后剩 6 个（5 个完整 + 1 个虚焊）
//   · 完整焊点面积 3699~5289，虚焊那个 2342
//   · 偏移焊点的质心 (257.0, 172.5)，离它该在的位置 36.6 像素
//   · 二值 mask 上 Canny 换阈值边缘数**一个不变**（都是 10117）
//   · 灰度图上 Canny 换阈值边缘数 112948 / 102476 / 5977
//
// ⚠ 上面这组数是**修完红色标记之后**的数。
// 修之前：轮廓 53 个、mask 上 Canny 10084 个、灰度 112881 / 102382 / 6215。
// 差异不是噪声 —— 是那个红色空框把偏移焊点切成了三块（1602 / 918 / 876），
// 详见 edge_pipeline.cpp 里 kOffsetPadRect 那段注释。
//
// 断言的门槛都比实测值**留了余量**（噪声的实现细节可能随 OpenCV 版本变，
// 门槛卡死在一个实测值上，就成了"只有这台机器成立"的断言 ——
// 那正是本仓库在 CI 第一次真跑时栽过的坑）。
//
// ## ⚠ 断的是**行为**，不是 OpenCV 的行为
//
// 断言全部走 w3:: 的入口，不直接调 cv::inRange / cv::Canny 去断它们的行为。
// W2 那轮栽过一次：断言写的是 OpenCV 的行为，
// 于是「把 bilateralFilter 换成 Gaussian」这种改动测试**照样绿** ——
// 绿灯什么也没证明。
//
// 这里断的是**这一层自己的约定**：窗口罩不罩得住焊点、
// 开运算会不会吃掉焊点、面积门槛是不是真的在过滤、
// 三个缺陷是不是真的被认出来了、**以及这个数是不是门槛的产物**。
//
// ## 这套测试**不**做什么
//
// 不写任何文件、不开任何窗口 —— 产物一个字节都不落地，
// 所以 CI 的"源码树必须干净"那一步对它是空转（这本身也是对的：
// 没有产物，就没有产物泄漏）。
//
// ============================================================

#include "edge_pipeline.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
// 极简断言（与 W1 / W2 / W6 / W7 各套件同一套风格）
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static std::string g_case;

static void beginCase(const std::string& name) {
    g_case = name;
    std::cout << "\n=== " << name << " ===" << std::endl;
}

static void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::cout << "  [PASS] " << what << std::endl;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << what << "  (case:" << g_case << ")" << std::endl;
    }
}

// 一个 Rect 里被置位的像素数
static int pixelsIn(const cv::Mat& mask, const cv::Rect& r) {
    const cv::Rect clipped = r & cv::Rect(0, 0, mask.cols, mask.rows);
    if (clipped.width <= 0 || clipped.height <= 0) return 0;
    return cv::countNonZero(mask(clipped));
}

// 一个 Rect 里被置位的像素数，但**扣掉**另一块 Rect 覆盖到的部分
//
// 为什么需要它：偏移焊点画在 (220,130)，而它"应该在"的 (200,100)
// 和它**有重叠** —— 直接数 (200,100) 里的金色，
// 数到的是偏移焊点漏过来的那一块，不是「原位置的金色」。
// 概念上要说「原位置**除了偏移焊点压住的那部分**以外还有没有金色」，
// 就必须先把压住的部分扣掉。
static int pixelsInOutside(const cv::Mat& mask, const cv::Rect& r,
                           const cv::Rect& exclude) {
    int n = 0;
    for (int y = r.y; y < r.y + r.height; ++y) {
        if (y < 0 || y >= mask.rows) continue;
        for (int x = r.x; x < r.x + r.width; ++x) {
            if (x < 0 || x >= mask.cols) continue;
            if (exclude.area() > 0 &&
                x >= exclude.x && x < exclude.x + exclude.width &&
                y >= exclude.y && y < exclude.y + exclude.height)
                continue;
            if (mask.at<uchar>(y, x) != 0) ++n;
        }
    }
    return n;
}

// 背景取样区：避开焊点、避开文字、避开红色标记
static const std::vector<cv::Rect>& backgroundSamples() {
    static const std::vector<cv::Rect> kSamples = {
        cv::Rect(10, 10, 30, 30), cv::Rect(300, 30, 40, 40),
        cv::Rect(600, 10, 30, 30), cv::Rect(600, 200, 30, 30),
        cv::Rect(450, 250, 40, 30), cv::Rect(10, 430, 40, 30),
        cv::Rect(290, 440, 40, 30), cv::Rect(600, 430, 30, 30),
    };
    return kSamples;
}

// 一个焊点的边框上（外扩 band 像素的环带里）有多少边缘像素
static int edgePixelsAround(const cv::Mat& edges, const cv::Rect& bbox, int band) {
    const cv::Rect outer(bbox.x - band, bbox.y - band,
                          bbox.width + 2 * band, bbox.height + 2 * band);
    const cv::Rect inner(bbox.x + band, bbox.y + band,
                         bbox.width - 2 * band, bbox.height - 2 * band);
    const cv::Rect clipped = outer & cv::Rect(0, 0, edges.cols, edges.rows);
    if (clipped.width <= 0 || clipped.height <= 0) return 0;
    int n = cv::countNonZero(edges(clipped));
    if (inner.width > 0 && inner.height > 0) {
        n -= cv::countNonZero(edges(inner));
    }
    return n;
}

// 质心离某个点有多远
static double distTo(const cv::Point2d& p, int x, int y) {
    return std::sqrt((p.x - x) * (p.x - x) + (p.y - y) * (p.y - y));
}

// ------------------------------------------------------------
// 1. 生成图
// ------------------------------------------------------------
static void test_image_generation() {
    beginCase("生成模拟 PCB 缺陷图");

    const cv::Mat img = w3::generatePcbDefectImage();
    check(img.cols == 640 && img.rows == 480,
          "尺寸是 640x480（实际 " + std::to_string(img.cols) + "x" +
              std::to_string(img.rows) + "）");
    check(img.type() == CV_8UC3, "类型是 CV_8UC3（8 位无符号 3 通道）");

    // 布局自洽：三个缺陷各在自己的位置上，且互不打架
    const cv::Rect& cold = w3::coldPadRect();
    const cv::Rect& coldMissing = w3::coldMissingRect();
    const cv::Rect& off = w3::offsetPadRect();
    const cv::Rect& offOrig = w3::offsetOriginalRect();
    const cv::Rect& missing = w3::missingPadRect();

    check((coldMissing & cold).area() == coldMissing.area(),
          "虚焊挖掉的那块在虚焊焊点里面");
    check(off.tl() != offOrig.tl(),
          "偏移焊点不在它该在的位置上（该在 " +
              std::to_string(offOrig.x) + "," + std::to_string(offOrig.y) +
              "，实际在 " + std::to_string(off.x) + "," + std::to_string(off.y) +
              "）");
    check(off.x - offOrig.x == 20 && off.y - offOrig.y == 30,
          "偏移量是 (20,30)");
    check((off & missing).area() == 0,
          "偏移焊点和缺失焊点不重叠");
    check(coldMissing.width == 50 && coldMissing.height == 50,
          "虚焊挖掉的是 50x50");

    // 5 个完整焊点 + 1 个虚焊 = 图里一共 6 块金色
    check(w3::solidPadRects().size() == 5,
          "有 5 个完整的金色焊点（实际 " +
              std::to_string(w3::solidPadRects().size()) + "）");

    // 同一个 seed → 同一张图。
    // ⚠ 这条在 2026-10-28 之前**根本写不出来**：
    // 老的生成器用 cv::randn 却没设种子，两次跑出来的是两张不同的图。
    const cv::Mat again = w3::generatePcbDefectImage();
    check(cv::norm(img, again, cv::NORM_INF) == 0,
          "同一个 seed 生成的是同一张图（逐像素相同）");

    const cv::Mat other = w3::generatePcbDefectImage(12345);
    check(cv::norm(img, other, cv::NORM_INF) > 0,
          "不同 seed 生成的是不同的图（噪声确实带种子）");
}

// ------------------------------------------------------------
// 2. 金色 mask
// ------------------------------------------------------------
static void test_gold_mask() {
    beginCase("inRange 提取金色");

    const cv::Mat img = w3::generatePcbDefectImage();
    const cv::Mat mask = w3::goldMask(img);

    check(mask.type() == CV_8UC1, "mask 是单通道 8 位");
    check(mask.size() == img.size(), "mask 尺寸和原图一样");

    bool onlyZeroOr255 = true;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            const uchar v = mask.at<uchar>(y, x);
            if (v != 0 && v != 255) onlyZeroOr255 = false;
        }
    check(onlyZeroOr255, "mask 的取值只有 0 和 255");

    // 每个**完整**焊点：绝大多数像素要被选中（实测 86.3%~87.2%，门槛 75%）
    double worst = 1.0;
    for (const auto& p : w3::solidPadRects()) {
        const double frac =
            static_cast<double>(pixelsIn(mask, p)) / static_cast<double>(p.area());
        worst = std::min(worst, frac);
    }
    check(worst >= 0.75,
          "每个完整焊点至少 75% 的像素被选中（实测最差 " +
              std::to_string((int)(worst * 1000) / 10.0) + "%）");

    // 背景：几乎不被选中（实测 0.56%，门槛 2%）
    int bgHit = 0, bgArea = 0;
    for (const auto& r : backgroundSamples()) {
        bgHit += pixelsIn(mask, r);
        bgArea += r.area();
    }
    const double bgFrac = static_cast<double>(bgHit) / bgArea;
    check(bgFrac <= 0.02,
          "背景误选不超过 2%（实测 " +
              std::to_string((int)(bgFrac * 10000) / 100.0) + "%）");

    // 三个缺陷位上，**不该有金色**
    //
    // ⚠ 这里断的是**误选率**，不是"一个像素都不许有" ——
    // 那三块地方是**带噪声的板面**，噪声本来就会被窗口误选一点
    // （实测背景误选 0.56%）。要求"一个都没有"等于要求噪声不存在，
    // 那条断言只会在没有噪声的图上成立。
    const int atMissing = pixelsIn(mask, w3::missingPadRect());
    check(atMissing <= w3::missingPadRect().area() * 0.02,
          "缺失焊点的位置误选不超过 2%（实测 " + std::to_string(atMissing) +
              " / " + std::to_string(w3::missingPadRect().area()) + "）");

    // 偏移焊点原来的位置：要把偏移焊点压住的那部分**扣掉**再数
    const int atOffsetOrig = pixelsInOutside(mask, w3::offsetOriginalRect(),
                                             w3::offsetPadRect());
    const int origFreeArea = w3::offsetOriginalRect().area() -
                             (w3::offsetOriginalRect() & w3::offsetPadRect()).area();
    check(atOffsetOrig <= origFreeArea * 0.02,
          "偏移焊点原来的位置误选不超过 2%（实测 " + std::to_string(atOffsetOrig) +
              " / " + std::to_string(origFreeArea) + "）");

    // 虚焊：挖掉的那 50x50 里没有金色，整块里有一部分金色
    const int atColdMissing = pixelsIn(mask, w3::coldMissingRect());
    const int atCold = pixelsIn(mask, w3::coldPadRect());
    check(atColdMissing <= w3::coldMissingRect().area() * 0.02,
          "虚焊挖掉的那块里误选不超过 2%（实测 " +
              std::to_string(atColdMissing) + " / " +
              std::to_string(w3::coldMissingRect().area()) + "）");
    check(atCold > 1000,
          "虚焊焊点剩下的那部分还是金色（实测 " + std::to_string(atCold) +
              " 个像素）");
}

// ------------------------------------------------------------
// 3. 形态学开运算
// ------------------------------------------------------------
static void test_morphology() {
    beginCase("形态学开运算");

    // 孤立的单个白点：开运算必须把它吃掉 —— 这是开运算**定义**上的行为，
    // 不依赖任何噪声实现细节，所以这条断言是"任何机器上都成立"的。
    cv::Mat lonely(200, 200, CV_8UC1, cv::Scalar(0));
    lonely.at<uchar>(100, 100) = 255;
    check(cv::countNonZero(w3::openClean(lonely)) == 0,
          "孤立的一个白点被开运算吃掉了（实测剩 " +
              std::to_string(cv::countNonZero(w3::openClean(lonely))) + " 个）");

    // 一个实心方块：开运算不该改变它（腐蚀一圈再膨胀一圈）
    cv::Mat block(200, 200, CV_8UC1, cv::Scalar(0));
    cv::rectangle(block, cv::Rect(60, 60, 80, 80), cv::Scalar(255), -1);
    const int before = cv::countNonZero(block);
    const int after = cv::countNonZero(w3::openClean(block));
    check(after >= before - 4 && after <= before + 4,
          "实心方块开运算前后大小不变（" + std::to_string(before) + " → " +
              std::to_string(after) + "）");

    // 真图上：开运算只会去掉像素，不会增加
    const cv::Mat img = w3::generatePcbDefectImage();
    const cv::Mat mask = w3::goldMask(img);
    const cv::Mat opened = w3::openClean(mask);
    check(cv::countNonZero(opened) <= cv::countNonZero(mask),
          "开运算不会让金色像素变多（" + std::to_string(cv::countNonZero(mask)) +
              " → " + std::to_string(cv::countNonZero(opened)) + "）");
    // 开运算会吃掉噪点，也会把焊点的边啃掉一圈 ——
    // 实测剩 75.4%（32379 → 24423），所以门槛放在 70%。
    check(cv::countNonZero(opened) > cv::countNonZero(mask) * 0.70,
          "开运算保住了大部分金色（剩 " +
              std::to_string(cv::countNonZero(opened)) + " / " +
              std::to_string(cv::countNonZero(mask)) + "）");
}

// ------------------------------------------------------------
// 4. Canny 边缘
// ------------------------------------------------------------
static void test_canny() {
    beginCase("Canny 边缘检测");

    const cv::Mat img = w3::generatePcbDefectImage();
    const cv::Mat mask = w3::openClean(w3::goldMask(img));
    const cv::Mat edges = w3::cannyEdges(mask);

    check(edges.type() == CV_8UC1, "边缘图是单通道 8 位");
    check(edges.size() == mask.size(), "边缘图尺寸和 mask 一样");

    const int nDefault = cv::countNonZero(edges);
    check(nDefault > 0, "边缘图不是空的（" + std::to_string(nDefault) + " 个像素）");

    // ⚠ 在**二值 mask** 上，双阈值几乎不起作用 ——
    // 梯度只有 0 和 255 两档，三档阈值给出的是**同一个数**（实测都是 10117）。
    // 这不是假设，是量出来的。
    const int nMaskLow = cv::countNonZero(w3::cannyEdges(mask, 10, 30));
    const int nMaskHigh = cv::countNonZero(w3::cannyEdges(mask, 100, 300));
    check(nMaskLow == nDefault && nMaskHigh == nDefault,
          "二值 mask 上换阈值边缘数不变（实测 10/30 → " +
              std::to_string(nMaskLow) + "，默认 → " + std::to_string(nDefault) +
              "，100/300 → " + std::to_string(nMaskHigh) + "）");

    // 阈值**要用**，得拿灰度图去用 —— 那里梯度是连续的。
    // 阈值越低，弱边缘被放进来得越多（Canny 的迟滞判定是单调的：
    // 降阈值只会**加**边缘，不会去掉强边缘）。
    const cv::Mat gray = w3::toGray(img);
    const int gLow = cv::countNonZero(w3::cannyEdges(gray, 10, 30));
    const int gDefault = cv::countNonZero(w3::cannyEdges(gray));
    const int gHigh = cv::countNonZero(w3::cannyEdges(gray, 100, 300));
    check(gLow >= gDefault && gDefault >= gHigh,
          "灰度图上阈值越低边缘越多（10/30 → " + std::to_string(gLow) +
              "，默认 → " + std::to_string(gDefault) +
              "，100/300 → " + std::to_string(gHigh) + "）");
    check(gLow > gDefault,
          "灰度图上降阈值确实加进来了边缘（" + std::to_string(gLow) +
              " vs " + std::to_string(gDefault) + "）");

    // 阈值高到梯度都够不着，就一条边缘都没有了
    const int nNone = cv::countNonZero(w3::cannyEdges(gray, 1000, 2000));
    check(nNone == 0,
          "阈值高到 1000/2000 就一条边缘都不剩（实测 " +
              std::to_string(nNone) + " 个）");
}

// ------------------------------------------------------------
// 5. 找焊点
// ------------------------------------------------------------
static void test_find_pads() {
    beginCase("按面积找焊点");

    const cv::Mat img = w3::generatePcbDefectImage();
    const cv::Mat mask = w3::openClean(w3::goldMask(img));

    const int total = w3::countContours(mask);
    const std::vector<w3::Pad> pads = w3::findPads(mask);

    check(total > static_cast<int>(pads.size()),
          "面积过滤真的在过滤（未过滤 " + std::to_string(total) +
              " 个轮廓，过滤后剩 " + std::to_string(pads.size()) + " 个）");

    // 5 个完整焊点 + 1 个虚焊 = 6
    check(pads.size() == 6,
          "找出 6 个焊点：5 个完整的 + 1 个虚焊的（实际 " +
              std::to_string(pads.size()) + " 个）");

    // 实测清单：每个焊点的外接矩形、面积、质心
    // （留在输出里，是为了让"6 个"这个数**看得见**，
    //   而不是只留一句"找到了 6 个"。）
    for (size_t i = 0; i < pads.size(); ++i) {
        std::cout << "    pad " << (i + 1) << ": bbox=(" << pads[i].bbox.x << ","
                  << pads[i].bbox.y << "," << pads[i].bbox.width << ","
                  << pads[i].bbox.height << ") area=" << std::fixed
                  << std::setprecision(0) << pads[i].area << " centroid=("
                  << std::setprecision(1) << pads[i].centroid.x << ","
                  << pads[i].centroid.y << ")" << std::endl;
    }

    // ⚠⚠ 这个数**不能只是**"6"。
    //
    // 2026-10-28 第一版就是这么写的，结果一改门槛它就变 ——
    // 因为那时候红色偏移标记画在焊点**上面**，把焊点切成了
    // 1602 / 918 / 876 三块，后两块卡在 1000 下面。
    // 那时候的"6"是**门槛的产物**，不是图的事实。
    //
    // 所以这里断的是：**门槛在 20 倍的范围里动，个数都不变**。
    // 这条断言专门用来抓「改一个阈值，结论就翻」这种病。
    //
    // ⚠ 上界为什么停在 2000：虚焊那个焊点的面积是 2342 ——
    // 门槛一高它先走（实测 3000 时只剩 5 个）。
    // 「6 个」这个数成立的范围是 100 ~ 2341。
    bool stableCount = true;
    std::string counts;
    for (double limit : {100.0, 300.0, 600.0, 1000.0, 1500.0, 2000.0}) {
        const size_t n = w3::findPads(mask, limit).size();
        counts += std::to_string((int)limit) + "→" + std::to_string((int)n) + " ";
        if (n != pads.size()) stableCount = false;
    }
    check(stableCount,
          "门槛从 100 到 2000（20 倍），找到的个数都是 6（实测 " + counts + "）");

    // 面积门槛的敏感性：门槛放到 100 会放进来更多东西，放到极大就一个不剩
    const std::vector<w3::Pad> loose = w3::findPads(mask, 100.0);
    const std::vector<w3::Pad> strict = w3::findPads(mask, 1e9);
    check(loose.size() >= pads.size(),
          "面积门槛放宽到 100，放进来的只会更多（" +
              std::to_string(loose.size()) + " 个）");
    check(strict.empty(),
          "面积门槛放大到 1e9，一个焊点都不剩（实测 " +
              std::to_string(strict.size()) + " 个）");

    // 逐个焊点：质心在自己的外接矩形里，面积和外接矩形对得上
    bool centroidInside = true;
    bool areaMatchesBox = true;
    for (const auto& p : pads) {
        if (p.centroid.x < p.bbox.x || p.centroid.x > p.bbox.x + p.bbox.width ||
            p.centroid.y < p.bbox.y || p.centroid.y > p.bbox.y + p.bbox.height)
            centroidInside = false;
        const double boxArea = static_cast<double>(p.bbox.width) * p.bbox.height;
        if (p.area > boxArea || p.area < boxArea * 0.25) areaMatchesBox = false;
    }
    check(centroidInside, "每个焊点的质心都在自己的外接矩形里");
    check(areaMatchesBox, "每个焊点的面积和外接矩形对得上");

    // --- 三个缺陷是不是真的被认出来了 ---
    //
    // 这一段才是这个套件真正的价值：
    // 光断「找到 6 个」不够 —— 一个**只数数量**的检查器，
    // 哪怕把图换成一张全是空白的图也能过。
    // 这里断的是**位置**和**形状**。

    // (a) 缺失的焊点：那个位置上不能有焊点
    bool foundAtMissing = false;
    for (const auto& p : pads) {
        if ((p.bbox & w3::missingPadRect()).area() > 0) foundAtMissing = true;
    }
    check(!foundAtMissing, "缺失焊点的位置上没有焊点");

    // (b) 偏移的焊点：焊点在偏移后的位置，不在原来的位置
    bool foundAtOffset = false, foundAtOriginal = false;
    double offsetDist = 999.0, originalDist = 999.0;
    for (const auto& p : pads) {
        offsetDist = std::min(offsetDist, distTo(p.centroid, 260, 170));
        originalDist = std::min(originalDist, distTo(p.centroid, 240, 140));
        if (distTo(p.centroid, 260, 170) < 8.0) foundAtOffset = true;
        if (distTo(p.centroid, 240, 140) < 8.0) foundAtOriginal = true;
    }
    check(foundAtOffset,
          "偏移的焊点是在偏移后的位置 (260,170) 附近被找到的（实测最近 " +
              std::to_string((int)(offsetDist * 10) / 10.0) + " 像素）");
    check(!foundAtOriginal,
          "偏移的焊点没有出现在它原来的位置 (240,140)（实测最近 " +
              std::to_string((int)(originalDist * 10) / 10.0) + " 像素）");

    // (c) 虚焊的焊点：面积比完整焊点小，外接矩形还是整块
    const w3::Pad* cold = nullptr;
    const w3::Pad* solid = nullptr;
    for (const auto& p : pads) {
        if (distTo(p.centroid, 90, 340) < 25.0) cold = &p;
        if (distTo(p.centroid, 90, 140) < 25.0) solid = &p;
    }
    check(cold != nullptr, "找到了 (50,300) 那个虚焊的焊点");
    check(solid != nullptr, "找到了 (50,100) 那个完整的焊点");
    if (cold && solid) {
        check(cold->area < solid->area * 0.8,
              "虚焊焊点的面积比完整焊点小（虚焊 " +
                  std::to_string((int)cold->area) + " vs 完整 " +
                  std::to_string((int)solid->area) + "）");
        check(cold->bbox.width >= 60 && cold->bbox.height >= 60,
              "虚焊焊点的外接矩形仍然是整块（" +
                  std::to_string(cold->bbox.width) + "x" +
                  std::to_string(cold->bbox.height) + "）");
    }

    // (d) 完整的焊点：外接矩形都是整块 80x80，质心落在矩形正中附近
    int fullSized = 0;
    double worstOffset = 0.0;
    double minSolidArea = 1e9, maxSolidArea = 0.0;
    for (const auto& p : pads) {
        if (p.bbox.width == 80 && p.bbox.height == 80) ++fullSized;
        if (distTo(p.centroid, 90, 140) < 25.0) {
            worstOffset = std::max(worstOffset, distTo(p.centroid, 90, 140));
        }
        if (!(cold && &p == cold)) {
            minSolidArea = std::min(minSolidArea, p.area);
            maxSolidArea = std::max(maxSolidArea, p.area);
        }
    }
    check(fullSized == 6,
          "6 个焊点的外接矩形都是 80x80（实际 " + std::to_string(fullSized) +
              " 个）");
    check(worstOffset < 8.0,
          "完整焊点的质心离矩形中心不到 8 像素（实测 " +
              std::to_string((int)(worstOffset * 10) / 10.0) + "）");

    // ⚠ 完整焊点的面积**不是一个数**。
    //
    // 五个一模一样的 80x80 焊点，开运算之后的面积从 3699 到 5289 ——
    // 相差 43%。而**开运算之前**的 mask 覆盖率是均匀的
    // （实测 5524~5580 / 6400）。
    // ⇒ 差异是**开运算这一步**造成的：mask 里那 13% 的洞被腐蚀放大，
    //    膨胀只能补回一部分，补多少取决于洞**怎么分布**。
    //
    // ⇒ 所以这里断的是**一个带**，不是一个数 ——
    //    把面积卡死在一个实测值上，就成了"只有这台机器成立"的断言。
    check(minSolidArea > 3000 && maxSolidArea < 6000,
          "完整焊点的面积都在 3000~6000 之间（实测 " +
              std::to_string((int)minSolidArea) + "~" +
              std::to_string((int)maxSolidArea) + "）");
}

// ------------------------------------------------------------
// 6. 全流程
// ------------------------------------------------------------
static void test_full_pipeline() {
    beginCase("整条链跑一遍");

    const cv::Mat img = w3::generatePcbDefectImage();
    const cv::Mat mask = w3::goldMask(img);
    const cv::Mat opened = w3::openClean(mask);
    const cv::Mat edges = w3::cannyEdges(opened);
    const std::vector<w3::Pad> pads = w3::findPads(opened);
    const cv::Mat marked = w3::drawPads(img, pads);

    bool allSame = true;
    for (const cv::Mat* m : {&mask, &opened, &edges})
        if (m->size() != img.size()) allSame = false;
    check(allSame, "每一环的输出尺寸都和原图一样");
    check(marked.size() == img.size() && marked.channels() == 3,
          "标注图的尺寸/通道不变");
    check(pads.size() == 6, "整条链跑完，找到 6 个焊点（实际 " +
                                std::to_string(pads.size()) + " 个）");

    // 跑两遍，结果必须一样 —— 生成器带种子，处理链没有隐藏状态
    const cv::Mat img2 = w3::generatePcbDefectImage();
    const std::vector<w3::Pad> pads2 =
        w3::findPads(w3::openClean(w3::goldMask(img2)));
    bool sameCount = pads.size() == pads2.size();
    bool sameAreas = sameCount;
    for (size_t i = 0; i < pads.size() && sameAreas; ++i) {
        if (std::abs(pads[i].area - pads2[i].area) > 1e-6) sameAreas = false;
    }
    check(sameCount && sameAreas, "跑两遍结果完全一样");

    // 标注：每个焊点的边框上都有 Canny 边缘
    int padsWithEdges = 0;
    for (const auto& p : pads) {
        if (edgePixelsAround(edges, p.bbox, 3) > 0) ++padsWithEdges;
    }
    check(padsWithEdges == static_cast<int>(pads.size()),
          "每个焊点的边框上都有 Canny 边缘（" + std::to_string(padsWithEdges) +
              "/" + std::to_string(pads.size()) + "）");
}

int main() {
    std::cout << "=== W3 形态学 + 边缘 + 轮廓 回归测试 ===" << std::endl;

    test_image_generation();
    test_gold_mask();
    test_morphology();
    test_canny();
    test_find_pads();
    test_full_pipeline();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
