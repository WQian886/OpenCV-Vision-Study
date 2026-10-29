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
//
// ⚠ 这一层**已经不在这个文件里了**。
//
// 模板匹配和 ORB 的活儿都在 feature_pipeline.{h,cpp}：
//   生成图 → 抠模板 → 算相似度图 → 找最佳/全部匹配 → 标注；
//   ORB 检测 → 描述子 → 比率检验 → 单应性。
// 这个文件只剩两件事：**调它**，和**把结果给人看**（imwrite + imshow）。
//
// 为什么要抽：imshow/waitKey 要显示器，imwrite 要写盘 ——
// 两个都不是无头环境能做的，所以处理链留在 main() 里就**一条测试都写不了**。
// 抽出来之后，tests/test_w4_template_feature.cpp 和这个文件调的是**同一份实现**，
// 绿灯才真的说明那套逻辑是好的。
//
// 同一份实现，两个调用方 —— 仓库里的既有做法（W2 的 color_pipeline、
// W3 的 edge_pipeline、W6 的 w6_cli）。抄一份到测试里就是「抄副本」：
// 改了这里，测试还在按旧值断言，于是测试绿着而 demo 已经变了。
//
// ------------------------------------------------------------
// 📌 这一轮的 demo 顺手把一个**常见的坑**摆出来了：
//
// 「模板匹配用 TM_CCOEFF_NORMED，README 说它是工业首选」——
// 那话**对有纹理的模板成立**，对**纯色模板不成立**。
//
// 实测（同一个金块模板，同一张图）：
//     TM_CCORR_NORMED    0.985996  @ 焊点上   ← 看得见目标
//     TM_CCOEFF_NORMED   0.030943  @ 背景上   ← 看不见目标
//
// 原因：CCOEFF 会**去掉均值**，只剩「噪声」可比 ——
// 而模板的噪声和图上的噪声是两回事，相关性本来就该接近 0。
// 「形状一样」这件事，在 CCOEFF 的眼里根本不存在。
// ============================================================

#include "feature_pipeline.h"

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

    // ===== 1. 读图(没有就生成) =====
    const std::string imgPath = "images/pcb_test.jpg";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cout << "[提示] " << imgPath << " 不存在,自动生成 PCB 测试图\n";
        img = w4::generatePcbImage();
        cv::imwrite(imgPath, img);
        std::cout << "[提示] 已保存到 " << imgPath << "\n\n";
    }
    std::cout << "===== 图像信息 =====\n";
    std::cout << "  PCB 大图: " << img.cols << " x " << img.rows << "\n\n";

    // ===== 2. 两种模板 =====
    //
    // A: 从大图上抠一块（老 demo 用的就是这种）
    // B: 一个**独立**的金块（相当于"用手机拍的电阻"）
    //
    // 两者长得很像，但 A 的分数是**白送的**：它和大图那块逐像素相同，
    // 任何图和自己匹配都是 1.0 —— 那个分数不含任何信息。
    const cv::Mat tplSelf = w4::cutTemplate(img, w4::templateRect());
    const cv::Mat tpl = w4::makeSyntheticTemplate();

    std::cout << "===== 模板 =====\n";
    std::cout << "  模板尺寸: " << tpl.cols << " x " << tpl.rows << "\n";
    std::cout << "  A 抠出来的: 与原图那块逐像素相同 → 匹配分数必然 1.0(白送)\n";
    std::cout << "  B 独立金块: 形状颜色一样,噪声是另一份 → 分数才有信息\n\n";

    // ===== 3. 六种 method 对比（课后练习 2）=====
    //
    // ⚠ 这里最值得看的一行: **CCOEFF_NORMED 对纯色模板是瞎的**。
    const int methods[] = {cv::TM_SQDIFF, cv::TM_SQDIFF_NORMED, cv::TM_CCORR,
                           cv::TM_CCORR_NORMED, cv::TM_CCOEFF, cv::TM_CCOEFF_NORMED};
    const char* names[] = {"TM_SQDIFF       ", "TM_SQDIFF_NORMED",
                           "TM_CCORR        ", "TM_CCORR_NORMED ",
                           "TM_CCOEFF       ", "TM_CCOEFF_NORMED"};
    std::cout << "===== 六种 method 对比（模板 B）=====\n";
    for (int i = 0; i < 6; ++i) {
        const cv::Mat scores = w4::scoreMap(img, tpl, methods[i]);
        const w4::Match best = w4::bestMatch(scores, methods[i]);
        std::cout << "  " << names[i] << " 最佳分数 "
                  << std::fixed << std::setprecision(6) << best.score
                  << "  位置 (" << best.loc.x << ", " << best.loc.y << ")"
                  << (w4::higherIsBetter(methods[i]) ? "  [越大越像]" : "  [越小越像]")
                  << "\n";
    }
    std::cout << "\n";

    // ===== 4. 多目标匹配（课后练习 3）=====
    //
    // 8 个焊点长得一模一样 ⇒ 纯色模板**分不出谁是谁**
    // （实测 8 个焊点的分数散得只有 0.0006）。
    const int kMethod = cv::TM_CCORR_NORMED;
    const cv::Mat scores = w4::scoreMap(img, tpl, kMethod);
    const std::vector<w4::Match> matches = w4::findMatches(scores, 0.90, kMethod, 80);

    std::cout << "===== 多目标匹配（阈值 0.90）=====\n";
    std::cout << "  找到 " << matches.size() << " 个目标";
    for (const auto& m : matches) {
        std::cout << " (" << std::fixed << std::setprecision(4) << m.score << "@"
                  << m.loc.x << "," << m.loc.y << ")";
    }
    std::cout << "\n";
    std::cout << "  ⚠ 注意: 8 个焊点的分数几乎一样(实测散得只有 0.0006)\n";
    std::cout << "    ⇒ 这张图上「最相似的那一个」是**分不出来的**\n";
    std::cout << "  ⚠ 注意: 朴素数像素会数出上万个(阈值 0.90 实测 12376 个)\n";
    std::cout << "    ⇒ 必须 NMS,否则一个目标被数成一大片\n\n";

    // ===== 5. 画框 =====
    cv::Mat marked = img.clone();
    for (const auto& m : matches) {
        marked = w4::drawMatch(marked, m, tpl.size());
    }
    // 顺带把「最佳匹配」单独画出来（CCOEFF_NORMED 的那个 = 落在背景上的）
    const cv::Mat scCoeff = w4::scoreMap(img, tpl, cv::TM_CCOEFF_NORMED);
    const w4::Match bestCoeff = w4::bestMatch(scCoeff, cv::TM_CCOEFF_NORMED);
    cv::Mat markedCoeff = w4::drawMatch(img, bestCoeff, tpl.size());

    // ===== 6. ORB 特征点（课后练习 6/7/8）=====
    //
    // 模板取图上那块**唯一**的文字 —— 8 个焊点长得一模一样,
    // 拿它们当 ORB 模板,ratio test 会把匹配全毙掉（实测好匹配 0 对）。
    const cv::Rect orbRect = w4::orbTemplateRect();
    const cv::Mat orbTpl = w4::cutTemplate(img, orbRect);

    std::vector<cv::KeyPoint> kpsTpl, kpsImg;
    const cv::Mat descTpl = w4::detectDescriptors(orbTpl, 500, &kpsTpl);
    const cv::Mat descImg = w4::detectDescriptors(img, 1000, &kpsImg);
    const std::vector<cv::DMatch> good = w4::matchDescriptors(descTpl, descImg, 0.75);
    const cv::Mat H = w4::estimateHomography(kpsTpl, kpsImg, good);

    std::cout << "===== ORB 特征点 =====\n";
    std::cout << "  模板区域: (" << orbRect.x << ", " << orbRect.y << ", "
              << orbRect.width << ", " << orbRect.height << ")  ← 图上那块唯一的文字\n";
    std::cout << "  模板关键点: " << kpsTpl.size() << " 个\n";
    std::cout << "  大图关键点: " << kpsImg.size() << " 个\n";
    std::cout << "  好匹配:     " << good.size() << " 对 (Lowe ratio 0.75)\n";
    if (!H.empty()) {
        std::vector<cv::Point2f> corners = {
            cv::Point2f(0, 0), cv::Point2f(static_cast<float>(orbRect.width), 0),
            cv::Point2f(static_cast<float>(orbRect.width),
                        static_cast<float>(orbRect.height)),
            cv::Point2f(0, static_cast<float>(orbRect.height))};
        std::vector<cv::Point2f> out;
        cv::perspectiveTransform(corners, out, H);
        std::cout << "  单应性把模板左上角映射到 (" << std::fixed << std::setprecision(1)
                  << out[0].x << ", " << out[0].y << ")\n";
        std::cout << "  ⚠ 模板高 60 像素的话,ORB 一个关键点都找不到(而且不报错)\n";
    }
    std::cout << "\n";

    // ===== 7. 保存结果 =====
    cv::imwrite("images/w4_template.png", tpl);
    cv::imwrite("images/w4_match_result.png", marked);
    cv::imwrite("images/w4_score_map.png", scores * 255);   // 浮点图放大到 0-255 才能存
    if (!good.empty()) {
        cv::Mat drawImg;
        cv::drawMatches(orbTpl, kpsTpl, img, kpsImg, good, drawImg,
                        cv::Scalar(255, 255, 255), cv::Scalar(200, 0, 0),
                        std::vector<char>(), cv::DrawMatchesFlags::DEFAULT);
        cv::imwrite("images/w4_orb_matches.png", drawImg);
    }

    std::cout << "===== 保存 =====\n";
    std::cout << "  images/w4_template.png      (模板)\n";
    std::cout << "  images/w4_match_result.png  (多目标匹配,红框)\n";
    std::cout << "  images/w4_score_map.png     (相似度热力图)\n";
    std::cout << "  images/w4_orb_matches.png   (ORB 匹配连线)\n\n";

    // ===== 8. 显示窗口 =====
    cv::Mat tplResized;
    cv::resize(tpl, tplResized, cv::Size(), 2.0, 2.0);
    cv::namedWindow("PCB 大图 + 红框", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("模板 (放大 2x)", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("相似度热力图", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("CCOEFF 的最佳匹配(在背景上!)", cv::WINDOW_AUTOSIZE);
    cv::imshow("PCB 大图 + 红框", marked);
    cv::imshow("模板 (放大 2x)", tplResized);
    cv::imshow("相似度热力图", scores);  // 浮点图直接显示
    cv::imshow("CCOEFF 的最佳匹配(在背景上!)", markedCoeff);

    std::cout << "===== 显示 =====\n";
    std::cout << "  窗口1: PCB 大图 + 8 个匹配框\n";
    std::cout << "  窗口2: 模板 (放大 2x)\n";
    std::cout << "  窗口3: 相似度热力图 (越亮越像)\n";
    std::cout << "  窗口4: CCOEFF_NORMED 的最佳匹配 —— 它在**背景**上\n";
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();

    std::cout << "\n✅ W4 完成!\n";
    std::cout << "下一练: 多尺度匹配(图像金字塔) / ORB 抗旋转实战\n";
    return 0;
}
