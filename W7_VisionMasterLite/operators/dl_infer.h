#pragma once
// ============================================================
// dl_infer — ONNX 深度学习推理算子(对标 VM "AI 推理"/"深度学习")
//
// === 本算子的来历 ===
// 前面几个算子都是传统 CV:阈值、模板、连通域 —— 规则是人写的。
// 工业视觉现在的分水岭是深度学习:规则学不出来,得靠数据训。
// W7 到这一步才算把「传统产线」和「AI 产线」两种形态都摆上了流水线,
// 而且它们之间可以任意串联(AI 定位 → 传统测量 → 传统判定)。
//
// 输入:image_path (上游图像源给)
// 输出:detections[] + count + inference_ms + marked_path
// params: model_path + input_size + conf/nms 阈值 + class_names + output_layout
//
// === 依赖 ===
// 需要 ONNX Runtime。环境变量 ONNXRUNTIME_DIR 指向 SDK 根目录
// (默认 D:\onnxruntime),需要 include\ 和 lib\ 两个子目录。
// 没装时:算子照常注册、照常显示参数面板,Run 时返回友好错误 ——
//         其余算子完全不受影响。这跟 image_source_camera 对 MVS SDK
//         的处理是同一套模式(见 CMakeLists 的 HAS_ONNXRUNTIME 探测)。
//
// === 为什么前后处理不写在这里 ===
// letterbox / NMS / 输出布局解析全在 dl_postprocess.{h,cpp}。
// 那些代码不需要任何 DL 运行时,已经被无头测试单独覆盖了 ——
// 见 tests/test_dl_postprocess.cpp。推理本身反而不容易测
// (要真模型 + 真图片),所以把能测的部分测透,别混在一起。
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;

namespace w7 {

class DlInfer : public OperatorBase {
public:
    QString name() const override { return "dl_infer"; }
    QString displayName() const override { return "DL 推理 (ONNX)"; }
    QString category() const override { return "dl"; }
    QString description() const override {
        return "加载 ONNX 模型做目标检测,支持 YOLOv5/v8 输出布局。"
               "需 ONNX Runtime,环境变量 ONNXRUNTIME_DIR 指向 SDK 根目录";
    }
    // 吃图,吐检测框(数据)
    PinType inputPinType()  const override { return PinType::Image; }
    PinType outputPinType() const override { return PinType::Data; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;

    // 运行时是否可用(没装 ONNX Runtime 时为 false)。
    // 参数面板用它把说明文字置灰,免得用户填完才发现跑不了。
    static bool runtimeAvailable();
};

// === DL 推理参数面板 ===
class DlInferForm : public ParamForm {
    Q_OBJECT
public:
    explicit DlInferForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QLineEdit*      m_modelPath = nullptr;
    QSpinBox*       m_inW = nullptr;
    QSpinBox*       m_inH = nullptr;
    QDoubleSpinBox* m_conf = nullptr;
    QDoubleSpinBox* m_iou  = nullptr;
    QLineEdit*      m_classes = nullptr;
    // 叫 m_outLayout 不叫 m_layout —— ParamForm 已经有 m_layout(QFormLayout*),
    // 同名会把它遮住,于是 m_layout->addRow() 全变成在 QComboBox 上调 addRow。
    // 踩过一次,注释留着。
    QComboBox*      m_outLayout = nullptr;
    QLabel*         m_hint   = nullptr;   // 运行时缺失时的提示
};

} // namespace w7
