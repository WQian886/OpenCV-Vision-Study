////// ============================================================
////// W1: 图像 I/O + 像素操作
////// 学习目标:
//////   1. cv::imread 读图
//////   2. cv::Mat 基本属性 (cols/rows/channels/depth/type)
//////   3. 像素读写 (at<>() / setTo)
//////   4. cv::imwrite 保存
//////   5. cv::imshow 显示
////// ============================================================
////
////#include <opencv2/opencv.hpp>
////#include <iostream>
////#include <string>
////
////// Windows 终端默认 GBK 编码,源码是 UTF-8 → 中文乱码
////// 解决:启动时把终端输出 codepage 改成 UTF-8
////#ifdef _WIN32
////#include <windows.h>
////#endif
////
////// 在图片上画一些几何形状 + 文字,作为测试图
////static cv::Mat generateTestImage() {
////    const int W = 640, H = 480;
////    cv::Mat img(H, W, CV_8UC3, cv::Scalar(30, 30, 60));   // 深蓝背景
////
////    // 画彩色色带（HSV 风格渐变）
////    for (int x = 0; x < W; ++x) {
////        cv::line(img, cv::Point(x, 0), cv::Point(x, 60),
////                 cv::Scalar(x % 180, 200, 220), 1);
////    }
////    cv::putText(img, "OpenCV 4.13 Test Image",
////                cv::Point(20, 110), cv::FONT_HERSHEY_SIMPLEX,
////                0.8, cv::Scalar(255, 255, 255), 2);
////
////    // 画几个圆
////    cv::circle(img, cv::Point(150, 250), 60, cv::Scalar(0, 255, 0), 3);    // 绿
////    cv::circle(img, cv::Point(320, 250), 60, cv::Scalar(0, 255, 255), 3);  // 黄
////    cv::circle(img, cv::Point(490, 250), 60, cv::Scalar(0, 0, 255), 3);    // 红
////
////    // 画一个矩形
////    cv::rectangle(img, cv::Point(100, 350), cv::Point(540, 420),
////                  cv::Scalar(255, 255, 255), 2);
////    cv::putText(img, "W1 ImageIO Demo",
////                cv::Point(180, 395), cv::FONT_HERSHEY_SIMPLEX,
////                0.7, cv::Scalar(255, 255, 255), 2);
////
////    return img;
////}
////
////int main() {
////#ifdef _WIN32
////    // Windows: 把终端输出 codepage 改成 UTF-8,解决中文乱码
////    SetConsoleOutputCP(CP_UTF8);
////#endif
////
////    // ---------- 1. 读图 ----------
////    const std::string imgPath = "images/test.jpg";
////    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
////
////    // 如果没有测试图，自动生成一张（方便你零准备上手）
////    if (img.empty()) {
////        std::cout << "[提示] images/test.jpg 不存在，自动生成一张测试图\n";
////        img = generateTestImage();
////        cv::imwrite(imgPath, img);
////        std::cout << "[提示] 已保存到 " << imgPath << "\n\n";
////    }
////
////    // ---------- 2. 打印图像信息 ----------
////    // 电气工程师视角:这就像读示波器的通道数 / 采样率
////    std::cout << "===== 图像信息 =====\n";
////    std::cout << "  尺寸:    " << img.cols << " x " << img.rows << " 像素\n";
////    std::cout << "  通道数:  " << img.channels()
////              << "  (1=灰度, 3=BGR彩色, 4=BGRA带透明)\n";
////    std::cout << "  位深:    " << img.depth()
////              << "  (0=CV_8U, 1=CV_8S, 2=CV_16U, 5=CV_32F...)\n";
////    std::cout << "  类型:    " << img.type()
////              << "  (CV_8UC3 = 8位无符号3通道)\n";
////    std::cout << "  总字节:  " << img.total() * img.elemSize() << "\n";
////    std::cout << "  数据指针: " << (void*)img.data << "\n\n";
////
////    // ---------- 3. 像素操作:左上角涂红 100x100 ----------
////    // OpenCV 默认 BGR (注意不是 RGB!),这是工业圈最常见的踩坑点
////
////    //测试1改变已有的方块的尺寸
////    cv::Rect roi(0, 0, 100, 100);
////    cv::Mat block = img(roi);          // ROI = Region Of Interest,不复制数据
////    block.setTo(cv::Scalar(0, 0, 255)); // BGR: 蓝绿=0, 红=255
////
////    // 验证:用 at<Vec3b> 读像素 (Vec3b = 3 个 unsigned char)
////    cv::Vec3b px = img.at<cv::Vec3b>(10, 10);
////
////
////    std::cout << "===== 像素操作 =====\n";
////    std::cout << "  左上 (10,10) 像素: B=" << (int)px[0]
////              << " G=" << (int)px[1]
////              << " R=" << (int)px[2] << "  (应该 R=255)\n";
////    std::cout << "  原图 (200,300) 像素: B=" << (int)img.at<cv::Vec3b>(300, 200)[0]
////              << " G=" << (int)img.at<cv::Vec3b>(300, 200)[1]
////              << " R=" << (int)img.at<cv::Vec3b>(300, 200)[2] << "\n\n";
////
////    /////测试题2画格子
////    const int N = 10;
////    const int cellW = img.cols / N;   // 640/8 = 80
////    const int cellH = img.rows / N;   // 480/8 = 60
////    const cv::Scalar gridColor(0, 255, 255);  // 黄色 (BGR)
////    for (int i = 1; i < N; ++i) {
////        // 竖线
////        cv::line(img,
////            cv::Point(i * cellW, 0),
////            cv::Point(i * cellW, img.rows),
////            gridColor, 1);
////        // 横线
////        cv::line(img,
////            cv::Point(0, i * cellH),
////            cv::Point(img.cols, i * cellH),
////            gridColor, 1);
////    }
////    std::cout << "[练习2] 已画 8×8 黄色网格\n";
////
////
////
////
////
////
////
////    // ---------- 4. 保存修改后的图 ----------
////    cv::imwrite("images/output_w1.png", img);
////    std::cout << "===== 保存 =====\n";
////    std::cout << "  已保存 images/output_w1.png\n\n";
////
////    // ---------- 5. 显示图像 ----------
////    cv::namedWindow("W1 Hello OpenCV", cv::WINDOW_AUTOSIZE);
////    cv::imshow("W1 Hello OpenCV", img);
////    std::cout << "按任意键关闭窗口...\n";
////    cv::waitKey(0);
////    cv::destroyAllWindows();
////
////    std::cout << "\n✅ W1 完成!\n";
////    std::cout << "下一节: W2 色彩空间 (BGR/HSV/Gray) + 滤波 + 阈值\n";
////
////    return 0;
////}
// exercises/ex3_blue2black.cpp
//
// ⚠ 判据和遍历**不在这个文件里** —— 它们在 pixel_ops.{h,cpp}。
// main.cpp 和 tests/test_w1_imageio.cpp 调的是**同一份**实现。
// 原因见 pixel_ops.h 的开头（2026-10-26 补测试时抽出来的）。
#include "pixel_ops.h"
#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    // 生成测试图:左半浅蓝,右半浅红,中间一条蓝带
    cv::Mat out = w1::makeEx3Image();

    // 🔥 核心:遍历 + at<Vec3b>(y, x) + 「什么算蓝」的判据
    const int blackCount = w1::paintBlueToBlack(out);

    cv::imwrite("images/ex3_blue2black.png", out);
    cv::imshow("Ex3: Blue > 100 -> Black", out);
    std::cout << "✅ Ex3 完成,涂黑像素 = " << blackCount << "\n";
    std::cout << "   左半浅蓝区 + 中间蓝带应全部变黑,白色区域保留\n";
    cv::waitKey(0);
    return 0;
}
