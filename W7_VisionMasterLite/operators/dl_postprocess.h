#pragma once
// ============================================================
// dl_postprocess — 深度学习推理的「前后处理」
//
// === 为什么要单独拆一个文件 ===
// dl_infer 算子里真正跟深度学习绑死的只有一件事:
//   ONNX 张量 → 前向计算 → 输出张量
// 这一步必须有 ONNX Runtime,没装就跑不了。
//
// 但围绕它的活计 —— letterbox 缩放、归一化、NMS、置信度过滤、
// 把输出框映射回原图坐标 —— 一行推理代码都不用写,纯 OpenCV,
// 而且**恰恰是出错最多、最难查的部分**:
//   - letterbox 的 pad 用错边 → 模型精度悄悄下降,推理能跑但结果全错
//   - YOLOv5 和 YOLOv8 的输出布局不一样(C-major vs N-major),
//     搞反了不报错,只是全检测不出来
//   - NMS 阈值随手给 0.1 → 相邻目标被误合并
//
// 这些以前只能等真接上模型、肉眼对比图才能发现。
// 现在拆出来,无头测试直接拿构造张量跑,不需要任何模型文件。
//
// 依赖:仅 OpenCV,无任何 DL 运行时
// ============================================================

#include <QString>
#include <QStringList>
#include <vector>
#include <opencv2/core.hpp>

namespace w7 {
namespace dl {

// === 一个检测框 ===
struct Detection {
    cv::Rect2f box;     // 原图坐标系
    int   classId = -1;
    float conf    = 0.f;
};

// === letterbox:等比缩放 + 居中补边 ===
// 返回值 scale(实际缩放倍数);padX/padY 是补在左/上的边长。
//
// 为什么必须"等比"而不是直接拉伸成 640x640:
//   宽高比变了,目标在网络眼里就变形了,置信度会掉。
//   letterbox 保持形状,代价是图里有黑边 —— 这是标准做法。
//
// 关键:返回的 pad 后面要把检测框减掉,否则框会整体偏移
//      (补左边时所有框右移了 padX,这是最常见的坐标系 bug)
cv::Mat letterbox(const cv::Mat& src, cv::Size target,
                  float* scale = nullptr, int* padX = nullptr, int* padY = nullptr);

// === NMS:非极大值抑制 ===
// 按置信度从高到低保留框,与已保留框 IoU 超过阈值的一律丢掉。
// iou 太大 → 相邻/重叠的多个目标被误合并成一个
// iou 太小  → 同一个目标被重复框了好几个
std::vector<Detection> nms(std::vector<Detection> dets, float iouThresh);

// === YOLOv8 输出解析 ===
// 原始输出形状 (1, 4+nc, 8400),但**传进来的是去掉 batch 维的二维视图**
//   行 = 通道,列 = 第几个框  →  at(通道, 框)
// 通道布局:[cx, cy, w, h, cls0_score, cls1_score, ...]
// 没有 objectness 通道 —— YOLOv8 把背景并进了 softmax,这是它和 v5 的关键差别。
// 值域已经是 0..1 的概率(模型内部做过 sigmoid/softmax)。
std::vector<Detection> parseYolov8(const cv::Mat& out, cv::Size origSize,
                                   const cv::Size& netInput,
                                   int padX, int padY, float scale,
                                   float confThresh);

// === YOLOv5 输出解析 ===
// 原始输出形状 (1, 25200, 5+nc),去掉 batch 后是二维 (N, 5+nc)
//   行 = 框,列 = 属性    →  at(框, 属性)   ← 和 v8 正好对调
// 属性布局:[cx, cy, w, h, obj_score, cls0_score, ...]
// 置信度 = obj_score * cls_score(两级相乘),v5 特有的结构。
std::vector<Detection> parseYolov5(const cv::Mat& out, cv::Size origSize,
                                   const cv::Size& netInput,
                                   int padX, int padY, float scale,
                                   float confThresh);

// === 在图上画检测框 + 类别 + 置信度 ===
// 返回写盘路径(调用方负责 imwrite)
cv::Mat drawDetections(const cv::Mat& img, const std::vector<Detection>& dets,
                       const QStringList& classNames);

} // namespace dl
} // namespace w7
