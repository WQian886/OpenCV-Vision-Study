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

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <iomanip>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================
// 生成模拟 PCB 测试图(同 W2,8 个金色焊点) — ⏸ 已注释,保留作对照
// ============================================================
/*
static cv::Mat generatePcbLikeImage() {
    const int W = 640, H = 480;
    cv::Mat img(H, W, CV_8UC3, cv::Scalar(40, 80, 40));   //  深绿 PCB 底色 (BGR)

    // 8 个金色焊点(80×80 = 6400 像素/个)
    std::vector<cv::Rect> pads = {
        cv::Rect(50,  100, 80, 80), cv::Rect(200, 100, 80, 80),
        cv::Rect(350, 100, 80, 80), cv::Rect(500, 100, 80, 80),
        cv::Rect(50,  300, 80, 80), cv::Rect(200, 300, 80, 80),
        cv::Rect(350, 300, 80, 80), cv::Rect(500, 300, 80, 80),
    };
    for (const auto& p : pads) {
        cv::rectangle(img, p, cv::Scalar(60, 180, 220), -1);  //  金黄色填充
    }

    // 中央文字
    cv::putText(img, "W3 Edge Contour Test",
                cv::Point(140, 240), cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar(200, 200, 200), 2);

    // 加点噪声
    cv::Mat noise(img.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(25));
    cv::add(img, noise, img, cv::noArray(), CV_8UC3);
    return img;
}
*/

// ============================================================
// 生成"带缺陷"的 PCB 测试图(用于测试 W3 的鲁棒性)
// 8 个焊点里故意混 3 种缺陷:
//   - 缺失焊点: 位置 (500, 100) 不画
//   - 偏移焊点: 位置 (200, 100) 改为 (220, 130) 偏移
//   - 虚焊焊点: 位置 (50, 300) 内部挖掉 40% (左下角)
// ============================================================
static cv::Mat generatePcbDefectImage() {
    const int W = 640, H = 480;
    cv::Mat img(H, W, CV_8UC3, cv::Scalar(40, 80, 40));   //  深绿 PCB 底色 (BGR)

    // 5 个正常焊点(去掉 (500,100), 改 (200,100)→(220,130) 偏移, 改 (50,300) 虚焊)
    std::vector<cv::Rect> normalPads = {
        cv::Rect(50,  100, 80, 80),                        // 0: 正常
        cv::Rect(220, 130, 80, 80),                        // 1: ⚠️ 偏移(原 200,100)
        cv::Rect(350, 100, 80, 80),                        // 2: 正常
        cv::Rect(200, 300, 80, 80),                        // 3: 正常
        cv::Rect(350, 300, 80, 80),                        // 4: 正常
    };
    for (const auto& p : normalPads) {
        cv::rectangle(img, p, cv::Scalar(60, 180, 220), -1);  //  金黄色填充
    }

    // 偏移标记: 在原位置画红色空框 + 箭头 + "offset" 文字
    cv::Rect offsetOriginal(200, 100, 80, 80);
    cv::rectangle(img, offsetOriginal, cv::Scalar(0, 0, 255), 2);  // 红色空框(原位置)
    cv::line(img, cv::Point(200, 100), cv::Point(220, 130),
             cv::Scalar(0, 0, 255), 1);
    cv::putText(img, "offset", cv::Point(285, 110),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);

    // 虚焊焊点: 先画完整,再用底色挖掉 50x50 = 40% 缺失
    cv::Rect coldPad(50, 300, 80, 80);
    cv::rectangle(img, coldPad, cv::Scalar(60, 180, 220), -1);     // 先画完整金色
    cv::Rect missing(50, 300, 50, 50);                              // 挖掉 50x50
    cv::rectangle(img, missing, cv::Scalar(40, 80, 40), -1);       // 用底色盖
    cv::putText(img, "cold", cv::Point(60, 395),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);

    // 中央文字 + 缺陷说明
    cv::putText(img, "W3 Defect Test: 1 missing + 1 offset + 1 cold",
                cv::Point(60, 240), cv::FONT_HERSHEY_SIMPLEX,
                0.5, cv::Scalar(200, 200, 200), 1);

    // 加点噪声
    cv::Mat noise(img.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(25));
    cv::add(img, noise, img, cv::noArray(), CV_8UC3);
    return img;
}

// ============================================================
// 把 3 张图横向拼成 1 张,方便对比展示
// ============================================================
static cv::Mat hconcat3(const cv::Mat& a, const cv::Mat& b, const cv::Mat& c) {
    cv::Mat out;
    cv::hconcat(a, b, out);
    cv::hconcat(out, c, out);
    return out;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ===== 1. 读图(自动生成 PCB 测试图) =====
    const std::string imgPath = "images/pcb_test.jpg";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cout << "[提示] " << imgPath << " 不存在,自动生成 PCB 测试图\n";
        img = generatePcbDefectImage();   // ← 缺陷版(原 generatePcbLikeImage 已注释)
        cv::imwrite(imgPath, img);
    }
    std::cout << "===== 图像信息 =====\n";
    std::cout << "  尺寸:    " << img.cols << " x " << img.rows << "\n\n";

    // ===== 2. 转 HSV + 金色 mask(复用 W2 思路) =====
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::Mat goldMask;
    cv::inRange(hsv, cv::Scalar(10, 100, 100), cv::Scalar(30, 255, 255), goldMask);

    int initialPixels = cv::countNonZero(goldMask);
    std::cout << "===== Step 1: 金色 mask (W2 复用) =====\n";
    std::cout << "  初始金色像素: " << initialPixels << "\n\n";

    // ===== 3. 形态学开运算 =====
    // OPEN = erode 先(去小噪点) → dilate 后(恢复形状)
    // 电气工程师类比:用 3×3 窗口过滤掉孤立的脉冲干扰
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::Mat morphMask;
    cv::morphologyEx(goldMask, morphMask, cv::MORPH_OPEN, kernel);

    int afterOpenPixels = cv::countNonZero(morphMask);
    std::cout << "===== Step 2: 形态学开运算 (去小噪点) =====\n";
    std::cout << "  开运算后像素: " << afterOpenPixels
              << " (减少 " << (initialPixels - afterOpenPixels) << " 个噪点)\n\n";

    // ===== 4. Canny 边缘检测 =====
    // 双阈值:低阈值 50 跟高阈值 150,经典经验值
    cv::Mat edges;
    cv::Canny(morphMask, edges, 50, 150);
    int edgePixels = cv::countNonZero(edges);
    std::cout << "===== Step 3: Canny 边缘检测 =====\n";
    std::cout << "  边缘像素:    " << edgePixels << "\n\n";

    // ===== 5. findContours 找轮廓 =====
    // RETR_EXTERNAL: 只找最外层轮廓 (焊点是实心的,不需要找内部洞)
    // CHAIN_APPROX_SIMPLE: 压缩水平/垂直/对角线段,只保留关键点
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(morphMask, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::cout << "===== Step 4: findContours =====\n";
    std::cout << "  找到轮廓总数: " << contours.size() << " (未过滤,含噪点小轮廓)\n\n";

    // ===== 6. 按面积过滤 (核心:区分焊点 vs 噪点) =====
    // 8 个焊点每个约 6400 像素,小于 1000 视为噪点
    const double MIN_AREA = 1000.0;
    std::vector<std::vector<cv::Point>> validContours;
    std::vector<double> areas;
    for (const auto& c : contours) {
        double area = cv::contourArea(c);
        if (area > MIN_AREA) {
            validContours.push_back(c);
            areas.push_back(area);
        }
    }
    std::cout << "===== Step 5: 按面积过滤 (面积 > " << MIN_AREA << ") =====\n";
    std::cout << "  过滤后焊点: " << validContours.size() << "\n";
    std::cout << "  各焊点面积: ";
    for (size_t i = 0; i < areas.size(); ++i) {
        std::cout << std::fixed << std::setprecision(0) << areas[i];
        if (i < areas.size() - 1) std::cout << ", ";
    }
    std::cout << "\n\n";

    // ===== 7. 标注到原图 (画外接矩形 + 中心点 + 编号) =====
    cv::Mat marked = img.clone();
    for (size_t i = 0; i < validContours.size(); ++i) {
        // 外接矩形
        cv::Rect bbox = cv::boundingRect(validContours[i]);
        cv::rectangle(marked, bbox, cv::Scalar(0, 0, 255), 2);  //  红色边框

        // 中心点(用 moments 计算质心)
        cv::Moments m = cv::moments(validContours[i]);
        if (m.m00 > 0) {
            int cx = static_cast<int>(m.m10 / m.m00);
            int cy = static_cast<int>(m.m01 / m.m00);
            cv::circle(marked, cv::Point(cx, cy), 4, cv::Scalar(0, 255, 255), -1);  // 黄色圆点
        }

        // 编号(白字)
        cv::putText(marked, std::to_string(i + 1),
                    cv::Point(bbox.x + 6, bbox.y + 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(255, 255, 255), 2);
    }

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