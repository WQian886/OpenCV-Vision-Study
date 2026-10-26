// ============================================================
// W1（图像 I/O + 像素操作）的无头回归测试
//
// ============================================================
//
// ## 这个套件在补什么
//
// 2026-10-26 之前，W1–W5 **一条自动化测试都没有**。
// W1 的情况尤其说明问题：它整个项目只有一段活着的代码 ——
// 「练习 3：把蓝色像素涂黑」，而那段代码**一行测试都没有**。
// 一个只有 20 行的程序没人测，不是因为它不重要，
// 是因为它写在了 main() 里 —— 写测试的人得先把它拆出来。
//
// 拆出来的样子见 pixel_ops.h：判据一个函数、涂黑一个函数、造图一个函数。
// main.cpp 和这个套件调的是**同一份**。
//
// ## 这套测试守住的是什么
//
// 1. **BGR 不是 RGB** —— 判据 px[0] > 100（蓝）写成 px[2] > 100（红），
//    白底 (255,255,255) 就会整片被判成"蓝" → 整张图涂黑，
//    而画面上看不出任何异常。判据单独断它。
// 2. **边界是闭的** —— B 正好 100 不算蓝，G/R 正好 150 不算蓝。
//    「> 100」和「< 150」里的等号写反了，肉眼几乎看不出来。
// 3. **涂黑之后不能再涂一次** —— 黑色 (0,0,0) 不满足判据，
//    所以这个操作是幂等的。这条同时证明判据没有"越涂越多"。
// 4. **imwrite → imread 往返必须一模一样** —— PNG 是无损的，
//    读回来逐像素相同；这条断的是"存了再读"这条链本身。
//
// ## 产物落在哪
//
// 往**当前工作目录**写一个 PNG 再读回来（ctest 的工作目录在 build/ 里），
// 读完删掉。源码树一个字节都不动 ——
// CI 的"源码树必须干净"那一步对这套测试不是空转。
//
// ============================================================

#include "pixel_ops.h"

#include <opencv2/opencv.hpp>

#include <iostream>
#include <string>

// ============================================================
// 极简断言（与 W2 / W6 / W7 各套件同一套风格）
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

static bool isBlack(const cv::Vec3b& px) {
    return px[0] == 0 && px[1] == 0 && px[2] == 0;
}

static bool isWhite(const cv::Vec3b& px) {
    return px[0] == 255 && px[1] == 255 && px[2] == 255;
}

// ------------------------------------------------------------
// 1. 造出来的图
// ------------------------------------------------------------
static void test_image() {
    beginCase("练习 3 的测试图");

    const cv::Mat img = w1::makeEx3Image();
    check(img.cols == 400 && img.rows == 300,
          "尺寸是 400x300（实际 " + std::to_string(img.cols) + "x" +
              std::to_string(img.rows) + "）");
    check(img.type() == CV_8UC3, "类型是 CV_8UC3");

    const cv::Vec3b white = img.at<cv::Vec3b>(20, 350);
    check(isWhite(white), "右上角是白色 255,255,255（实际 " +
                              std::to_string(white[0]) + "," +
                              std::to_string(white[1]) + "," +
                              std::to_string(white[2]) + "）");

    const cv::Vec3b light = img.at<cv::Vec3b>(50, 50);
    check(light[0] == 200 && light[1] == 100 && light[2] == 100,
          "左半是浅蓝 200,100,100（实际 " + std::to_string(light[0]) + "," +
              std::to_string(light[1]) + "," + std::to_string(light[2]) + "）");

    const cv::Vec3b deep = img.at<cv::Vec3b>(150, 300);
    check(deep[0] == 220 && deep[1] == 80 && deep[2] == 80,
          "中间那条是深蓝 220,80,80（实际 " + std::to_string(deep[0]) + "," +
              std::to_string(deep[1]) + "," + std::to_string(deep[2]) + "）");
}

// ------------------------------------------------------------
// 2. 「什么算蓝」的判据
// ------------------------------------------------------------
static void test_predicate() {
    beginCase("什么算蓝");

    check(!w1::isBluePixel(cv::Vec3b(255, 255, 255)),
          "白色不算蓝（B 高但 G/R 也高）");
    check(w1::isBluePixel(cv::Vec3b(200, 100, 100)), "浅蓝算蓝");
    check(w1::isBluePixel(cv::Vec3b(220, 80, 80)), "深蓝算蓝");
    check(!w1::isBluePixel(cv::Vec3b(0, 255, 0)),
          "纯绿不算蓝（B 低）");
    check(!w1::isBluePixel(cv::Vec3b(255, 0, 255)),
          "紫色不算蓝（R 高）");

    // ⚠ 纯红**按这个判据算蓝** —— 这是练习 3 判据的已知粗糙处，不是 bug。
    //
    // 判据是「B 通道 > 100 且 G/R 通道 < 150」，
    // 而纯红 (255,0,0) 的 B 通道**也是 255**，G=R=0 也满足 "< 150"
    // —— 于是纯红被当成了蓝。
    //
    // 练习 3 的测试图里没有红色像素，所以图上的结果不受影响。
    // 这里如实记下**当前真实行为**（和仓库里别处一样：
    // 断言记的是现状，哪天判据重做了，这条会立刻转红，
    // 提醒补一条「必须不算蓝」的断言）。
    check(w1::isBluePixel(cv::Vec3b(255, 0, 0)),
          "纯红按这个判据算蓝（B=255 也高）—— 练习 3 判据的已知粗糙处");

    // 边界：等号在哪一边
    check(!w1::isBluePixel(cv::Vec3b(100, 100, 100)),
          "B 正好 100 不算蓝（判据是「大于」100，不是「大于等于」）");
    check(w1::isBluePixel(cv::Vec3b(101, 149, 149)),
          "B=101 且 G/R=149 算蓝（刚好越过边界）");
    check(!w1::isBluePixel(cv::Vec3b(200, 150, 100)),
          "G 正好 150 不算蓝（判据是「小于」150）");
    check(!w1::isBluePixel(cv::Vec3b(200, 100, 150)),
          "R 正好 150 不算蓝");
}

// ------------------------------------------------------------
// 3. 涂黑
// ------------------------------------------------------------
static void test_paint() {
    beginCase("把蓝色像素涂黑");

    cv::Mat img = w1::makeEx3Image();
    const int painted = w1::paintBlueToBlack(img);

    // 浅蓝 200x300 = 60000，深蓝带 200x100 = 20000
    check(painted == 80000,
          "涂了 80000 个像素（实际 " + std::to_string(painted) + "）");

    int blacks = 0, whites = 0, others = 0;
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x) {
            const cv::Vec3b px = img.at<cv::Vec3b>(y, x);
            if (isBlack(px)) ++blacks;
            else if (isWhite(px)) ++whites;
            else ++others;
        }
    check(blacks == 80000, "图里有 80000 个纯黑像素（实际 " +
                               std::to_string(blacks) + "）");
    check(whites == 40000, "白区一个都没动，还剩 40000 个纯白像素（实际 " +
                               std::to_string(whites) + "）");
    check(others == 0, "没有半涂不涂的中间状态（实际 " +
                           std::to_string(others) + " 个）");

    // 浅蓝区和深蓝带真的全黑了
    bool lightAllBlack = true, deepAllBlack = true, whiteUntouched = true;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            const cv::Vec3b px = img.at<cv::Vec3b>(y, x);
            const bool inLight = (x < 200);
            const bool inDeep = (x >= 200 && y >= 100 && y < 200);
            const bool inWhite = (!inLight && !inDeep);
            if (inLight && !isBlack(px)) lightAllBlack = false;
            if (inDeep && !isBlack(px)) deepAllBlack = false;
            if (inWhite && !isWhite(px)) whiteUntouched = false;
        }
    }
    check(lightAllBlack, "左半浅蓝区全部变黑");
    check(deepAllBlack, "中间深蓝带全部变黑");
    check(whiteUntouched, "白区一个像素都没变");

    // 幂等：黑色不满足判据，再涂一次应该是 0
    const int again = w1::paintBlueToBlack(img);
    check(again == 0,
          "再涂一次是 0 个（黑色不算蓝，这一步是幂等的）（实际 " +
              std::to_string(again) + "）");
}

// ------------------------------------------------------------
// 4. 存盘 / 读盘
// ------------------------------------------------------------
static void test_io() {
    beginCase("imwrite / imread");

    cv::Mat img = w1::makeEx3Image();
    w1::paintBlueToBlack(img);

    const std::string path = "w1_roundtrip.png";
    const bool written = cv::imwrite(path, img);
    check(written, "imwrite 写成功了");

    const cv::Mat back = cv::imread(path, cv::IMREAD_COLOR);
    check(!back.empty(), "imread 读回来了");
    check(back.size() == img.size() && back.type() == img.type(),
          "读回来的尺寸/类型和写出去的一样");

    bool identical = true;
    if (back.size() == img.size()) {
        for (int y = 0; y < img.rows && identical; ++y)
            for (int x = 0; x < img.cols; ++x)
                if (img.at<cv::Vec3b>(y, x) != back.at<cv::Vec3b>(y, x)) {
                    identical = false;
                    break;
                }
    } else {
        identical = false;
    }
    check(identical, "PNG 无损：读回来的每一个像素都和写出去的一样");

    const cv::Mat gray = cv::imread(path, cv::IMREAD_GRAYSCALE);
    check(gray.channels() == 1 && gray.size() == img.size(),
          "用 IMREAD_GRAYSCALE 读是 1 通道，尺寸不变");

    const cv::Mat missing = cv::imread("w1_no_such_file_for_test.png", cv::IMREAD_COLOR);
    check(missing.empty(), "读一个不存在的文件得到空 Mat（不是崩溃）");

    // 产物删掉：工作目录在 build/ 里，但不留垃圾
    std::remove(path.c_str());
}

int main() {
    std::cout << "=== W1 图像 I/O + 像素操作 回归测试 ===" << std::endl;

    test_image();
    test_predicate();
    test_paint();
    test_io();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
