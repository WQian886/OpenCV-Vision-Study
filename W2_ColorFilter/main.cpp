// ============================================================
// W2: 色彩空间 + 滤波 + 阈值
// 学习目标:
//   1. cv::cvtColor 色彩空间转换 (BGR ↔ HSV ↔ Gray)
//   2. cv::inRange 颜色范围提取（HSV 空间）
//   3. cv::GaussianBlur / medianBlur / bilateralFilter 三种滤波
//   4. cv::threshold / adaptiveThreshold 二值化
//   5. Otsu 自动阈值
//
// ============================================================
//
// ⚠ 处理链**不在这个文件里** —— 它在 color_pipeline.{h,cpp}。
//
// 原因：这一层原本写在 main() 里，外面挂着 imshow 和 10 个 imwrite，
// 于是它**一条测试都写不了**（无头环境没有显示器，也不能往源码树写东西）。
// 2026-10-26 把纯逻辑抽出来之后，main.cpp 和
// tests/test_w2_color_filter.cpp 调的是**同一份**实现。
//
// 抽出来之后，本文件只负责三件事：
//   1. 读图（没有就生成）
//   2. 把每一环的**结果**打印出来（这是给人看的部分）
//   3. 保存 + 显示（这是要 GUI 和磁盘的部分）
//
// 一条都不许在这里重写一遍 —— 重写就是「抄副本」，
// 而抄出来的副本会随核心漂移，漂了没人知道。
//
// ============================================================

#include "color_pipeline.h"

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================
// 把 3 张图横向拼成 1 张,方便对比展示
// ============================================================
static cv::Mat hconcat3(const cv::Mat& a, const cv::Mat& b, const cv::Mat& c) {
    cv::Mat out;
    cv::hconcat(a, b, out);
    cv::hconcat(out, c, out);
    return out;
}

// ============================================================
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ============ 1. 读图(自动生成 PCB 测试图) ============
    const std::string imgPath = "images/pcb_test.jpg";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cout << "[提示] " << imgPath << " 不存在,自动生成 PCB 测试图\n";
        img = w2::generatePcbLikeImage();
        cv::imwrite(imgPath, img);
        std::cout << "[提示] 已保存到 " << imgPath << "\n\n";
    }
    std::cout << "===== 图像信息 =====\n";
    std::cout << "  尺寸:    " << img.cols << " x " << img.rows << "\n";
    std::cout << "  通道数:  " << img.channels() << " (BGR)\n\n";

    // ============ 2. 色彩空间转换 BGR → HSV ============
    // HSV: H(色相 0-179)/ S(饱和度 0-255)/ V(明度 0-255)
    // 电气工程师视角:像把示波器从 XY 切到极坐标模式
    const cv::Mat hsv = w2::toHsv(img);

    cv::Mat hsvChannels[3];
    cv::split(hsv, hsvChannels);
    std::cout << "===== HSV 通道拆分 =====\n";
    std::cout << "  H 通道(色相):  均值 = " << std::fixed << std::setprecision(1)
              << cv::mean(hsvChannels[0])[0] << "\n";
    std::cout << "  S 通道(饱和度):均值 = " << cv::mean(hsvChannels[1])[0] << "\n";
    std::cout << "  V 通道(明度):  均值 = " << cv::mean(hsvChannels[2])[0] << "\n\n";

    // ============ 3. 颜色提取(金色焊点) ============
    // 金色 HSV 范围在 color_pipeline.cpp 里(只有那一份):
    // H=10-30(黄到橙),S=100-255(够饱和),V=100-255(够亮)
    const cv::Mat goldMask = w2::goldMask(img);
    const cv::Mat goldOnly = w2::goldOnly(img);

    const int goldPixels = cv::countNonZero(goldMask);
    std::cout << "===== 颜色提取(金色焊点) =====\n";
    std::cout << "  检测到 " << goldPixels << " 个金色像素\n";
    std::cout << "  占总像素 " << std::fixed << std::setprecision(2)
              << (100.0 * goldPixels / (img.cols * img.rows)) << "%\n\n";

    // ============ 4. 滤波对比(去噪) ============
    const cv::Mat blurG = w2::gaussianBlur(img);
    const cv::Mat blurM = w2::medianBlur(img);
    const cv::Mat blurB = w2::bilateralDenoise(img);

    // 量化对比:滤波后图像与原图的"差异度"
    auto diffMean = [&](const cv::Mat& filtered) {
        cv::Mat d;
        cv::absdiff(img, filtered, d);
        return cv::mean(d)[0];
    };
    std::cout << "===== 滤波对比(均值差异,越小越接近原图) =====\n";
    std::cout << "  [高斯]  " << diffMean(blurG) << "\n";
    std::cout << "  [中值]  " << diffMean(blurM) << "\n";
    std::cout << "  [双边]  " << diffMean(blurB) << "  (越大说明保边越强)\n\n";

    // ============ 5. 灰度化 + 阈值对比 ============
    const cv::Mat gray = w2::toGray(img);

    const cv::Mat threshBinary = w2::thresholdFixed(gray);
    const cv::Mat threshAdaptive = w2::thresholdAdaptive(gray);
    double otsuThresh = 0;
    const cv::Mat threshOtsu = w2::thresholdOtsu(gray, &otsuThresh);

    std::cout << "===== 阈值对比(前景像素数) =====\n";
    std::cout << "  [全局@127]   " << cv::countNonZero(threshBinary) << "\n";
    std::cout << "  [自适应]     " << cv::countNonZero(threshAdaptive) << "\n";
    std::cout << "  [Otsu 自动]  " << cv::countNonZero(threshOtsu)
              << "  (Otsu 选的阈值 = " << otsuThresh << ")\n\n";

    // ============ 6. 保存所有输出 ============
    cv::imwrite("images/w2_hsv.png", hsv);
    cv::imwrite("images/w2_gold_mask.png", goldMask);
    cv::imwrite("images/w2_gold_only.png", goldOnly);
    cv::imwrite("images/w2_gray.png", gray);
    cv::imwrite("images/w2_blur_gauss.png", blurG);
    cv::imwrite("images/w2_blur_median.png", blurM);
    cv::imwrite("images/w2_blur_bilateral.png", blurB);
    cv::imwrite("images/w2_thresh_binary.png", threshBinary);
    cv::imwrite("images/w2_thresh_adaptive.png", threshAdaptive);
    cv::imwrite("images/w2_thresh_otsu.png", threshOtsu);
    std::cout << "===== 保存 =====\n";
    std::cout << "  已保存 images/w2_*.png 共 10 个结果文件\n\n";

    // ============ 7. 显示 3 个对比窗口(避免窗口太多) ============
    cv::Mat colorDemo, blurDemo, threshDemo;
    // 颜色提取对比:原图 | 金色 mask | 金色区域
    // 先把 1 通道的 mask 转成 3 通道,才能跟 img / goldOnly 横向拼接
    cv::Mat colorMask3ch;
    cv::cvtColor(goldMask, colorMask3ch, cv::COLOR_GRAY2BGR);
    cv::hconcat(img, colorMask3ch, colorDemo);
    cv::hconcat(colorDemo, goldOnly, colorDemo);

    // 滤波对比:高斯 | 中值 | 双边
    blurDemo = hconcat3(blurG, blurM, blurB);

    // 阈值对比:全局 | 自适应 | Otsu
    cv::Mat bin3ch, adp3ch, otsu3ch;
    cv::cvtColor(threshBinary, bin3ch, cv::COLOR_GRAY2BGR);
    cv::cvtColor(threshAdaptive, adp3ch, cv::COLOR_GRAY2BGR);
    cv::cvtColor(threshOtsu, otsu3ch, cv::COLOR_GRAY2BGR);
    threshDemo = hconcat3(bin3ch, adp3ch, otsu3ch);

    cv::namedWindow("1. 颜色提取:原图 | mask | 金色区域", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("2. 滤波对比:高斯 | 中值 | 双边", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("3. 阈值对比:全局 | 自适应 | Otsu", cv::WINDOW_AUTOSIZE);
    cv::imshow("1. 颜色提取:原图 | mask | 金色区域", colorDemo);
    cv::imshow("2. 滤波对比:高斯 | 中值 | 双边", blurDemo);
    cv::imshow("3. 阈值对比:全局 | 自适应 | Otsu", threshDemo);

    std::cout << "===== 显示 =====\n";
    std::cout << "  窗口1: 颜色提取对比\n";
    std::cout << "  窗口2: 滤波对比\n";
    std::cout << "  窗口3: 阈值对比\n";
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();

    std::cout << "\n✅ W2 完成!\n";
    std::cout << "下一节: W3 边缘 + 轮廓 + 形态学\n";
    return 0;
}
