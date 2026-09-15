#include "param_form.h"
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>

namespace w7 {

ParamForm::ParamForm(QWidget* parent) : QWidget(parent) {
    // === S3-bug 修复:不在构造函数里调用 buildUI() ===
    // 原因:C++ 在基类构造函数里调虚函数不会 dispatch 到子类
    // → 子类成员(如 m_minArea)永远不会被初始化
    // → 后续 setValue 访问 nullptr → 0xC0000005 @ 0x8
    // 调用方必须在 new 之后立刻调 initialize()
}

void ParamForm::initialize() {
    // 此时 this 已是完整的子类对象,虚函数 dispatch 正确
    auto* root = new QVBoxLayout(this);
    m_layout = new QFormLayout;
    root->addLayout(m_layout);

    buildUI();   // ← 这里会正确调到子类的 buildUI()

    m_applyBtn = new QPushButton(tr("应用参数"));
    root->addWidget(m_applyBtn);
    root->addStretch();

    connect(m_applyBtn, &QPushButton::clicked, this, [this] {
        emit valueChanged(collect());
    });
}

void ParamForm::setParams(const QJsonObject& params) {
    // 默认实现:子类可 override 来用 params 填充表单
    Q_UNUSED(params);
}

} // namespace w7
