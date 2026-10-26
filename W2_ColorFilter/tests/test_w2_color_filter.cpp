// ============================================================
// W2（色彩空间 + 滤波 + 阈值）的无头回归测试
//
// ============================================================
//
// ## 这个套件在补什么
//
// 2026-10-26 之前，W1–W5 **一条自动化测试都没有**。
// 原因不是"没时间"，是**写不了**：处理链全在 main() 里，
// 外面挂着 imshow / waitKey（要显示器）和 10 个 imwrite（要写盘）。
// 无头环境里这两样都做不了，于是这一层就永远只能靠人眼看。
//
// 现在把纯逻辑抽到 color_pipeline.{h,cpp}（main.cpp 和测试调的是同一份），
// 这个套件就是那一层的回归网。
//
// ## ⚠ 这里的期望值都是**量出来的**，不是拍脑袋定的
//
// 2026-10-26 用一份一次性探针（build/probe/，不入库）实测：
//
//   · 焊点 BGR(60,180,220) 在 OpenCV 8bit HSV 里色相是 **H=23**
//     —— OpenCV 的公式是 (g-b)*30/delta，比教科书少一半。
//     按教科书的算法去凑那个 HSV 窗口，会凑出一个**选不中焊点**的窗口。
//   · demo 那个窗口（H=10..30）罩住焊点 **87.0%** 的像素，最差的一块 86.5%；
//     背景误选只有 **0.60%**。
//   · 平坦区标准差：原始 23.78 → 高斯 6.56 / 中值 6.22 / 双边 6.25。
//   · 阶跃边缘的过渡带宽度：原始 0 列 → 高斯 **4** 列，双边 **0** 列。
//   · 椒盐噪声：中值滤波后**全部像素**回到背景值 40，高斯是 [31,94]。
//   · 光照不均的斜坡图：暗区在固定阈值@127 下前景 **0** 个，自适应 **20000** 个。
//
// 断言的门槛都比实测值**留了余量**（噪声的实现细节可能随 OpenCV 版本变，
// 门槛卡死在一个实测值上，就成了"只有这台机器成立"的断言 ——
// 那正是本仓库在 CI 第一次真跑时栽过的坑）。
//
// ## 这套测试**不**做什么
//
// 不写任何文件、不开任何窗口 —— 产物一个字节都不落地，
// 所以 CI 的"源码树必须干净"那一步对它是空转（这本身也是对的：
// 没有产物，就没有产物泄漏）。
//
// ============================================================

#include "color_pipeline.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
// 极简断言（与 W6 / W7 各套件同一套风格）
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

// 一个区域里某个通道的中位数（避开均值被离群点带跑）
static int medianOf(const cv::Mat& m, const cv::Rect& r, int channel) {
    std::vector<uchar> v;
    v.reserve(static_cast<size_t>(r.area()));
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            v.push_back(m.at<cv::Vec3b>(y, x)[channel]);
    std::sort(v.begin(), v.end());
    return v.empty() ? -1 : v[v.size() / 2];
}

static double stddevOf(const cv::Mat& m, const cv::Rect& r) {
    cv::Scalar mu, sd;
    cv::meanStdDev(m(r), mu, sd);
    return sd[0];
}

static double meanOf(const cv::Mat& m, const cv::Rect& r) {
    return cv::mean(m(r))[0];
}

// 背景取样区：避开 8 个焊点，也避开中央那行文字
static const std::vector<cv::Rect>& backgroundSamples() {
    static const std::vector<cv::Rect> kSamples = {
        cv::Rect(10, 10, 30, 30), cv::Rect(300, 30, 40, 40),
        cv::Rect(600, 10, 30, 30), cv::Rect(10, 430, 40, 30),
        cv::Rect(290, 440, 40, 30), cv::Rect(600, 430, 30, 30),
    };
    return kSamples;
}

// ------------------------------------------------------------
// 1. 生成图
// ------------------------------------------------------------
static void test_image_generation() {
    beginCase("生成模拟 PCB 图");

    const cv::Mat img = w2::generatePcbLikeImage();
    check(img.cols == 640 && img.rows == 480,
          "尺寸是 640x480（实际 " + std::to_string(img.cols) + "x" +
              std::to_string(img.rows) + "）");
    check(img.type() == CV_8UC3, "类型是 CV_8UC3（8 位无符号 3 通道）");

    // 焊点：8 个，4 列 x 2 行，每个 80x80
    const std::vector<cv::Rect>& pads = w2::padRects();
    check(pads.size() == 8,
          "有 8 个焊点（实际 " + std::to_string(pads.size()) + "）");

    bool allSameSize = true, allInside = true, noOverlap = true;
    std::vector<int> xs, ys;
    for (const auto& p : pads) {
        if (p.width != 80 || p.height != 80) allSameSize = false;
        if (p.x < 0 || p.y < 0 || p.x + p.width > img.cols ||
            p.y + p.height > img.rows)
            allInside = false;
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    for (size_t i = 0; i < pads.size(); ++i)
        for (size_t j = i + 1; j < pads.size(); ++j)
            if ((pads[i] & pads[j]).area() > 0) noOverlap = false;
    check(allSameSize, "每个焊点都是 80x80");
    check(allInside, "每个焊点都在图内");
    check(noOverlap, "焊点两两不重叠");

    // 排成 4 列 x 2 行：去重后的 x 有 4 个、y 有 2 个，且间距一致。
    //
    // ⚠ 这里断的是**结构**（网格、等距），不是把坐标抄一遍 ——
    // 抄坐标就是「抄副本」：改了布局，测试还在按旧坐标断言。
    std::vector<int> ux = xs, uy = ys;
    std::sort(ux.begin(), ux.end());
    ux.erase(std::unique(ux.begin(), ux.end()), ux.end());
    std::sort(uy.begin(), uy.end());
    uy.erase(std::unique(uy.begin(), uy.end()), uy.end());
    check(ux.size() == 4 && uy.size() == 2,
          "焊点排成 4 列 x 2 行（实际 " + std::to_string(ux.size()) + " 列 " +
              std::to_string(uy.size()) + " 行）");
    bool evenCols = true, evenRows = true;
    for (size_t i = 1; i < ux.size(); ++i)
        if (ux[i] - ux[i - 1] != ux[1] - ux[0]) evenCols = false;
    for (size_t i = 1; i < uy.size(); ++i)
        if (uy[i] - uy[i - 1] != uy[1] - uy[0]) evenRows = false;
    check(evenCols, "列间距一致");
    check(evenRows, "行间距一致");

    // 焊点的颜色：金黄。噪声 σ=25，中位数离真值很近（容差 40）
    const cv::Scalar& pad = w2::padColor();
    bool nearPad = true;
    for (const auto& p : pads) {
        const cv::Rect c(p.x + 30, p.y + 30, 20, 20);  // 中心 20x20，避开边缘
        for (int ch = 0; ch < 3; ++ch) {
            const int med = medianOf(img, c, ch);
            if (std::abs(med - static_cast<int>(pad[ch])) > 40) nearPad = false;
        }
    }
    check(nearPad, "焊点像素是金黄色（BGR 接近 " +
                       std::to_string((int)pad[0]) + "," +
                       std::to_string((int)pad[1]) + "," +
                       std::to_string((int)pad[2]) + "）");

    // 背景是深绿
    const cv::Scalar& bg = w2::backgroundColor();
    bool nearBg = true;
    for (const auto& r : backgroundSamples()) {
        for (int ch = 0; ch < 3; ++ch) {
            const int med = medianOf(img, r, ch);
            if (std::abs(med - static_cast<int>(bg[ch])) > 40) nearBg = false;
        }
    }
    check(nearBg, "背景像素是深绿色（BGR 接近 " +
                       std::to_string((int)bg[0]) + "," +
                       std::to_string((int)bg[1]) + "," +
                       std::to_string((int)bg[2]) + "）");

    // 同一个 seed → 同一张图（测试自己的基线必须是固定的）
    const cv::Mat again = w2::generatePcbLikeImage();
    check(cv::norm(img, again, cv::NORM_INF) == 0,
          "同一个 seed 生成的是同一张图（逐像素相同）");

    const cv::Mat other = w2::generatePcbLikeImage(12345);
    check(cv::norm(img, other, cv::NORM_INF) > 0,
          "不同 seed 生成的是不同的图（噪声确实带种子）");
}

// ------------------------------------------------------------
// 2. 色彩空间
// ------------------------------------------------------------
static void test_color_space() {
    beginCase("色彩空间转换");

    const cv::Mat img = w2::generatePcbLikeImage();
    const cv::Mat hsv = w2::toHsv(img);
    const cv::Mat gray = w2::toGray(img);

    check(hsv.channels() == 3, "HSV 是 3 通道");
    check(hsv.size() == img.size(), "HSV 尺寸不变");
    check(gray.channels() == 1, "灰度是 1 通道");
    check(gray.size() == img.size(), "灰度尺寸不变");

    // 灰度公式：0.299R + 0.587G + 0.114B（干净像素，±2）
    const cv::Mat clean(1, 1, CV_8UC3, cv::Scalar(60, 180, 220));
    const int gv = w2::toGray(clean).at<uchar>(0, 0);
    const double expect = 0.299 * 220 + 0.587 * 180 + 0.114 * 60;
    check(std::abs(gv - expect) <= 2,
          "灰度值符合 0.299R+0.587G+0.114B（实际 " + std::to_string(gv) +
              "，公式 " + std::to_string((int)expect) + "）");

    // 焊点的色相落在金色窗口里 —— 这条是「窗口罩得住焊点」的判据
    const cv::Rect c(80, 130, 20, 20);
    cv::Mat hC[3];
    cv::split(hsv, hC);
    std::vector<uchar> hs;
    for (int y = c.y; y < c.y + c.height; ++y)
        for (int x = c.x; x < c.x + c.width; ++x)
            hs.push_back(hC[0].at<uchar>(y, x));
    std::sort(hs.begin(), hs.end());
    const int hMedian = hs[hs.size() / 2];
    const cv::Scalar& lo = w2::goldHsvLower();
    const cv::Scalar& hi = w2::goldHsvUpper();
    check(hMedian >= lo[0] && hMedian <= hi[0],
          "焊点的色相落在金色窗口 H=[" + std::to_string((int)lo[0]) + ".." +
              std::to_string((int)hi[0]) + "] 里（实测中位数 H=" +
              std::to_string(hMedian) + "）");

    // 背景的色相不在金色窗口里（绿 vs 黄橙）
    const cv::Rect b(10, 10, 30, 30);
    std::vector<uchar> hb;
    for (int y = b.y; y < b.y + b.height; ++y)
        for (int x = b.x; x < b.x + b.width; ++x)
            hb.push_back(hC[0].at<uchar>(y, x));
    std::sort(hb.begin(), hb.end());
    const int hBg = hb[hb.size() / 2];
    check(hBg < lo[0] || hBg > hi[0],
          "背景的色相不在金色窗口里（实测中位数 H=" + std::to_string(hBg) + "）");
}

// ------------------------------------------------------------
// 3. 颜色提取
// ------------------------------------------------------------
static void test_color_extraction() {
    beginCase("inRange 提取金色");

    const cv::Mat img = w2::generatePcbLikeImage();
    const cv::Mat mask = w2::goldMask(img);

    check(mask.type() == CV_8UC1, "mask 是单通道 8 位");

    bool onlyZeroOr255 = true;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            const uchar v = mask.at<uchar>(y, x);
            if (v != 0 && v != 255) onlyZeroOr255 = false;
        }
    check(onlyZeroOr255, "mask 的取值只有 0 和 255");

    // 每个焊点：绝大多数像素要被选中（实测最差的一块 86.5%，门槛留到 75%）
    double worst = 1.0;
    for (const auto& p : w2::padRects()) {
        const double frac = static_cast<double>(cv::countNonZero(mask(p))) / p.area();
        worst = std::min(worst, frac);
    }
    check(worst >= 0.75,
          "每个焊点至少 75% 的像素被选中（实测最差 " +
              std::to_string((int)(worst * 100)) + "%）");

    // 背景：几乎不被选中（实测 0.60%，门槛放到 2%）
    int bgHit = 0, bgArea = 0;
    for (const auto& r : backgroundSamples()) {
        bgHit += cv::countNonZero(mask(r));
        bgArea += r.area();
    }
    const double bgFrac = static_cast<double>(bgHit) / bgArea;
    check(bgFrac <= 0.02,
          "背景误选不超过 2%（实测 " +
              std::to_string((int)(bgFrac * 10000) / 100.0) + "%）");

    // goldOnly：mask 外全黑，mask 内与原图一致
    const cv::Mat only = w2::goldOnly(img);
    check(only.size() == img.size() && only.channels() == 3,
          "goldOnly 尺寸/通道不变");
    bool outsideZero = true, insideSame = true;
    for (int y = 0; y < img.rows; ++y) {
        for (int x = 0; x < img.cols; ++x) {
            const cv::Vec3b src = img.at<cv::Vec3b>(y, x);
            const cv::Vec3b dst = only.at<cv::Vec3b>(y, x);
            if (mask.at<uchar>(y, x) == 0) {
                if (dst[0] != 0 || dst[1] != 0 || dst[2] != 0) outsideZero = false;
            } else {
                if (src != dst) insideSame = false;
            }
        }
    }
    check(outsideZero, "mask 之外全黑");
    check(insideSame, "mask 之内和原图逐像素相同");
}

// ------------------------------------------------------------
// 4. 滤波
// ------------------------------------------------------------
static void test_filters() {
    beginCase("三种滤波");

    const cv::Mat img = w2::generatePcbLikeImage();
    const cv::Mat g = w2::gaussianBlur(img);
    const cv::Mat m = w2::medianBlur(img);
    const cv::Mat b = w2::bilateralDenoise(img);

    check(g.size() == img.size() && g.type() == img.type(), "高斯：尺寸/类型不变");
    check(m.size() == img.size() && m.type() == img.type(), "中值：尺寸/类型不变");
    check(b.size() == img.size() && b.type() == img.type(), "双边：尺寸/类型不变");

    // 平坦区的噪声被压下去（实测 23.78 -> 6.2~6.6，门槛取原始的 1/3）
    const cv::Rect flat(10, 10, 30, 30);
    const double sd0 = stddevOf(img, flat);
    const double sdG = stddevOf(g, flat);
    const double sdM = stddevOf(m, flat);
    const double sdB = stddevOf(b, flat);
    check(sdG < sd0 / 3.0, "高斯把平坦区的标准差压到 1/3 以下（实测 " +
                               std::to_string((int)sd0) + " -> " +
                               std::to_string((int)sdG) + "）");
    check(sdM < sd0 / 3.0, "中值把平坦区的标准差压到 1/3 以下（实测 " +
                               std::to_string((int)sd0) + " -> " +
                               std::to_string((int)sdM) + "）");
    check(sdB < sd0 / 3.0, "双边把平坦区的标准差压到 1/3 以下（实测 " +
                               std::to_string((int)sd0) + " -> " +
                               std::to_string((int)sdB) + "）");

    // 平坦区的均值基本不动（滤波不是"改颜色"）
    const double mu0 = meanOf(img, flat);
    check(std::abs(meanOf(g, flat) - mu0) < 3.0, "高斯不改变平坦区的均值");
    check(std::abs(meanOf(m, flat) - mu0) < 3.0, "中值不改变平坦区的均值");
    check(std::abs(meanOf(b, flat) - mu0) < 3.0, "双边不改变平坦区的均值");

    // 阶跃边缘：高斯糊开，双边保住（实测过渡带 高斯 4 列 / 双边 0 列）
    //
    // ⚠ 这里**必须调 w2:: 的函数**，不能直接调 cv::GaussianBlur / cv::bilateralFilter。
    //
    // 2026-10-26 的变异验证抓到了这一条：把 w2::bilateralDenoise 的实现
    // 换成高斯（一个手滑就能犯的错），**这个断言还是绿的** ——
    // 因为它量的是 OpenCV 的双边滤波，不是我们的双边滤波。
    //
    // 断言量的是 OpenCV 的行为，测的是"OpenCV 好不好"；
    // 断言量的是 w2::bilateralDenoise 的输出，测的才是"我们的实现对不对"。
    // 前者永远绿着 —— 它**验证不等于验证**。
    cv::Mat step(200, 200, CV_8UC1, cv::Scalar(40));
    step(cv::Rect(100, 0, 100, 200)).setTo(cv::Scalar(200));
    const cv::Mat sg = w2::gaussianBlur(step, 9);
    const cv::Mat sb = w2::bilateralDenoise(step, 9, 75, 75);
    auto bandWidth = [](const cv::Mat& m) {
        int n = 0;
        for (int x = 0; x < m.cols; ++x) {
            const int v = m.at<uchar>(100, x);
            if (v > 60 && v < 180) ++n;
        }
        return n;
    };
    const int bandG = bandWidth(sg);
    const int bandB = bandWidth(sb);
    check(bandG >= 2, "高斯把阶跃边缘糊开（过渡带 " + std::to_string(bandG) + " 列）");
    check(bandB <= 1, "双边保住阶跃边缘（过渡带 " + std::to_string(bandB) + " 列）");

    // 椒盐噪声：中值完全还原，高斯抹不开（实测中值 [40,40]，高斯 [31,94]）
    // 同样走 w2::，不走 OpenCV —— 理由见上面阶跃边缘那条的注释。
    cv::Mat sp(100, 100, CV_8UC1, cv::Scalar(40));
    cv::theRNG() = cv::RNG(7);
    for (int i = 0; i < 200; ++i) {
        const int x = cv::theRNG().uniform(0, 100);
        const int y = cv::theRNG().uniform(0, 100);
        sp.at<uchar>(y, x) = ((i % 2) ? 255 : 0);
    }
    const cv::Mat spg = w2::gaussianBlur(sp, 5);
    const cv::Mat spm = w2::medianBlur(sp, 5);
    int mMin = 255, mMax = 0, gMin = 255, gMax = 0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) {
            const int a = spg.at<uchar>(y, x);
            const int c = spm.at<uchar>(y, x);
            gMin = std::min(gMin, a); gMax = std::max(gMax, a);
            mMin = std::min(mMin, c); mMax = std::max(mMax, c);
        }
    check(mMin == 40 && mMax == 40,
          "中值把椒盐噪声完全还原成背景值 40（实测 [" +
              std::to_string(mMin) + "," + std::to_string(mMax) + "]）");
    check(gMax > 60 || gMin < 20,
          "高斯抹不开椒盐噪声（实测 [" + std::to_string(gMin) + "," +
              std::to_string(gMax) + "]，背景被带偏了）");
}

// ------------------------------------------------------------
// 5. 阈值
// ------------------------------------------------------------
static void test_thresholds() {
    beginCase("三种阈值");

    const cv::Mat img = w2::generatePcbLikeImage();
    const cv::Mat gray = w2::toGray(img);

    // 固定阈值：逐点核对规则本身
    const cv::Mat fixed = w2::thresholdFixed(gray, 127);
    bool binary = true, ruleOk = true;
    for (int y = 0; y < gray.rows; ++y)
        for (int x = 0; x < gray.cols; ++x) {
            const int src = gray.at<uchar>(y, x);
            const int dst = fixed.at<uchar>(y, x);
            if (dst != 0 && dst != 255) binary = false;
            const int want = (src > 127) ? 255 : 0;
            if (dst != want) ruleOk = false;
        }
    check(binary, "固定阈值的输出只有 0 和 255");
    check(ruleOk, "固定阈值遵守「>127 记前景，否则记背景」");

    // 自适应阈值：输出二值
    const cv::Mat adapt = w2::thresholdAdaptive(gray);
    bool adaptBinary = true;
    for (int y = 0; y < adapt.rows; ++y)
        for (int x = 0; x < adapt.cols; ++x) {
            const int v = adapt.at<uchar>(y, x);
            if (v != 0 && v != 255) adaptBinary = false;
        }
    check(adaptBinary, "自适应阈值的输出只有 0 和 255");

    // Otsu：双峰图上阈值落在两峰之间，前景正好是亮的那一半
    {
        cv::Mat bi(100, 100, CV_8UC1, cv::Scalar(30));
        bi(cv::Rect(50, 0, 50, 100)).setTo(cv::Scalar(200));
        double t = -1;
        const cv::Mat out = w2::thresholdOtsu(bi, &t);
        // ⚠ 这里量到的是 **30**，不是"两峰中间" —— 但**不是 bug**。
        //
        // OpenCV 的 Otsu 在**并列最大**的阈值里取**最小**的那个。
        // 这张图只有两个灰度值 30 和 200，类间方差在 t ∈ [30, 199]
        // 的**每一个** t 上都等于最大值，所以并列了一整个区间，
        // OpenCV 取了区间的左端点 30。
        //
        // 教科书说"Otsu 落在两峰之间"没有说错，但那句话隐含了
        // "双峰图上阈值唯一"—— 而一张只有两个取值、没有任何噪声的图
        // 上，**分离得开**这件事和阈值取哪个端点**无关**。
        //
        // 所以这里断的是**区间**（在 [30,199] 里 = 把两堆分开了），
        // 真正证明"分对了"的是下一条：前景正好 5000 个 = 亮的那一半。
        check(t >= 30 && t < 200,
              "Otsu 在双峰图上的阈值落在两峰之间（实测 " +
                  std::to_string((int)t) +
                  "，并列时取最小的那个）");
        check(cv::countNonZero(out) == 5000,
              "Otsu 把亮的那一半整个认成前景（实测 " +
                  std::to_string(cv::countNonZero(out)) + " 个）");
    }

    // Otsu：纯色图上阈值是 0（没有可分的峰）
    {
        const cv::Mat flatImg(50, 50, CV_8UC1, cv::Scalar(77));
        double t = -1;
        w2::thresholdOtsu(flatImg, &t);
        check(t == 0, "Otsu 在纯色图上的阈值是 0（实测 " +
                         std::to_string((int)t) + "）");
    }

    // Otsu：演示图上，焊点当前景、背景当背景
    {
        double t = -1;
        const cv::Mat out = w2::thresholdOtsu(gray, &t);
        check(t > 60 && t < 200,
              "Otsu 在演示图上的阈值落在背景峰和焊点峰之间（实测 " +
                  std::to_string((int)t) + "）");
        double worst = 1.0;
        for (const auto& p : w2::padRects()) {
            const double frac = static_cast<double>(cv::countNonZero(out(p))) / p.area();
            worst = std::min(worst, frac);
        }
        check(worst >= 0.75, "Otsu 把焊点认成前景（实测最差 " +
                                 std::to_string((int)(worst * 100)) + "%）");
        int bgHit = 0, bgArea = 0;
        for (const auto& r : backgroundSamples()) {
            bgHit += cv::countNonZero(out(r));
            bgArea += r.area();
        }
        check(static_cast<double>(bgHit) / bgArea <= 0.05,
              "Otsu 把背景认成背景（实测误判 " +
                  std::to_string((int)(100.0 * bgHit / bgArea)) + "%）");
    }

    // 光照不均：固定阈值在暗区全军覆没，自适应留得住
    {
        cv::Mat uneven(200, 400, CV_8UC1);
        for (int x = 0; x < 400; ++x)
            for (int y = 0; y < 200; ++y)
                uneven.at<uchar>(y, x) = static_cast<uchar>(30 + x * 0.4);
        const cv::Rect darkSide(0, 0, 100, 200);
        const cv::Mat fx = w2::thresholdFixed(uneven, 127);
        const cv::Mat ad = w2::thresholdAdaptive(uneven);
        const int fxDark = cv::countNonZero(fx(darkSide));
        const int adDark = cv::countNonZero(ad(darkSide));
        check(fxDark == 0,
              "固定阈值@127 在暗区一个像素都留不住（实测 " +
                  std::to_string(fxDark) + " 个）");
        check(adDark > fxDark * 10 + 1000,
              "自适应阈值在暗区留得住（实测 " + std::to_string(adDark) +
                  " 个 vs 固定阈值的 " + std::to_string(fxDark) + " 个）");
    }
}

// ------------------------------------------------------------
// 6. 全流程
// ------------------------------------------------------------
static void test_full_pipeline() {
    beginCase("整条链跑一遍");

    const cv::Mat img = w2::generatePcbLikeImage();
    const cv::Mat hsv = w2::toHsv(img);
    const cv::Mat mask = w2::goldMask(img);
    const cv::Mat only = w2::goldOnly(img);
    const cv::Mat g = w2::gaussianBlur(img);
    const cv::Mat m = w2::medianBlur(img);
    const cv::Mat b = w2::bilateralDenoise(img);
    const cv::Mat gray = w2::toGray(img);
    const cv::Mat t1 = w2::thresholdFixed(gray);
    const cv::Mat t2 = w2::thresholdAdaptive(gray);
    double otsuT = -1;
    const cv::Mat t3 = w2::thresholdOtsu(gray, &otsuT);

    bool allSame = true;
    for (const cv::Mat* mm : {&hsv, &mask, &only, &g, &m, &b, &gray, &t1, &t2, &t3})
        if (mm->size() != img.size()) allSame = false;
    check(allSame, "每一环的输出尺寸都和原图一样");
    check(otsuT >= 0, "Otsu 把阈值带了出来（" + std::to_string((int)otsuT) + "）");
    check(cv::countNonZero(mask) > 0, "整条链跑完，提到了东西");
}

int main() {
    std::cout << "=== W2 色彩空间 + 滤波 + 阈值 回归测试 ===" << std::endl;

    test_image_generation();
    test_color_space();
    test_color_extraction();
    test_filters();
    test_thresholds();
    test_full_pipeline();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
