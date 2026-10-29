// ============================================================
// W4（模板匹配 + ORB 特征点）的无头回归测试
// ============================================================
//
// ## 这个套件在补什么
//
// 2026-10-29 之前，W4 **一条自动化测试都没有** ——
// 和 W5 一起，是欠账总账里"W3–W5 零测试缺口"那一条的最后一笔。
//
// 原因不是"没时间"，是**写不了**：整条链写在 main() 里，
// 外面挂着 imshow / waitKey（要显示器）和 3 个 imwrite（要写盘）。
// 无头环境里这两样都做不了，于是这一层就永远只能靠人眼看。
//
// 现在把纯逻辑抽到 feature_pipeline.{h,cpp}（main.cpp 和测试调的是同一份），
// 这个套件就是那一层的回归网。
//
// ## ⚠ 这里的期望值都是**量出来的**，不是拍脑袋定的
//
// 2026-10-29 一次跑完 57 条断言，把该量的都量了（数字见每条断言后面）：
//
//   · 相似度图 561x401（640-80+1 × 480-80+1）
//   · 从大图上抠的模板：CCORR_NORMED 打 1.000000，**就在它被抠下来的那一点**
//   · 独立的金块模板 + CCORR_NORMED：0.985996，落在 8 个焊点之一上
//   · **同一个模板 + CCOEFF_NORMED：只有 0.030943，位置跑到背景上去了**
//   · 8 个焊点的分数 0.985409 ~ 0.985996，**散得只有 0.000587**
//   · 背景最好的地方 0.982913
//   · 朴素数像素：阈值 0.90 → 12376 个；NMS 之后 → **8 个**
//   · 阈值从 0.90 到 0.98，NMS 之后**都是 8 个**；0.99 → 0 个
//   · NMS 半径 40~100 都是 8 个；半径 10 → **104 个**
//   · 纯色模板（方差 0）+ CCOEFF_NORMED：**每一处都是 1.0**，最好的一处是 (0,0)
//   · ORB 关键点：nfeatures 50/100/200/500 各找出这么多；1000 → 960（图上就这么多）
//   · ORB 模板高 60 → **0 个**关键点；高 80 → 162 个
//   · 文字模板：好匹配 127 对，单应性把四角还原到 **0.77 像素**误差
//   · **拿金块当 ORB 模板：好匹配 0 对**（8 个一模一样的焊点把 ratio test 全毙了）
//   · 模板转 15/30/45 度：好匹配 34 / 26 / 19 对（ORB 抗旋转）
//
// 门槛都比实测值**留了余量**（噪声的实现细节可能随 OpenCV 版本变，
// 门槛卡死在一个实测值上，就成了"只有这台机器成立"的断言 ——
// 那正是本仓库在 CI 第一次真跑时栽过的坑）。
//
// ## ⚠ 断的是**行为**，不是 OpenCV 的行为
//
// 断言全部走 w4:: 的入口，不直接调 cv::matchTemplate 去断它的行为。
// W2 那轮栽过一次：断言写的是 OpenCV 的行为，
// 于是「把 bilateralFilter 换成 Gaussian」这种改动测试**照样绿** ——
// 绿灯什么也没证明。
//
// 这里断的是**这一层自己的约定**：method 决定「像」是什么意思、
// 极性取对没有、最佳匹配是不是真的落在目标上、
// 多目标数是不是**门槛的产物**、ORB 模板够不够大、旋转了还认不认得出。
//
// ## 这套测试**不**做什么
//
// 不写任何文件、不开任何窗口 —— 产物一个字节都不落地，
// 所以 CI 的"源码树必须干净"那一步对它是空转。
//
// ============================================================

#include "feature_pipeline.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
// 极简断言（与 W1 / W2 / W3 / W6 / W7 各套件同一套风格）
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

// 一个点落在哪个焊点里（落在角上也算），落在外面返回 -1
static int padIndexAt(const cv::Point& p) {
    const std::vector<cv::Rect>& pads = w4::padRects();
    for (size_t i = 0; i < pads.size(); ++i) {
        if (p.x >= pads[i].x && p.x <= pads[i].x + pads[i].width &&
            p.y >= pads[i].y && p.y <= pads[i].y + pads[i].height)
            return static_cast<int>(i);
    }
    return -1;
}

// 一块区域里的平均颜色
static cv::Scalar meanColor(const cv::Mat& bgr, const cv::Rect& r) {
    const cv::Rect clipped = r & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (clipped.width <= 0 || clipped.height <= 0) return cv::Scalar(0, 0, 0);
    cv::Scalar m = cv::mean(bgr(clipped));
    return m;
}

// 分数图里过线的像素数（**不做** NMS —— 用来演示"为什么需要 NMS"）
static int countAbove(const cv::Mat& scores, double threshold) {
    int n = 0;
    for (int y = 0; y < scores.rows; ++y)
        for (int x = 0; x < scores.cols; ++x)
            if (scores.at<float>(y, x) >= threshold) ++n;
    return n;
}

static double dist2(const cv::Point2f& a, const cv::Point2f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// ------------------------------------------------------------
// 1. 生成图
// ------------------------------------------------------------
static void test_image_generation() {
    beginCase("生成模拟 PCB 图");

    const cv::Mat img = w4::generatePcbImage();
    check(img.cols == 640 && img.rows == 480,
          "尺寸是 640x480（实际 " + std::to_string(img.cols) + "x" +
              std::to_string(img.rows) + "）");
    check(img.type() == CV_8UC3, "类型是 CV_8UC3（8 位无符号 3 通道）");

    // 8 个焊点，每个都在自己该在的位置上
    check(w4::padRects().size() == 8,
          "图上有 8 个焊点（实际 " + std::to_string(w4::padRects().size()) + " 个）");

    // 每个焊点那块确实是金色的
    //
    // ⚠ 这里断的是**颜色**，不是"数出来几个矩形" ——
    // 一张全是背景的图也能通过"数矩形"的检查。
    bool allGold = true;
    double worstDelta = 0.0;
    for (const auto& p : w4::padRects()) {
        const cv::Scalar m = meanColor(img, p);
        const cv::Scalar gold(60, 180, 220);
        for (int c = 0; c < 3; ++c) {
            const double d = std::abs(m[c] - gold[c]);
            worstDelta = std::max(worstDelta, d);
            if (d > 5.0) allGold = false;
        }
    }
    check(allGold,
          "每个焊点那块确实是金色的（与 BGR(60,180,220) 的最大偏差 " +
              std::to_string((int)(worstDelta * 10) / 10.0) + "）");

    // 背景是深绿的
    const cv::Scalar bg = meanColor(img, cv::Rect(10, 10, 40, 40));
    check(std::abs(bg[0] - 40) < 5 && std::abs(bg[1] - 80) < 5 &&
              std::abs(bg[2] - 40) < 5,
          "板面是深绿的 BGR(40,80,40)（实测 " +
              std::to_string((int)bg[0]) + "," + std::to_string((int)bg[1]) + "," +
              std::to_string((int)bg[2]) + "）");

    // 同一个 seed → 同一张图。
    //
    // ⚠ 这条在 2026-10-29 之前**根本写不出来**：
    // 老的生成器用 cv::randn 却没设种子，两次跑出来的是两张不同的图。
    const cv::Mat again = w4::generatePcbImage();
    check(cv::norm(img, again, cv::NORM_INF) == 0,
          "同一个 seed 生成的是同一张图（逐像素相同）");

    const cv::Mat other = w4::generatePcbImage(12345);
    check(cv::norm(img, other, cv::NORM_INF) > 0,
          "不同 seed 生成的是不同的图（噪声确实带种子）");
}

// ------------------------------------------------------------
// 2. 模板
// ------------------------------------------------------------
static void test_templates() {
    beginCase("两种模板");

    const cv::Mat img = w4::generatePcbImage();

    // 模板 A：从大图上抠下来的
    const cv::Rect r = w4::templateRect();
    const cv::Mat self = w4::cutTemplate(img, r);
    check(self.cols == 80 && self.rows == 80,
          "抠出来的模板是 80x80（实际 " + std::to_string(self.cols) + "x" +
              std::to_string(self.rows) + "）");
    check(cv::norm(self, img(r), cv::NORM_INF) == 0,
          "抠出来的模板和原图那块**逐像素相同**");

    // 模板 B：独立的金块
    const cv::Mat syn = w4::makeSyntheticTemplate();
    check(syn.cols == 80 && syn.rows == 80, "独立模板也是 80x80");
    check(cv::norm(syn, self, cv::NORM_INF) > 0,
          "独立模板和抠出来的模板**不是同一张**（这才是重点）");

    // 但它们的**平均颜色**一样 —— 形状/颜色相同，只有噪声不同
    const cv::Scalar m1 = meanColor(self, cv::Rect(0, 0, 80, 80));
    const cv::Scalar m2 = meanColor(syn, cv::Rect(0, 0, 80, 80));
    check(std::abs(m1[0] - m2[0]) < 5 && std::abs(m1[1] - m2[1]) < 5 &&
              std::abs(m1[2] - m2[2]) < 5,
          "两个模板的平均颜色一样（金色），差的是噪声");

    // 裁剪越界时要给出**空图**，不是越界访问
    const cv::Mat oob = w4::cutTemplate(img, cv::Rect(600, 450, 200, 200));
    check(oob.empty(), "裁剪超出画面时返回空图（不是越界访问）");
}

// ------------------------------------------------------------
// 3. 六种 method —— 「像」是什么意思，是 method 定的
// ------------------------------------------------------------
static void test_methods() {
    beginCase("六种 method 的差别");

    const cv::Mat img = w4::generatePcbImage();
    const cv::Mat syn = w4::makeSyntheticTemplate();

    // 极性：平方差类取最小，其余取最大
    check(!w4::higherIsBetter(cv::TM_SQDIFF), "TM_SQDIFF 是越小越像");
    check(!w4::higherIsBetter(cv::TM_SQDIFF_NORMED), "TM_SQDIFF_NORMED 是越小越像");
    check(w4::higherIsBetter(cv::TM_CCORR), "TM_CCORR 是越大越像");
    check(w4::higherIsBetter(cv::TM_CCORR_NORMED), "TM_CCORR_NORMED 是越大越像");
    check(w4::higherIsBetter(cv::TM_CCOEFF), "TM_CCOEFF 是越大越像");
    check(w4::higherIsBetter(cv::TM_CCOEFF_NORMED), "TM_CCOEFF_NORMED 是越大越像");

    // bestMatch 得按 method 取对极值。
    //
    // 实测（独立金块模板）：
    //   TM_SQDIFF        min=15307885 @(200,100)   max=290282720 @(496,17)
    //   TM_CCORR_NORMED  max=0.985996  @(200,100)   min=0.737936  @(146,218)
    const cv::Mat scSq = w4::scoreMap(img, syn, cv::TM_SQDIFF);
    const w4::Match sq = w4::bestMatch(scSq, cv::TM_SQDIFF);
    double sqMin = 0, sqMax = 0;
    cv::Point sqMinL, sqMaxL;
    cv::minMaxLoc(scSq, &sqMin, &sqMax, &sqMinL, &sqMaxL);
    check(sq.score == sqMin && sq.loc == sqMinL,
          "TM_SQDIFF 时 bestMatch 取的是**最小值**那一处（" +
              std::to_string((long long)sq.score) + " @(" +
              std::to_string(sq.loc.x) + "," + std::to_string(sq.loc.y) + "））");
    check(padIndexAt(sq.loc) >= 0,
          "取最小值时落在焊点上（不是背景里）");
    check(padIndexAt(sqMaxL) < 0,
          "取最大值会落到背景里去（实测 (" + std::to_string(sqMaxL.x) + "," +
              std::to_string(sqMaxL.y) + "））—— 所以极性取反了不会报错，只会悄悄给你一个错答案");

    // ⚠⚠ 这一段是这一轮最值钱的一条。
    //
    // W4 的 demo 一直用 TM_CCOEFF_NORMED（README 里也写着「工业首选」）。
    // 但那是对**有纹理**的模板而言的。
    //
    // 拿**纯色**的金块模板去试：
    //   TM_CCORR_NORMED   0.985996  @(200,100)  ← 落在焊点上，看得见目标
    //   TM_CCOEFF_NORMED  0.030943  @(386,373)  ← 跑到背景上去了，看不见目标
    //
    // 原因：CCOEFF 会**去掉均值**，于是只剩"噪声"可比 ——
    // 而模板的噪声和图上的噪声是**两回事**，相关性本来就该接近 0。
    // 形状一样（都是金块）这件事，在 CCOEFF 的眼里**根本不存在**。
    const cv::Mat scCorr = w4::scoreMap(img, syn, cv::TM_CCORR_NORMED);
    const w4::Match corr = w4::bestMatch(scCorr, cv::TM_CCORR_NORMED);
    const cv::Mat scCoeff = w4::scoreMap(img, syn, cv::TM_CCOEFF_NORMED);
    const w4::Match coeff = w4::bestMatch(scCoeff, cv::TM_CCOEFF_NORMED);

    check(corr.score >= 0.98,
          "CCORR_NORMED 看得见纯色模板（分数 " +
              std::to_string((int)(corr.score * 10000) / 10000.0) + "）");
    check(padIndexAt(corr.loc) >= 0,
          "CCORR_NORMED 的最佳匹配落在 8 个焊点之一上（(" +
              std::to_string(corr.loc.x) + "," + std::to_string(corr.loc.y) + "））");
    check(coeff.score <= 0.10,
          "CCOEFF_NORMED 对纯色模板**几乎是瞎的**（分数 " +
              std::to_string((int)(coeff.score * 10000) / 10000.0) + "）");
    check(corr.score - coeff.score > 0.5,
          "同一个模板，两种 method 的分数差了 " +
              std::to_string((int)((corr.score - coeff.score) * 100) / 100.0) +
              " —— 「像」是什么意思，是 method 定的");

    // 8 个焊点的分数几乎一样 —— 分不出谁是谁
    double lo = 1e9, hi = -1e9;
    for (const auto& p : w4::padRects()) {
        const double v = scCorr.at<float>(p.y, p.x);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    check(hi - lo < 0.001,
          "8 个焊点的分数散得只有 " +
              std::to_string((int)((hi - lo) * 100000) / 100.0 / 1000.0) +
              "（" + std::to_string((int)(lo * 10000) / 10000.0) + "~" +
              std::to_string((int)(hi * 10000) / 10000.0) +
              "）—— 这张图上「最相似的那一个」是**分不出来的**");

    // 但它们仍然比背景好
    double bgBest = -1e9;
    for (int y = 0; y < scCorr.rows; ++y) {
        for (int x = 0; x < scCorr.cols; ++x) {
            bool inPad = false;
            for (const auto& p : w4::padRects())
                if (x >= p.x && x < p.x + p.width && y >= p.y && y < p.y + p.height)
                    inPad = true;
            if (!inPad) bgBest = std::max(bgBest, static_cast<double>(scCorr.at<float>(y, x)));
        }
    }
    check(lo > bgBest,
          "焊点的分数确实比背景高（焊点最低 " +
              std::to_string((int)(lo * 10000) / 10000.0) + " vs 背景最好 " +
              std::to_string((int)(bgBest * 10000) / 10000.0) + "）");
}

// ------------------------------------------------------------
// 4. 多目标匹配 + NMS
// ------------------------------------------------------------
static void test_multi_target() {
    beginCase("多目标匹配");

    const cv::Mat img = w4::generatePcbImage();
    const cv::Mat syn = w4::makeSyntheticTemplate();
    const cv::Mat sc = w4::scoreMap(img, syn, cv::TM_CCORR_NORMED);

    // 朴素做法：数一数分数超过阈值的像素
    //
    // 实测：阈值 0.90 时过了线的像素有 **12376 个** ——
    // 一个目标对应一大片区域，不是**一个点**。
    const int naive = countAbove(sc, 0.90);
    check(naive > 1000,
          "朴素数像素会数出上万个（实测 " + std::to_string(naive) +
              " 个）—— 目标是一片区域，不是一个点");

    // NMS 之后：8 个焊点
    const std::vector<w4::Match> ms = w4::findMatches(sc, 0.90, cv::TM_CCORR_NORMED, 80);
    check(ms.size() == 8,
          "NMS 之后是 8 个匹配（实测 " + std::to_string(ms.size()) + " 个）");

    // 每个匹配都落在一个焊点上
    int onPad = 0;
    for (const auto& m : ms)
        if (padIndexAt(m.loc) >= 0) ++onPad;
    check(onPad == static_cast<int>(ms.size()),
          "8 个匹配全都落在焊点上（实测 " + std::to_string(onPad) + "/" +
              std::to_string(ms.size()) + "）");

    // ⚠⚠ 这个数**不能只是**"8"。
    //
    // W3 那轮栽过一回：断言写的是「== 6」，而那个 6 是**面积门槛的产物** ——
    // 门槛一改，结论就翻。
    //
    // 这里同一个道理：多目标的个数天然是**阈值的产物**。
    // 所以断的是「**阈值在 0.90~0.98 这一段里动，个数都是 8**」。
    bool stableCount = true;
    std::string counts;
    for (double th : {0.90, 0.93, 0.95, 0.98}) {
        const size_t n = w4::findMatches(sc, th, cv::TM_CCORR_NORMED, 80).size();
        counts += std::to_string((int)(th * 100)) + "%→" + std::to_string((int)n) + " ";
        if (n != ms.size()) stableCount = false;
    }
    check(stableCount,
          "阈值从 0.90 到 0.98，匹配的个数都是 8（实测 " + counts + "）");

    // 上界：0.99 之上就没有了（8 个焊点的最高分是 0.985996）
    const std::vector<w4::Match> tooStrict = w4::findMatches(sc, 0.99, cv::TM_CCORR_NORMED, 80);
    check(tooStrict.empty(),
          "阈值提到 0.99 一个都不剩（实测 " + std::to_string(tooStrict.size()) +
              " 个）—— 8 这个数成立的范围是 0.90~0.98");

    // ⚠ 半径**也是**个旋钮。
    //
    // 实测：半径 40~100 都是 8 个；半径缩到 10，一个目标被拆成十几个（104 个）。
    bool stableRadius = true;
    for (int r : {40, 60, 80, 100}) {
        if (w4::findMatches(sc, 0.90, cv::TM_CCORR_NORMED, r).size() != 8)
            stableRadius = false;
    }
    check(stableRadius, "NMS 半径从 40 到 100，匹配的个数都是 8");
    check(w4::findMatches(sc, 0.90, cv::TM_CCORR_NORMED, 10).size() > 20,
          "半径缩到 10，一个目标被拆成很多个（实测 " +
              std::to_string(w4::findMatches(sc, 0.90, cv::TM_CCORR_NORMED, 10).size()) +
              " 个）");
}

// ------------------------------------------------------------
// 5. 退化情形：纯色模板 / 极性
// ------------------------------------------------------------
static void test_degenerate() {
    beginCase("退化情形");

    const cv::Mat img = w4::generatePcbImage();

    // 一个**纯色**模板（方差为 0）
    cv::Mat flat(80, 80, CV_8UC3, cv::Scalar(40, 80, 40));

    // CCOEFF_NORMED：分母是 0（模板没有变化可算），
    // OpenCV 直接把结果给成 1.0 —— 整张分数图**全是 1.0**。
    //
    // ⇒ argmax 落在 (0,0)：图的左上角，一个什么都不是的地方。
    //
    // 这不是 bug，是 CCOEFF 的**定义**在方差为 0 时的样子。
    // 写下来是为了防止以后有人把它当 bug 去"修"。
    const cv::Mat scFlat = w4::scoreMap(img, flat, cv::TM_CCOEFF_NORMED);
    bool allOne = true;
    for (int y = 0; y < scFlat.rows && allOne; ++y)
        for (int x = 0; x < scFlat.cols; ++x)
            if (scFlat.at<float>(y, x) != 1.0f) {
                allOne = false;
                break;
            }
    check(allOne, "纯色模板 + CCOEFF_NORMED：分数图**每一处都是 1.0**");

    const w4::Match flatBest = w4::bestMatch(scFlat, cv::TM_CCOEFF_NORMED);
    check(flatBest.loc == cv::Point(0, 0),
          "所以「最相似位置」是 (0,0) —— 图的左上角（实测 (" +
              std::to_string(flatBest.loc.x) + "," + std::to_string(flatBest.loc.y) +
              "））");

    // 换成不去均值的 method，纯色模板就**有分可打了**
    const cv::Mat scFlatCorr = w4::scoreMap(img, flat, cv::TM_CCORR_NORMED);
    const w4::Match flatCorr = w4::bestMatch(scFlatCorr, cv::TM_CCORR_NORMED);
    check(flatCorr.score > 0.90,
          "同一个纯色模板，CCORR_NORMED 给 " +
              std::to_string((int)(flatCorr.score * 10000) / 10000.0) +
              " 分（不是 1.0）—— 分数终于有区分度了");

    // 自己抠的模板：分数是 1.0，而且**就在它被抠下来的那一点**
    const cv::Mat self = w4::cutTemplate(img, w4::templateRect());
    const cv::Mat scSelf = w4::scoreMap(img, self, cv::TM_CCORR_NORMED);
    const w4::Match selfBest = w4::bestMatch(scSelf, cv::TM_CCORR_NORMED);
    check(selfBest.score > 0.9999,
          "从大图上抠的模板，分数是 1.0（实测 " +
              std::to_string((int)(selfBest.score * 10000) / 10000.0) +
              "）—— 这个分是**白送的**：它只说明「它和它自己一样」");
    check(selfBest.loc == w4::templateRect().tl(),
          "而且最佳位置就是它被抠下来的那一点（(" +
              std::to_string(selfBest.loc.x) + "," + std::to_string(selfBest.loc.y) +
              "））");
}

// ------------------------------------------------------------
// 6. ORB
// ------------------------------------------------------------
static void test_orb() {
    beginCase("ORB 特征点");

    const cv::Mat img = w4::generatePcbImage();

    // 关键点数量：**上限**是参数，不是承诺
    //
    // 实测：nfeatures 50/100/200/500 → 各找出这么多；
    //       1000 → 960（图上总共就这么多）。
    bool underLimit = true;
    int prev = 0;
    bool monotone = true;
    for (int nf : {50, 100, 200, 500, 1000}) {
        std::vector<cv::KeyPoint> kps;
        const cv::Mat d = w4::detectDescriptors(img, nf, &kps);
        if (static_cast<int>(kps.size()) > nf) underLimit = false;
        if (static_cast<int>(kps.size()) < prev) monotone = false;
        prev = static_cast<int>(kps.size());
        if (d.rows != static_cast<int>(kps.size()) || d.cols != 32)
            underLimit = false;
    }
    check(underLimit, "关键点个数不超过 nfeatures，描述子是 N x 32");
    check(monotone, "nfeatures 变大，关键点个数只增不减");

    // 「找到 N 个关键点」这个说法：**N 是参数的产物**。
    //
    // 所以断的是「**关键点的位置**不依赖 nfeatures」——
    // 用 100 找到的那些点，在 500 的那一遍里**也得在**（实测 100/100 命中，容差 1 像素）。
    std::vector<cv::KeyPoint> k100, k500;
    w4::detectDescriptors(img, 100, &k100);
    w4::detectDescriptors(img, 500, &k500);
    int hit = 0;
    for (const auto& a : k100) {
        for (const auto& b : k500) {
            if (std::abs(a.pt.x - b.pt.x) <= 1.0 && std::abs(a.pt.y - b.pt.y) <= 1.0) {
                ++hit;
                break;
            }
        }
    }
    check(hit >= 95,
          "用 nfeatures=100 找到的关键点，在 500 的那一遍里也都在（实测 " +
              std::to_string(hit) + "/" + std::to_string(k100.size()) +
              "，容差 1 像素）—— 位置是图的性质，不是参数的产物");

    // ⚠ 模板**太小** ⇒ ORB 直接给你 0 个关键点，而且**不报错**。
    //
    // 实测：同样宽 380 的裁剪，高 60 → 0 个；高 80 → 162 个。
    // 原因是 ORB 默认 edgeThreshold=19、patchSize=31 ——
    // 矮于这个尺寸，特征全被当边界扔了。
    const cv::Mat thin = w4::cutTemplate(img, cv::Rect(90, 210, 380, 60));
    const cv::Mat ok = w4::cutTemplate(img, w4::orbTemplateRect());
    std::vector<cv::KeyPoint> kThin, kOk;
    w4::detectDescriptors(thin, 500, &kThin);
    w4::detectDescriptors(ok, 500, &kOk);
    check(kThin.empty(),
          "模板只有 60 像素高时，ORB 一个关键点都找不到（实测 " +
              std::to_string(kThin.size()) + " 个）—— **不报错**，只是给你 0");
    check(kOk.size() >= 50,
          "模板高 80 就找得到一堆（实测 " + std::to_string(kOk.size()) + " 个）");

    // 拿**唯一**的文字当模板 → 匹配得上
    std::vector<cv::KeyPoint> kTpl, kImg;
    const cv::Mat dTpl = w4::detectDescriptors(ok, 500, &kTpl);
    const cv::Mat dImg = w4::detectDescriptors(img, 1000, &kImg);
    const std::vector<cv::DMatch> good = w4::matchDescriptors(dTpl, dImg, 0.75);
    check(good.size() >= 100,
          "文字模板能找到 100 对以上好匹配（实测 " +
              std::to_string(good.size()) + " 对）");

    // 单应性矩阵把模板的四个角**还原**回它在图上的位置
    const cv::Mat H = w4::estimateHomography(kTpl, kImg, good);
    check(!H.empty(), "好匹配够多，算得出单应性矩阵");
    if (!H.empty()) {
        const cv::Rect r = w4::orbTemplateRect();
        std::vector<cv::Point2f> corners = {
            cv::Point2f(0, 0), cv::Point2f(static_cast<float>(r.width), 0),
            cv::Point2f(static_cast<float>(r.width), static_cast<float>(r.height)),
            cv::Point2f(0, static_cast<float>(r.height))};
        std::vector<cv::Point2f> out;
        cv::perspectiveTransform(corners, out, H);
        const cv::Point2f e0(static_cast<float>(r.x), static_cast<float>(r.y));
        const cv::Point2f e1(static_cast<float>(r.x + r.width), static_cast<float>(r.y));
        const cv::Point2f e2(static_cast<float>(r.x + r.width),
                             static_cast<float>(r.y + r.height));
        const cv::Point2f e3(static_cast<float>(r.x), static_cast<float>(r.y + r.height));
        const double err = std::max(std::max(dist2(out[0], e0), dist2(out[1], e1)),
                                    std::max(dist2(out[2], e2), dist2(out[3], e3)));
        check(err < 3.0,
              "单应性把模板的四个角还原回原位（实测最大偏差 " +
                  std::to_string((int)(err * 100) / 100.0) + " 像素）");
    }

    // ⚠ 拿**金块**当 ORB 模板 ⇒ 好匹配一个都没有。
    //
    // 原因：8 个焊点长得**一模一样** —— 模板上的每个描述子，
    // 在图里都能找到 8 个一样好的候选 ⇒ 次好距离和最好距离差不多
    // ⇒ **Lowe ratio test 全毙掉**。
    //
    // 这不是 ORB 坏了，是这张图**分不清谁是谁**。
    const cv::Mat padTpl = w4::cutTemplate(img, w4::padRects()[0]);
    std::vector<cv::KeyPoint> kPad;
    const cv::Mat dPad = w4::detectDescriptors(padTpl, 500, &kPad);
    const std::vector<cv::DMatch> goodPad = w4::matchDescriptors(dPad, dImg, 0.75);
    check(goodPad.size() <= 5,
          "拿金块当 ORB 模板，好匹配几乎没有（实测 " +
              std::to_string(goodPad.size()) + " 对）—— 8 个一模一样的焊点把 ratio test 全毙了");
    check(good.size() > 10 * goodPad.size(),
          "而唯一的文字模板能找到 " + std::to_string(good.size()) +
              " 对 —— 模板**唯一**，才谈得上「匹配」");

    // ORB 抗旋转：转 30 度还是认得出来
    const cv::Mat rot = w4::rotateAroundCenter(ok, 30.0);
    std::vector<cv::KeyPoint> kRot;
    const cv::Mat dRot = w4::detectDescriptors(rot, 500, &kRot);
    const std::vector<cv::DMatch> goodRot = w4::matchDescriptors(dRot, dImg, 0.75);
    check(goodRot.size() >= 20,
          "模板转 30° 之后 ORB 照样认得出来（实测 " +
              std::to_string(goodRot.size()) + " 对好匹配）");

    // 不够 4 对就算不出单应性 —— 返回**空矩阵**，不给一个假的
    const cv::Mat H2 = w4::estimateHomography(kTpl, kImg, std::vector<cv::DMatch>());
    check(H2.empty(), "好匹配不足 4 对时返回空矩阵（不是给一个假的）");
}

// ------------------------------------------------------------
// 7. 全流程
// ------------------------------------------------------------
static void test_full_pipeline() {
    beginCase("整条链跑一遍");

    const cv::Mat img = w4::generatePcbImage();
    const cv::Mat tpl = w4::makeSyntheticTemplate();
    const cv::Mat scores = w4::scoreMap(img, tpl, cv::TM_CCORR_NORMED);
    const std::vector<w4::Match> ms = w4::findMatches(scores, 0.90, cv::TM_CCORR_NORMED, 80);
    const cv::Mat marked = w4::drawMatch(img, ms[0], tpl.size());

    check(scores.cols == img.cols - tpl.cols + 1 && scores.rows == img.rows - tpl.rows + 1,
          "分数图尺寸是 (图 - 模板 + 1)（实测 " + std::to_string(scores.cols) + "x" +
              std::to_string(scores.rows) + "）");
    check(scores.type() == CV_32FC1, "分数图是单通道 32 位浮点");
    check(marked.size() == img.size() && marked.channels() == 3,
          "标注图的尺寸/通道不变");

    // 标注：红框真的画在匹配位置上
    int redInBox = 0;
    const cv::Rect box(ms[0].loc.x, ms[0].loc.y, tpl.cols, tpl.rows);
    for (int y = box.y; y < box.y + box.height; ++y) {
        for (int x = box.x; x < box.x + box.width; ++x) {
            const cv::Vec3b& p = marked.at<cv::Vec3b>(y, x);
            if (p[2] > 150 && p[0] < 100 && p[1] < 100) ++redInBox;
        }
    }
    check(redInBox > 100,
          "红框画在匹配位置上了（框里 " + std::to_string(redInBox) + " 个红像素）");

    // 跑两遍，结果必须一样
    const cv::Mat img2 = w4::generatePcbImage();
    const cv::Mat scores2 = w4::scoreMap(img2, tpl, cv::TM_CCORR_NORMED);
    const std::vector<w4::Match> ms2 =
        w4::findMatches(scores2, 0.90, cv::TM_CCORR_NORMED, 80);
    bool same = ms.size() == ms2.size();
    for (size_t i = 0; same && i < ms.size(); ++i) {
        if (ms[i].loc != ms2[i].loc) same = false;
        if (std::abs(ms[i].score - ms2[i].score) > 1e-9) same = false;
    }
    check(same, "跑两遍结果完全一样");
}

int main() {
    std::cout << "=== W4 模板匹配 + ORB 回归测试 ===" << std::endl;

    test_image_generation();
    test_templates();
    test_methods();
    test_multi_target();
    test_degenerate();
    test_orb();
    test_full_pipeline();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
