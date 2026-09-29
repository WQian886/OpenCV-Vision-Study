#pragma once
// ============================================================
// image_source_video — 视频文件图像源(对标 VM "视频图像" 图像源)
//
// === 本算子的来历 ===
// VM 的图像源有四种:本地图像 / 文件夹 / 视频 / 相机。
// W7 之前只有「文件夹」和「相机」,本地单张图靠工具栏兜底。
// 视频源补上之后,四种图像源齐了 ——
// 更重要的是它引入了「一帧一帧跑流水线」这个概念:
//
//   文件夹源 = N 张静态图,逐张跑
//   视频源   = N 帧动态图,逐帧跑
//
// 这两件事是同一件事。Run 引擎因此支持帧循环:
//   源算子输出 frame_count → 引擎按帧重跑整张图
//   引擎通过 inputs["frame_index"] 告诉源"这轮要第几帧"
// 文件夹源和视频源共用这一套,不是两套并行机制。
//
// 输入:无(起点算子)
// 输出:image_path(当前帧快照) + frame_count + current_index
//       + fps + width/height
// params: video_path, max_frames, frame_step
//
// === 关于视频和图像的关键差异 ===
// 视频解码出来的帧没有"文件路径",而 W7 的 Run 引擎只传 path 字符串
// (下游算子各自 imread)。所以这里把当前帧**落盘成临时 png**,
// 下游拿到的仍然是普通 image_path,和图像源完全一致。
// 代价是每帧一次磁盘写 —— 对离线产线调试完全够用,
// 真要上实时流,该走 image_source_camera 那条路(不进文件系统)。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLineEdit;
class QSpinBox;

namespace w7 {

class ImageSourceVideo : public OperatorBase {
public:
    QString name() const override { return "image_source_video"; }
    QString displayName() const override { return "视频文件图像源"; }
    QString category() const override { return "acquisition"; }
    QString description() const override {
        return "从视频文件逐帧取图送进流水线(对标 VM '视频图像' 图像源)。"
               "Run 时按帧循环,每帧都让下游算子跑一遍";
    }
    // 图像源:出图像,没有输入口
    PinType outputPinType() const override { return PinType::Image; }
    bool isSource() const override { return true; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === 视频图像源参数面板 ===
class ImageSourceVideoForm : public ParamForm {
    Q_OBJECT
public:
    explicit ImageSourceVideoForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QLineEdit* m_videoPath = nullptr;
    QSpinBox*  m_maxFrames = nullptr;
    QSpinBox*  m_frameStep = nullptr;
};

} // namespace w7
