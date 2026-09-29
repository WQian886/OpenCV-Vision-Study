#pragma once
// ============================================================
// image_source_camera — 工业相机实时图像源(对标 VM "相机采集")
// 输入:无 (起点算子,主动从海康工业相机取流)
// 输出:image_path (临时文件路径) + image_size/width/height + camera_info
// params: camera_ip(string) + exposure(float,ms) + trigger_mode(int)
//
// 用法:
//   - 此节点必须放在流水线最前面
//   - camera_ip 填海康 GigE 相机 IP (如 192.168.1.10)
//   - Run 时取一帧 → 写临时文件 → 下游算子用 image_path 读
// 依赖: 海康 MVS SDK (D:\MVS),环境变量 MVS_DIR 指向 SDK 根目录
//   没装 SDK 时此算子不被编译(其他算子不受影响)
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLineEdit;
class QDoubleSpinBox;
class QComboBox;

namespace w7 {

class ImageSourceCamera : public OperatorBase {
public:
    QString name() const override { return "image_source_camera"; }
    QString displayName() const override { return "工业相机实时流"; }
    QString category() const override { return "acquisition"; }
    QString description() const override {
        return "从海康 GigE Vision 工业相机取一帧(对标 VM '相机采集' 图像源)。依赖 MVS SDK,环境变量 MVS_DIR 指向 SDK 根目录";
    }
    // 图像源:出图像,没有输入口
    PinType outputPinType() const override { return PinType::Image; }
    bool isSource() const override { return true; }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === 工业相机参数面板 ===
class ImageSourceCameraForm : public ParamForm {
    Q_OBJECT
public:
    explicit ImageSourceCameraForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QLineEdit*    m_cameraIp  = nullptr;
    QDoubleSpinBox* m_exposure = nullptr;
    QComboBox*    m_trigger   = nullptr;
};

} // namespace w7