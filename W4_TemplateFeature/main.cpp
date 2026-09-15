// ============================================================
// W4: 模板匹配 + ORB 特征点
// 学习目标:
//   1. cv::matchTemplate 单目标模板匹配
//   2. cv::minMaxLoc 找最相似位置
//   3. cv::ORB 特征点提取
//   4. cv::BFMatcher 暴力匹配
//   5. cv::findHomography 单应性矩阵
//   6. 实战: 在 PCB 上识别电阻、电容、IC
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
// 生成模拟 PCB 测试图(W3 同款 8 个金色元件 + 文字 + 噪声)
// ============================================================
static cv::Mat generatePcbLikeImage() {
    const int W = 640, H = 480;
    cv::Mat img(H, W, CV_8UC3, cv::Scalar(40, 80, 40));   // 深绿 PCB 底色

    // 8 个金色元件(80x80 = 6400 像素/个)
    std::vector<cv::Rect> pads = {
        cv::Rect(50,  100, 80, 80), cv::Rect(200, 100, 80, 80),
        cv::Rect(350, 100, 80, 80), cv::Rect(500, 100, 80, 80),
        cv::Rect(50,  300, 80, 80), cv::Rect(200, 300, 80, 80),
        cv::Rect(350, 300, 80, 80), cv::Rect(500, 300, 80, 80),
    };
    for (const auto& p : pads) {
        cv::rectangle(img, p, cv::Scalar(60, 180, 220), -1);  // 金黄色填充
    }

    // 中央文字
    cv::putText(img, "W4 Template Feature Test",
                cv::Point(100, 240), cv::FONT_HERSHEY_SIMPLEX,
                0.7, cv::Scalar(200, 200, 200), 2);

    // 加点噪声
    cv::Mat noise(img.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(25));
    cv::add(img, noise, img, cv::noArray(), CV_8UC3);
    return img;
}

// ============================================================
// 生成模拟"模板"小图(取自 PCB 第一个焊点区域)
// 真实场景:这是你用手机拍下来的"电阻长这样"的小图
// ============================================================
static cv::Mat generateTemplateImage() {
    // 80x80 的纯金色方块,代表"我们要找的目标"
    cv::Mat tpl(80, 80, CV_8UC3, cv::Scalar(60, 180, 220));
    // 加点噪声让它更像真实拍的照片
    cv::Mat noise(tpl.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(15));
    cv::add(tpl, noise, tpl, cv::noArray(), CV_8UC3);
    return tpl;
}

// ============================================================
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ============ 1. 读大图(PCB) ============
    const std::string imgPath = "images/pcb_test.jpg";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cout << "[提示] " << imgPath << " 不存在,自动生成 PCB 测试图\n";
        img = generatePcbLikeImage();
        cv::imwrite(imgPath, img);
        std::cout << "[提示] 已保存到 " << imgPath << "\n\n";
    }
    std::cout << "===== 图像信息 =====\n";
    std::cout << "  PCB 大图: " << img.cols << " x " << img.rows << "\n\n";

    // ============ 2. 准备模板(从 PCB 抠一块出来,或单独拍一张) ============
    // 真实做法:用 cv::imread 读你拍好的小图
    // 这里为了自包含,从 PCB 左上角第一个焊点直接 ROI 截取
    cv::Mat tpl = img(cv::Rect(50, 100, 80, 80)).clone();
    cv::imwrite("images/w4_template.png", tpl);
    std::cout << "===== 模板信息 =====\n";
    std::cout << "  模板尺寸: " << tpl.cols << " x " << tpl.rows << "\n";
    std::cout << "  已保存到 images/w4_template.png\n\n";

    // ============ 3. 模板匹配(matchTemplate) ⭐ W4a-1 ============
    // 第 1 步:滑动匹配,得到 result 图(每个像素 = 该位置的相似度)
    cv::Mat result;
    cv::matchTemplate(img, tpl, result, cv::TM_CCOEFF_NORMED);

    // 第 2 步:找 result 图里最大值的位置(最相似)
    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    std::cout << "===== 模板匹配结果 =====\n";
    std::cout << "  result 图尺寸: " << result.cols << " x " << result.rows << "\n";
    std::cout << "  最相似位置:   (" << maxLoc.x << ", " << maxLoc.y << ")\n";
    std::cout << "  相似度分数:   " << std::fixed << std::setprecision(4) << maxVal
              << "  (越接近 1.0 越像)\n\n";

    // 第 3 步:在 img 上画红框标注匹配位置
    cv::Mat marked = img.clone();
    cv::Rect matchBox(maxLoc.x, maxLoc.y, tpl.cols, tpl.rows);
    cv::rectangle(marked, matchBox, cv::Scalar(0, 0, 255), 3);  // 红框,粗线
    cv::putText(marked, "match " + std::to_string(maxVal).substr(0, 5),
                cv::Point(maxLoc.x, maxLoc.y - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 1);

    cv::imwrite("images/w4_match_result.png", marked);
    cv::imwrite("images/w4_score_map.png", result * 255);  // result 是浮点,放大到 0-255 才能存

    std::cout << "===== 保存 =====\n";
    std::cout << "  images/w4_match_result.png (红框标注)\n";
    std::cout << "  images/w4_score_map.png    (相似度热力图)\n\n";

    // ============ 4. 显示窗口 ============
    cv::Mat tplResized;
    cv::resize(tpl, tplResized, cv::Size(), 2.0, 2.0);  // 模板放大 2 倍便于看
    cv::namedWindow("PCB 大图 + 红框", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("模板 (放大 2x)", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("相似度热力图", cv::WINDOW_AUTOSIZE);
    cv::imshow("PCB 大图 + 红框", marked);
    cv::imshow("模板 (放大 2x)", tplResized);
    cv::imshow("相似度热力图", result);  // 浮点图直接显示

    std::cout << "===== 显示 =====\n";
    std::cout << "  窗口1: PCB 大图 + 红框标注\n";
    std::cout << "  窗口2: 模板 (放大 2x)\n";
    std::cout << "  窗口3: 相似度热力图 (越亮越像)\n";
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();

    std::cout << "\n✅ W4a-1 完成!\n";
    std::cout << "下一练: 试 6 种 method 对比 / 多目标匹配\n";
    return 0;
}
