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
#include "pin_types.h"

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

    // ============================================================
    // === pin 类型(连线检查用) ===
    //
    // 默认全给 Any —— "没声明"意味着"不检查",而不是"拒绝"。
    // 这样新增算子时忘了写声明,功能照常,只是少一层保护;
    // 反过来默认给具体类型的话,每个新算子都得记得覆盖这两个函数,
    // 忘一次就在连线时被莫名拦住,比不检查更烦人。
    // ============================================================

    // 我这个算子吃什么
    virtual PinType inputPinType() const { return PinType::Any; }

    // 我这个算子吐什么
    virtual PinType outputPinType() const { return PinType::Any; }

    // 图像源:没有输入口,不能接在别的算子后面
    virtual bool isSource() const { return false; }
};

} // namespace w7

Q_DECLARE_INTERFACE(w7::OperatorBase, "w7.OperatorBase/1.0")
