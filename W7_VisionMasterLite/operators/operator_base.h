#pragma once
// ============================================================
// W7 Mini VisionMaster — 算子抽象基类
// 所有定位/测量/识别/检测算子继承此类
// 设计:对标海康 VM 算子接口(name/run/createParamWidget)
// ============================================================

#include <QString>
#include <QJsonObject>
#include <QWidget>
#include <opencv2/opencv.hpp>

namespace w7 {

class OperatorBase {
public:
    virtual ~OperatorBase() = default;

    // 算子唯一名(英文,与 VM 官方名对齐)
    virtual QString name() const = 0;

    // 中文显示名
    virtual QString displayName() const = 0;

    // 所属模块("positioning" / "measurement" / "identification" / "detection")
    virtual QString category() const = 0;

    // 算子描述(1-2 句话)
    virtual QString description() const = 0;

    // === S3: 算子默认参数 ===
    virtual QJsonObject defaultParams() const = 0;

    // 执行算子:params 算子参数,inputs 上游输入(图像 / 数值 / 字符串)
    // 输出统一打包成 QJsonObject
    virtual QJsonObject run(const QJsonObject& params,
                            const QJsonObject& inputs) = 0;

    // === S3: 创建参数配置面板 ===
    // parent: 父窗口(QStackedWidget)
    // params: 当前参数(用于初始化表单)
    // 返回的 widget 必须发 valueChanged(QJsonObject) 信号
    // 当用户改了表单时,MainWindow 会收到并更新 node.setParams()
    virtual QWidget* createParamWidget(QWidget* parent,
                                        const QJsonObject& params) {
        Q_UNUSED(parent);
        Q_UNUSED(params);
        return nullptr;
    }
};

} // namespace w7

Q_DECLARE_INTERFACE(w7::OperatorBase, "w7.OperatorBase/1.0")
