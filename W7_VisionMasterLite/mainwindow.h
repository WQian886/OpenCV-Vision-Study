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
#include <QPointF>
#include <QAction>
#include <QUuid>
#include "pipeline_view.h"
#include "commands/command.h"

class QTextEdit;
class QPlainTextEdit;
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
    void onNodeDeleteRequested(w7::OperatorNode* node);   // === P0 ===
    void onNodeMoved(w7::OperatorNode* node, const QPointF& oldPos,
                     const QPointF& newPos);              // === M6 ===
    void onViewStepResultRequested(w7::OperatorNode* node);  // === 每步结果查看 ===

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

    // ============================================================
    // === 每步结果查看 ===
    //
    // 以前 Run 完只保留最后一个节点的输出,前面 7 步的结果全丢了。
    // 要看中间某步出了什么,只能重跑一次、把别的节点删掉 —— 调试
    // 一条 8 步的流水线时这个循环很要命。
    //
    // 现在 Run 时把每个节点的 output 都缓存下来(按 QUuid 索引,
    // 不用裸指针:节点删了重建会换地址),右键节点即可回看。
    // ============================================================

    // 把一个 output 渲染到结果区(挑合适的图 + 写详情文本)
    // 返回是否真的画出了图。final result 和「查看该步输出」共用这一份逻辑 ——
    // 两处各写一份的话,加一种新输出类型就必然只改一处,两处会跑偏。
    bool renderOutputToResultPanel(const QJsonObject& out,
                                   const QString& fallbackPath,
                                   const QString& title);
    // 把 output 的全部字段按 类型 = 值 写进详情框
    void showStepDetail(const QString& title, const QJsonObject& out);
    // 节点被删 / 流水线被改动 → 缓存失效
    void clearStepCache();

    // 工具函数
    void appendLog(const QString& msg);
    QJsonObject runOperatorByName(const QString& name,
                                   const QJsonObject& params,
                                   const QJsonObject& inputs);

    // === M6 撤销/重做 ===
    // 执行命令:执行 + push undoStack + 清空 redoStack
    void execCommand(Command* cmd);
    // 撤销:从 undoStack 弹 → undo() → push redoStack
    void execUndo();
    // 重做:从 redoStack 弹 → redo() → push undoStack
    void execRedo();
    // 清空两个栈(Load 新工程后调用,旧命令已无意义且会指向不存在的节点)
    void clearUndoRedoStacks();
    // 刷新工具栏 Undo/Redo 按钮的可用状态
    void updateUndoRedoActions();

    // === M6: 各操作入口 → 生成对应 Command ===
    void doAddNode(const QString& opName, const QString& display,
                   const QString& category, const QPointF& pos);
    void doDeleteNode(OperatorNode* node);
    void doMoveNode(OperatorNode* node, const QPointF& oldPos, const QPointF& newPos);
    void doConnect(OperatorNode* from, OperatorNode* to);

private:
    PipelineView*    m_canvas    = nullptr;
    QTreeWidget*     m_opLib     = nullptr;
    QStackedWidget*  m_paramPanel= nullptr;
    QTextEdit*       m_logView   = nullptr;
    QLabel*          m_resultImg = nullptr;
    QLabel*          m_resultTitle = nullptr;   // === 每步结果查看:当前显示的是哪一步 ===
    QPlainTextEdit*  m_stepDetail  = nullptr;   // === 每步结果查看:该步全部输出字段 ===
    QLineEdit*       m_imagePathEdit = nullptr;

    // === 每步结果查看:上次 Run 每个节点的输出 ===
    // 用 QUuid 索引而不是 OperatorNode* —— 节点删掉重建后地址会变,
    // 裸指针做 key 会留下指向已释放内存的 key,清缓存时直接崩。
    QHash<QUuid, QJsonObject> m_stepOutputs;
    QHash<QUuid, QString>     m_stepLabels;   // "第 3 步 · 颜色分割"
    int m_stepCacheFrame = 0;                // 这些结果来自第几帧(多帧时要说清)

    // === M6: 工具栏 Undo / Redo 按钮 ===
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;

    // === M6: 重入防护 ===
    // undo/redo 会改场景 → 触发 nodeMoved 等信号 → 可能又生成新 Command。
    // 这个标志让 undo/redo 期间产生的事件不落到栈里。
    bool m_applyingHistory = false;

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
