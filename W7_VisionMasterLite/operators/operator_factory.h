#pragma once
// ============================================================
// OperatorFactory — 算子工厂(根据 opName 创建实例)
// 供 MainWindow::createOperatorByName 调用
// ============================================================

#include <QString>

namespace w7 {

class OperatorBase;

OperatorBase* createOperatorByName(const QString& name);

} // namespace w7
