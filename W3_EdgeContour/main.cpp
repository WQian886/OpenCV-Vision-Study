// ============================================================
// W3: 边缘 + 轮廓 + 形态学
// 学习目标:
//   1. cv::morphologyEx 开运算去小噪点
//   2. cv::Canny 边缘检测
//   3. cv::findContours 找闭合轮廓
//   4. cv::contourArea / boundingRect / moments 轮廓属性
//   5. cv::drawContours 可视化
//   6. 实战: 数清楚 PCB 上有几个焊点
// ============================================================
//
// ⚠ 这一层**已经不在这个文件里了**。
//
// 整条处理链在 edge_pipeline.{h,cpp}：
//   生成图 → 提金色 → 开运算 → Canny → findContours → 面积过滤 → 标注。
// 这个文件只剩两件事：**调它**，和**把结果给人看**（imwrite + imshow）。
//
// 为什么要抽：imshow/waitKey 要显示器，imwrite 要写盘 ——
// 两个都不是无头环境能做的，所以处理链留在 main() 里就**一条测试都写不了**。
// 抽出来之后，tests/test_w3_edge_contour.cpp 和这个文件调的是**同一份实现**，
// 绿灯才真的说明那套逻辑是好的。
//
// 同一份实现，两个调用方 —— 仓库里的既有做法（W2 的 color_pipeline、
// W6 的 w6_cli）。抄一份到测试里就是「抄副本」：改了这里，测试还在按旧值断言，
// 于是测试绿着而 demo 已经变了。
// ============================================================

#include "edge_pipeline.h"

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <iomanip>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ===== 1. 读图(没有就生成 PCB 缺陷测试图) =====
    const std::string imgPath = "images/pcb_test.jpg";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cout << "[提示] " << imgPath << " 不存在,自动生成 PCB 缺陷测试图\n";
        img = w3::generatePcbDefectImage();   // 缺陷版:1 缺失 + 1 偏移 + 1 虚焊
        cv::imwrite(imgPath, img);
    }
    std::cout << "===== 图像信息 =====\n";
    std::cout << "  尺寸:    " << img.cols << " x " << img.rows << "\n\n";

    // ===== 2. 转 HSV + 金色 mask(窗口和 W2 共用同一份) =====
    const cv::Mat goldMask = w3::goldMask(img);

    const int initialPixels = cv::countNonZero(goldMask);
    std::cout << "===== Step 1: 金色 mask (W2 共用同一份窗口) =====\n";
    std::cout << "  初始金色像素: " << initialPixels << "\n\n";

    // ===== 3. 形态学开运算 =====
    // OPEN = erode 先(去小噪点) → dilate 后(恢复形状)
    // 电气工程师类比:用 3×3 窗口过滤掉孤立的脉冲干扰
    const cv::Mat morphMask = w3::openClean(goldMask);

    const int afterOpenPixels = cv::countNonZero(morphMask);
    std::cout << "===== Step 2: 形态学开运算 (去小噪点) =====\n";
    std::cout << "  开运算后像素: " << afterOpenPixels
              << " (减少 " << (initialPixels - afterOpenPixels) << " 个噪点)\n\n";

    // ===== 4. Canny 边缘检测 =====
    // 双阈值:低阈值 50 跟高阈值 150,经典经验值
    const cv::Mat edges = w3::cannyEdges(morphMask);

    const int edgePixels = cv::countNonZero(edges);
    std::cout << "===== Step 3: Canny 边缘检测 =====\n";
    std::cout << "  边缘像素:    " << edgePixels << "\n\n";

    // ===== 5. findContours 找轮廓 =====
    // RETR_EXTERNAL: 只找最外层轮廓 (焊点是实心的,不需要找内部洞)
    // CHAIN_APPROX_SIMPLE: 压缩水平/垂直/对角线段,只保留关键点
    const int totalContours = w3::countContours(morphMask);
    std::cout << "===== Step 4: findContours =====\n";
    std::cout << "  找到轮廓总数: " << totalContours << " (未过滤,含噪点小轮廓)\n\n";

    // ===== 6. 按面积过滤 (核心:区分焊点 vs 噪点) =====
    // 焊点每个约 6400 像素,小于 minPadArea() 视为噪点
    const std::vector<w3::Pad> pads = w3::findPads(morphMask);
    std::cout << "===== Step 5: 按面积过滤 (面积 > " << w3::minPadArea() << ") =====\n";
    std::cout << "  过滤后焊点: " << pads.size() << "\n";
    std::cout << "  各焊点面积: ";
    for (size_t i = 0; i < pads.size(); ++i) {
        std::cout << std::fixed << std::setprecision(0) << pads[i].area;
        if (i < pads.size() - 1) std::cout << ", ";
    }
    std::cout << "\n";
    std::cout << "  各焊点质心: ";
    for (size_t i = 0; i < pads.size(); ++i) {
        std::cout << "(" << std::fixed << std::setprecision(1)
                  << pads[i].centroid.x << "," << pads[i].centroid.y << ")";
        if (i < pads.size() - 1) std::cout << " ";
    }
    std::cout << "\n\n";

    // ===== 7. 标注到原图 (画外接矩形 + 中心点 + 编号) =====
    const cv::Mat marked = w3::drawPads(img, pads);

    // ===== 8. 保存结果 =====
    cv::imwrite("images/w3_morph_mask.png", morphMask);
    cv::imwrite("images/w3_edges.png", edges);
    cv::imwrite("images/w3_marked.png", marked);
    std::cout << "===== 保存 =====\n";
    std::cout << "  images/w3_morph_mask.png (开运算后的 mask)\n";
    std::cout << "  images/w3_edges.png       (Canny 边缘)\n";
    std::cout << "  images/w3_marked.png      (红框 + 编号标注的 PCB)\n\n";

    // ===== 9. 显示 3 个窗口 =====
    cv::namedWindow("1. 形态学清理后的 mask", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("2. Canny 边缘", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("3. 检测结果 (红框 + 编号)", cv::WINDOW_AUTOSIZE);
    cv::imshow("1. 形态学清理后的 mask", morphMask);
    cv::imshow("2. Canny 边缘", edges);
    cv::imshow("3. 检测结果 (红框 + 编号)", marked);

    std::cout << "===== 显示 =====\n";
    std::cout << "  窗口1: 形态学清理后的 mask (灰度)\n";
    std::cout << "  窗口2: Canny 边缘 (白色细线)\n";
    std::cout << "  窗口3: 检测结果 (彩色,带红色边框 + 黄色中心点 + 白色编号)\n";
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();

    std::cout << "\n✅ W3 完成!\n";
    std::cout << "下一节: W4 模板匹配 + ORB 特征点 (识别具体是什么元件)\n";
    return 0;
}
