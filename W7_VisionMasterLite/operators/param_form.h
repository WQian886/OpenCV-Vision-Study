#pragma once
// ============================================================
// ParamForm — 通用算子参数表单 widget 基类
// 子类 override buildUI() 添加具体字段
// 提供:valueChanged(QJsonObject) 信号 + collect() 方法
// ============================================================

#include <QWidget>
#include <QJsonObject>
#include <QFormLayout>

class QPushButton;

namespace w7 {

class ParamForm : public QWidget {
    Q_OBJECT
public:
    explicit ParamForm(QWidget* parent = nullptr);

    // === S3-bug 修复:显式初始化 UI(避免基类构造函数调用虚函数不 dispatch 的 bug) ===
    // 调用方必须在 new 出对象之后立刻调用一次
    void initialize();

signals:
    // 表单值变化时发(用户改输入框时)
    void valueChanged(const QJsonObject& params);

public slots:
    // 外部触发值变化时调(比如选中其他节点时重新填充)
    virtual void setParams(const QJsonObject& params);

protected:
    // 子类必须实现:把字段塞到 m_layout
    virtual void buildUI() {}   // 默认空实现,子类 override

    // 子类可选实现:从表单收集 QJsonObject
    virtual QJsonObject collect() const = 0;

    QFormLayout* m_layout = nullptr;
    QPushButton* m_applyBtn = nullptr;
};

} // namespace w7
