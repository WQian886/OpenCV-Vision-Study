#pragma once
// ============================================================
// MainWindow — 主窗口(QMainWindow)
// 4 个 DockWidget + 工具栏 Run/Save/Load + 默认 demo 流水线
// ============================================================

#include <QMainWindow>
#include <QList>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTreeWidget>
#include <QMimeData>
#include <QStack>
#include "pipeline_view.h"
#include "commands/command.h"

class QTextEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QKeyEvent;

namespace w7 {

// 自定义算子树控件:拖拽时返回自定义 mime(application/x-vm-operator)
class OpTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        auto* md = new QMimeData;
        for (auto* it : items) {
            QString opName = it->data(0, Qt::UserRole).toString();
            if (!opName.isEmpty()) {
                md->setData("application/x-vm-operator", opName.toUtf8());
                break;   // 只拖一个算子
            }
        }
        return md;
    }
};

struct OperatorInfo {
    QString name;        // 英文名(注册到 OperatorBase::name)
    QString display;     // 中文显示
    QString category;    // positioning/measurement/identification/detection
    QString description;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onRunClicked();
    void onLoadProject();
    void onSaveProject();
    void onOperatorSelected();
    void onSceneSelectionChanged();   // === S3 ===
    void onParamApplied();            // === S3 ===

protected:
    // === 修复 2: Delete 键删除 ===
    void keyPressEvent(QKeyEvent* event) override;

    // 工具:删除当前选中的所有图元(节点 + 连线)
    void deleteSelectedItems();

private:
    void setupUi();
    void setupToolbar();
    void setupOperatorLibrary();
    void setupParamPanel();
    void setupResultPanel();
    void registerOperators();
    void loadDefaultPipeline();

    // === S3 ===
    void updateParamPanelForNode(w7::OperatorNode* node);

    // 工具函数
    void appendLog(const QString& msg);
    QJsonObject runOperatorByName(const QString& name,
                                   const QJsonObject& params,
                                   const QJsonObject& inputs);

    // === M6 撤销/重做 (Step1 仅框架,Step2/3 接入 addNode/removeNode 等) ===
    // 执行命令:执行 + push undoStack + 清空 redoStack
    void execCommand(Command* cmd);
    // 撤销:从 undoStack 弹 → undo() → push redoStack
    void execUndo();
    // 重做:从 redoStack 弹 → redo() → push undoStack
    void execRedo();

private:
    PipelineView*    m_canvas    = nullptr;
    QTreeWidget*     m_opLib     = nullptr;
    QStackedWidget*  m_paramPanel= nullptr;
    QTextEdit*       m_logView   = nullptr;
    QLabel*          m_resultImg = nullptr;
    QLineEdit*       m_imagePathEdit = nullptr;

    // 算子注册表
    QList<OperatorInfo> m_operatorInfo;
    QList<QPushButton*> m_paramButtons;

    // 当前选中的 PipelineView 节点
    OperatorNode* m_selectedNode = nullptr;

    // === S3: 当前显示在参数面板里的 widget + 关联节点 ===
    // 用 QPointer 防止 widget 析构后悬空(闪退修复)
    QPointer<QWidget> m_currentParamWidget = nullptr;
    OperatorNode*     m_currentParamNode   = nullptr;

    // === S3 重入防护:防止 selectionChanged 在 widget swap 期间递归触发 ===
    bool m_inUpdate = false;

    // === M6 撤销/重做栈 ===
    QStack<Command*> m_undoStack;   // 已执行,等待 undo
    QStack<Command*> m_redoStack;   // 已 undo,等待 redo
};

} // namespace w7
