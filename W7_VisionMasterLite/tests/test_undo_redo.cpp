// ============================================================
// W7 M6 撤销/重做 — 无头回归测试
//
// 为什么需要它:
//   撤销重做的正确性依赖「按 UUID 找回节点」「方向锁防重入」
//   「Load 后清栈」这些状态转换,纯编译和启动都验不出来。
//   而手动点鼠标验 7 条清单既慢又不可重复 —— 改一行代码就得重点一遍。
//   这里把状态转换写成断言,跑一次 1 秒,改完立刻能回归。
//
// 不链接 OpenCV:本测试只碰 PipelineView / OperatorNode /
// ConnectionLine / Command,这几个文件本身不依赖 cv::。
// OperatorBase 仅通过 pipelineFromJson 的工厂 lambda 间接出现,
// 测试里传自己的 lambda,不 include operator_base.h。
//
// 运行:  w7_undo_test.exe
// ============================================================

#include <QApplication>
#include <QDebug>
#include <windows.h>   // SetConsoleOutputCP
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QStack>
#include <QUuid>

#include "pipeline_view.h"
#include "operator_node.h"
#include "connection_line.h"
#include "commands/node_commands.h"
#include "commands/edge_commands.h"

using namespace w7;

// ============================================================
// 极简断言(不引 QtTest,少一个依赖)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}

static void check(bool cond, const QString& what) {
    if (cond) {
        ++g_pass;
        qInfo().noquote() << "  [PASS]" << what;
    } else {
        ++g_fail;
        qWarning().noquote() << "  [FAIL]" << what << "  (case:" << g_case << ")";
    }
}

static void checkEq(int actual, int expected, const QString& what) {
    check(actual == expected,
          QString("%1 (期望 %2,实际 %3)").arg(what).arg(expected).arg(actual));
}

// ============================================================
// 模拟 MainWindow 的命令栈(测试不依赖 GUI 主窗口)
// MainWindow 里的 execUndo/execRedo 逻辑很薄(弹栈→undo→压栈),
// 这里复刻一份,保证测的是同一套语义。
// ============================================================
class History {
public:
    ~History() { clear(); }

    void exec(Command* cmd) {
        if (!cmd) return;
        cmd->exec();
        m_undo.push(cmd);
        m_redo.clear();
    }
    void undo() {
        if (m_undo.isEmpty()) return;
        Command* c = m_undo.pop();
        c->undo();
        m_redo.push(c);
    }
    void redo() {
        if (m_redo.isEmpty()) return;
        Command* c = m_redo.pop();
        c->redo();
        m_undo.push(c);
    }
    void clear() {
        qDeleteAll(m_undo); m_undo.clear();
        qDeleteAll(m_redo); m_redo.clear();
    }
    int undoDepth()  const { return m_undo.size(); }
    int redoDepth()  const { return m_redo.size(); }

private:
    QStack<Command*> m_undo;
    QStack<Command*> m_redo;
};

static OperatorNode* addPlain(PipelineView* cv, const QString& op,
                               const QPointF& pos) {
    return cv->addNode(op, op, "detection", pos);
}

// ============================================================
// 用例
// ============================================================

// 1. 增删节点的基本往返
static void test_add_delete() {
    beginCase("1. 增删节点往返");
    PipelineView cv;
    History h;

    checkEq(cv.nodes().size(), 0, "初始节点数");
    h.exec(new AddNodeCmd(&cv, "color_segment", "颜色分割", "detection", {100, 100}));
    checkEq(cv.nodes().size(), 1, "exec 后节点数");

    h.undo();
    checkEq(cv.nodes().size(), 0, "undo 后节点数");

    h.redo();
    checkEq(cv.nodes().size(), 1, "redo 后节点数");
    checkEq(h.undoDepth(), 1, "redo 后 undo 栈深度");
}

// 2. 关键回归:AddNodeCmd::redo 必须沿用原 UUID,
//    否则重做出来的节点和已有的连线对不上
static void test_add_redo_uuid_stable() {
    beginCase("2. AddNode redo 保持 UUID 稳定");
    PipelineView cv;
    History h;

    h.exec(new AddNodeCmd(&cv, "blob_count", "Blob", "detection", {200, 200}));
    const QUuid first = cv.nodes().first()->uuid();

    h.undo();
    checkEq(cv.nodes().size(), 0, "undo 后无节点");

    h.redo();
    checkEq(cv.nodes().size(), 1, "redo 后有节点");
    check(cv.nodes().first()->uuid() == first,
          "redo 出的节点 UUID 与原节点一致");
}

// 3. 核心用例:删除节点会连带删掉它的连线,撤销后线要回来
static void test_delete_restores_edges() {
    beginCase("3. 删节点连带删线,撤销后线恢复");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",  {200, 0});
    auto* c = addPlain(&cv, "blob_count",     {400, 0});
    cv.addConnection(a, b);
    cv.addConnection(b, c);
    checkEq(cv.connections().size(), 2, "初始连线数");

    h.exec(new DeleteNodeCmd(&cv, b));
    checkEq(cv.nodes().size(), 2, "删除中间节点后节点数");
    checkEq(cv.connections().size(), 0, "两条线都被连带删除");

    h.undo();
    checkEq(cv.nodes().size(), 3, "撤销后节点全部回来");
    checkEq(cv.connections().size(), 2, "撤销后连线全部恢复");
}

// 4. 删节点要保住参数(否则撤销后参数面板是空的)
static void test_delete_preserves_params() {
    beginCase("4. 删节点保留 params,撤销后还原");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "color_segment", {0, 0});
    const QUuid id = a->uuid();     // 先记下,后面 a 就被释放了
    QJsonObject p;
    p["h_min"] = 35;
    p["h_max"] = 85;
    a->setParams(p);

    h.exec(new DeleteNodeCmd(&cv, a));
    h.undo();

    checkEq(cv.nodes().size(), 1, "节点恢复");
    auto* back = cv.nodes().first();
    check(back->params().value("h_min").toInt() == 35, "h_min 参数还原");
    check(back->params().value("h_max").toInt() == 85, "h_max 参数还原");
    check(back->uuid() == id, "重建节点沿用原 UUID");
}

// 5. 危险序列:移动 → 删除 → 撤销删除 → 撤销移动
//    撤销删除会重建节点(新地址),MoveNodeCmd 靠 UUID 找回
static void test_move_then_delete_then_undo() {
    beginCase("5. 移动→删除→撤销删除→撤销移动(悬垂指针回归)");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "color_segment", {100, 100});
    const QUuid id = a->uuid();
    const void* oldAddr = a;

    h.exec(new MoveNodeCmd(&cv, id, {100, 100}, {500, 400}));
    check(a->pos() == QPointF(500, 400), "移动后位置正确");

    h.exec(new DeleteNodeCmd(&cv, a));
    checkEq(cv.nodes().size(), 0, "节点已删");

    h.undo();   // 重建节点
    checkEq(cv.nodes().size(), 1, "撤销删除后节点重建");
    auto* rebuilt = cv.nodes().first();
    check(rebuilt->uuid() == id, "重建节点沿用原 UUID");
    // 本用例的意义:确认「地址确实会变」这一前提成立,
    // 从而证明 MoveNodeCmd 存裸指针就是错的、必须靠 UUID 定位。
    qInfo().noquote() << "    (重建前后地址:" << oldAddr << "→" << (void*)rebuilt << ")";
    check(rebuilt->pos() == QPointF(500, 400), "重建后位置是移动后的位置");

    h.undo();   // 这一步若用裸指针就会访问已释放内存
    checkEq(cv.nodes().size(), 1, "撤销移动后节点仍在(未误删)");
    check(cv.nodes().first()->pos() == QPointF(100, 100), "位置回到移动前");
}

// 6. 连线的增删往返
static void test_edge_roundtrip() {
    beginCase("6. 连线增删往返");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",  {200, 0});
    checkEq(cv.connections().size(), 0, "初始连线数");

    h.exec(new AddEdgeCmd(&cv, a->uuid(), b->uuid()));
    checkEq(cv.connections().size(), 1, "exec 后有连线");

    h.undo();
    checkEq(cv.connections().size(), 0, "undo 后无连线");

    h.redo();
    checkEq(cv.connections().size(), 1, "redo 后连线恢复");
    check(cv.connections().first()->fromNode() == a, "线的起点正确");
    check(cv.connections().first()->toNode() == b, "线的终点正确");
}

// 7. RemoveEdgeCmd 往返
static void test_remove_edge_roundtrip() {
    beginCase("7. 单独删连线往返");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "a", {0, 0});
    auto* b = addPlain(&cv, "b", {200, 0});
    cv.addConnection(a, b);
    checkEq(cv.connections().size(), 1, "初始连线数");

    h.exec(new RemoveEdgeCmd(&cv, a->uuid(), b->uuid()));
    checkEq(cv.connections().size(), 0, "exec 后无连线");

    h.undo();
    checkEq(cv.connections().size(), 1, "undo 后连线恢复");
}

// 8. 新操作应清空 redo 栈(标准 Command 语义)
static void test_redo_cleared_by_new_op() {
    beginCase("8. 新操作清空 redo 栈");
    PipelineView cv;
    History h;

    h.exec(new AddNodeCmd(&cv, "n1", "N1", "detection", {0, 0}));
    h.undo();
    checkEq(h.redoDepth(), 1, "undo 后 redo 栈有 1 个");

    h.exec(new AddNodeCmd(&cv, "n2", "N2", "detection", {200, 0}));
    checkEq(h.redoDepth(), 0, "新操作后 redo 栈被清空");
}

// 9. 命令在节点已消失时应静默 no-op,不能崩
//    (Load 新工程后按 Ctrl+Z 的场景)
static void test_command_survives_missing_node() {
    beginCase("9. 节点消失后命令静默 no-op");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "color_segment", {0, 0});
    h.exec(new MoveNodeCmd(&cv, a->uuid(), {0, 0}, {300, 300}));

    // 模拟 Load:画布清空
    cv.clearPipeline();
    checkEq(cv.nodes().size(), 0, "画布已清空");

    bool crashed = false;
    h.undo();     // 节点已不在,应 no-op
    h.redo();
    check(!crashed, "命令在节点缺失时未崩溃");
    checkEq(cv.nodes().size(), 0, "画布保持为空");
}

// 10. UUID 唯一性 —— Command 正确性的地基
static void test_uuids_unique() {
    beginCase("10. UUID 唯一性");
    PipelineView cv;
    QSet<QString> seen;
    for (int i = 0; i < 50; ++i) {
        addPlain(&cv, "same_op", {i * 10.0, 0});   // 全是同名节点
    }
    checkEq(cv.nodes().size(), 50, "创建 50 个同名节点");
    for (auto* n : cv.nodes()) {
        seen.insert(n->uuid().toString());
    }
    checkEq(seen.size(), 50, "50 个同名节点的 UUID 全部不同");
}

// 11. 同名节点的边能被 UUID 精确指认
//     (这正是旧的「op 名 + FIFO」方案会出错的场景)
static void test_same_name_edges_by_uuid() {
    beginCase("11. 同名节点连线按 UUID 精确指认");
    PipelineView cv;

    auto* a1 = addPlain(&cv, "color_segment", {0, 0});
    auto* b  = addPlain(&cv, "blob_count",   {200, 0});
    auto* a2 = addPlain(&cv, "color_segment", {0, 300});   // 与 a1 同名

    cv.addConnection(a1, b);
    cv.addConnection(a2, b);
    checkEq(cv.connections().size(), 2, "两条线都建了");

    // 按 UUID 找回,不受同名干扰
    check(cv.findNodeByUuid(a1->uuid()) == a1, "找到 a1");
    check(cv.findNodeByUuid(a2->uuid()) == a2, "找到 a2");
    check(cv.findNodeByUuid(a1->uuid()) != a2, "a1 和 a2 未混淆");

    // 只删 a1,a2 的线不受影响
    History h;
    h.exec(new DeleteNodeCmd(&cv, a1));
    checkEq(cv.nodes().size(), 2, "删 a1 后剩 2 个节点");
    checkEq(cv.connections().size(), 1, "只剩 a2 的线");

    h.undo();
    checkEq(cv.connections().size(), 2, "撤销后两条线都回来");
}

// 12. clearPipeline 后不残留悬垂指针
static void test_no_dangling_after_clear() {
    beginCase("12. clearPipeline 不留悬垂指针");
    PipelineView cv;
    auto* a = addPlain(&cv, "a", {0, 0});
    auto* b = addPlain(&cv, "b", {200, 0});
    cv.addConnection(a, b);

    cv.clearPipeline();
    checkEq(cv.nodes().size(), 0, "节点列表已清空");
    checkEq(cv.connections().size(), 0, "连线列表已清空");
    check(cv.scene()->items().isEmpty(), "scene 已无残留图元");
}

// ============================================================
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");   // headless
    // Windows 控制台默认 GBK,中文断言信息会乱码 —— 强制 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 M6 撤销/重做 回归测试";

    test_add_delete();
    test_add_redo_uuid_stable();
    test_delete_restores_edges();
    test_delete_preserves_params();
    test_move_then_delete_then_undo();
    test_edge_roundtrip();
    test_remove_edge_roundtrip();
    test_redo_cleared_by_new_op();
    test_command_survives_missing_node();
    test_uuids_unique();
    test_same_name_edges_by_uuid();
    test_no_dangling_after_clear();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看出结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
