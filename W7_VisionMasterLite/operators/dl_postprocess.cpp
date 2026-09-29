#include "dl_postprocess.h"
#include <QJsonObject>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace w7 {
namespace dl {

cv::Mat letterbox(const cv::Mat& src, cv::Size target,
                  float* scale, int* padX, int* padY) {
    if (src.empty() || target.width <= 0 || target.height <= 0)
        return cv::Mat();

    // 等比缩放:取宽高比例的较小者,保证长边刚好贴住,
    // 短边留出补边空间。用 0.0 让 OpenCV 自己算(INTER_AREA 缩小时更清楚)
    const float s = std::min(static_cast<float>(target.width)  / src.cols,
                             static_cast<float>(target.height) / src.rows);
    const int newW = std::max(1, static_cast<int>(std::round(src.cols * s)));
    const int newH = std::max(1, static_cast<int>(std::round(src.rows * s)));

    const int px = (target.width  - newW) / 2;
    const int py = (target.height - newH) / 2;

    if (scale) *scale = s;
    if (padX) *padX = px;
    if (padY) *padY = py;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(newW, newH), 0, 0,
               s < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);

    // 补边颜色用 114(灰),不是纯黑 —— YOLO 系列训练时就是用 114 填充的,
    // 用 0 会让模型看到训练时没见过的分布,精度掉一截。这是细节,但很值钱。
    cv::Mat canvas(target, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(px, py, newW, newH)));
    return canvas;
}

static float iou(const cv::Rect2f& a, const cv::Rect2f& b) {
    // 先求交集再求并集,注意用 float —— 检测框坐标大,整数截断会引入偏差
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width,  b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float iw = x2 - x1, ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.f;
    const float inter = iw * ih;
    const float uni = a.width * a.height + b.width * b.height - inter;
    return uni > 0 ? inter / uni : 0.f;
}

std::vector<Detection> nms(std::vector<Detection> dets, float iouThresh) {
    // 按置信度降序 —— NMS 的核心就是「先留最有把握的」,
    // 后面的框拿它当基准判断该不该丢。顺序反了结果就反了。
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.conf > b.conf; });

    std::vector<Detection> keep;
    std::vector<bool> dead(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dead[i]) continue;
        keep.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (dead[j]) continue;
            // 类别不同就不抑制 —— 并排的两个人不该被 NMS 合成一个
            if (dets[i].classId != dets[j].classId) continue;
            if (iou(dets[i].box, dets[j].box) > iouThresh) dead[j] = true;
        }
    }
    return keep;
}

// === 公共:把「网络输入坐标」的框换算回原图坐标 ===
// 链路是: 网络坐标 → 减去 letterbox 补边 → 除以缩放倍数 → 原图坐标
// 三步都做反了就框飘,顺序错了也飘。这是 DL 可视化里最常见的 bug。
static cv::Rect2f toOriginal(cv::Point2f center, cv::Size2f wh,
                             cv::Size origSize, int padX, int padY, float scale) {
    float x = (center.x - padX) / scale;
    float y = (center.y - padY) / scale;
    float w = wh.width  / scale;
    float h = wh.height / scale;

    // 裁到原图内 —— 模型偶尔会给出越界框(框住半个画面外的东西),
    // 不裁的话画框时 cv::rectangle 会画到图外,截图上看像画对了其实偏了
    const float x1 = std::max(0.f, x - w / 2);
    const float y1 = std::max(0.f, y - h / 2);
    const float x2 = std::min(static_cast<float>(origSize.width),  x + w / 2);
    const float y2 = std::min(static_cast<float>(origSize.height), y + h / 2);
    return cv::Rect2f(x1, y1, std::max(0.f, x2 - x1), std::max(0.f, y2 - y1));
}

std::vector<Detection> parseYolov8(const cv::Mat& out, cv::Size origSize,
                                   const cv::Size& netInput,
                                   int padX, int padY, float scale,
                                   float confThresh) {
    std::vector<Detection> dets;
    // === 这里收到的是「去掉 batch 维」的二维视图 (4+nc, N),不是 (1,4+nc,N) ===
    // 因为 OpenCV 的 Mat 根本没有三维,调用方拿到的就是二维连续内存。
    // 写 dims != 3 的检查会把自己的合法输入判成畸形 —— 这是个真实的坑。
    // 行 = 通道,列 = 第几个框,索引是 at(通道, 框)
    if (out.dims != 2) return dets;
    const int channels = out.rows;
    const int numBoxes = out.cols;
    if (channels < 5) return dets;          // 4 框 + 至少 1 类

    const int numClasses = channels - 4;
    // YOLOv8 没有 objectness:score 直接就是类别概率
    for (int i = 0; i < numBoxes; ++i) {
        float best = 0.f;
        int bestCls = -1;
        for (int c = 0; c < numClasses; ++c) {
            const float s = out.at<float>(4 + c, i);
            if (s > best) { best = s; bestCls = c; }
        }
        if (bestCls < 0 || best < confThresh) continue;
        const cv::Point2f center(out.at<float>(0, i), out.at<float>(1, i));
        const cv::Size2f wh(out.at<float>(2, i), out.at<float>(3, i));
        Detection d;
        d.box = toOriginal(center, wh, origSize, padX, padY, scale);
        d.classId = bestCls;
        d.conf = best;
        dets.push_back(d);
    }
    return dets;
}

std::vector<Detection> parseYolov5(const cv::Mat& out, cv::Size origSize,
                                   const cv::Size& netInput,
                                   int padX, int padY, float scale,
                                   float confThresh) {
    std::vector<Detection> dets;
    // 期望 (1, N, 5+nc) → 去掉 batch 后是 (N, 5+nc),行 = 框,列 = 属性
    // 索引是 at(框, 属性),和 v8 的 at(通道, 框) 正好对调 —— 别搞混
    if (out.dims != 2) return dets;
    const int numBoxes   = out.rows;
    const int totalAttrs = out.cols;
    if (totalAttrs < 6) return dets;        // 4 框 + obj + 至少 1 类

    const int numClasses = totalAttrs - 5;
    for (int i = 0; i < numBoxes; ++i) {
        const float obj = out.at<float>(i, 4);
        // objectness 先过一道 —— v5 是两级结构,obj 低说明这个位置压根没东西
        if (obj < confThresh) continue;

        float best = 0.f;
        int bestCls = -1;
        for (int c = 0; c < numClasses; ++c) {
            const float s = out.at<float>(i, 5 + c);
            if (s > best) { best = s; bestCls = c; }
        }
        if (bestCls < 0) continue;
        const float conf = obj * best;       // ← v5 特有:两级相乘
        if (conf < confThresh) continue;

        const cv::Point2f center(out.at<float>(i, 0), out.at<float>(i, 1));
        const cv::Size2f wh(out.at<float>(i, 2), out.at<float>(i, 3));
        Detection d;
        d.box = toOriginal(center, wh, origSize, padX, padY, scale);
        d.classId = bestCls;
        d.conf = conf;
        dets.push_back(d);
    }
    return dets;
}

cv::Mat drawDetections(const cv::Mat& img, const std::vector<Detection>& dets,
                       const QStringList& classNames) {
    cv::Mat canvas = img.clone();
    for (const auto& d : dets) {
        const cv::Rect r(cv::Point(cvRound(d.box.x), cvRound(d.box.y)),
                         cv::Point(cvRound(d.box.x + d.box.width),
                                   cvRound(d.box.y + d.box.height)));
        // 颜色按类别取,固定住 —— 同一个类别每次跑颜色一样,便于肉眼追踪
        const cv::Scalar color(((d.classId * 67) % 180) + 40,
                               ((d.classId * 113) % 180) + 40,
                               ((d.classId * 173) % 180) + 40);
        cv::rectangle(canvas, r, color, 2);

        QString name = (d.classId >= 0 && d.classId < classNames.size())
                       ? classNames[d.classId] : QString("cls%1").arg(d.classId);
        const QString label = QString("%1 %2").arg(name).arg(d.conf, 0, 'f', 2);

        // 标签画在框上方;顶到图外就压回框内,否则第一行框的标签看不见
        int ty = std::max(18, r.y - 5);
        if (r.y - 5 < 18) ty = r.y + 18;
        cv::putText(canvas, label.toStdString(), cv::Point(r.x, ty),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
    }
    return canvas;
}

} // namespace dl
} // namespace w7
