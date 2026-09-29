#pragma once
// ============================================================
// Command — 撤销/重做系统的基础设施 (M6 撤销重做)
//
// 设计要点:
//   - 多态基类:每个具体操作(AddNode/DeleteNode/MoveNode/...)继承 Command
//   - 持有画布引用,在 undo/redo 时直接操作画布
//   - 构造时 = "initial state",exec/undo/redo 都是"应用一次状态变更"
//   - 不持有 UI 状态(高亮/选中);只持有业务状态(节点/边/参数)
//
// 用法(后续 Step M6-3 才接入现有 addNode/removeNode):
//   auto* cmd = new AddNodeCmd(canvas, "color_segment", "颜色分割", "detection", {150, 200});
//   mainWindow.execCommand(cmd);   // 执行 + push undoStack
//   ...
//   Ctrl+Z → mainWindow.execUndo();  // 从 undoStack 弹 → cmd->undo() → push redoStack
// ============================================================

#include <QString>

namespace w7 {

class PipelineView;   // 子类实现时才需要,这里只前置声明

class Command {
public:
    virtual ~Command() = default;

    // 执行命令(应用一次状态变更)
    virtual void exec() = 0;

    // 撤销命令(回到 exec 之前的状态)
    virtual void undo() = 0;

    // 重做命令(等价于再次 exec)
    // 默认实现 = undo() + exec()
    virtual void redo() { undo(); exec(); }

    // 命令描述(用于日志/调试),例如 "Add color_segment at (150,200)"
    virtual QString desc() const = 0;
};

} // namespace w7