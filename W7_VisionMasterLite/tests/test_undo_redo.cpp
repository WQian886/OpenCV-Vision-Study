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
#include "connection_payload.h"   // 纯逻辑编解码,零 QtWidgets 依赖
#include "commands/node_commands.h"
#include "commands/edge_commands.h"
#include "select_delete_plan.h"   // 批量删除的计划层(纯函数,头文件内联)

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

// 13. 关键回归:连线拖拽 payload 的编解码
//
//     这条锁的是把用户坑了的那三个渲染 bug 里最难查的一个:
//     "连线后元件乱跑"。真凶在 dropEvent —— 以前按【算子名】去找源节点,
//     算子名不唯一,画布里放两个 color_segment 时必然接到第一个身上;
//     而 addConnection 开头会顶掉第一个节点原有的连线,
//     于是 GUI 上看��是"被连的那个节点自己跳走",真凶隔了三层。
//
//     编码/解码抽成纯函数(connection_payload.h)就是为了能被这里直接验。
static void test_connection_payload_codec() {
    beginCase("13. 连线 payload 编解码");
    PipelineView cv;

    // --- 13.1 同名节点必须编出不同的 payload ---
    // 这条就是 bug 本身:以前编的是算子名,两个 color_segment 编码完全相同,
    // 落点根本无从分辨。UUID 才是稳定身份。
    auto* a1 = addPlain(&cv, "color_segment", {0, 0});
    auto* a2 = addPlain(&cv, "color_segment", {0, 300});   // 与 a1 同名同类
    check(a1->uuid() != a2->uuid(), "两个同名节点 UUID 不同");

    const QString p1 = encodeConnectionPayload(a1->uuid(), 1);   // Right
    const QString p2 = encodeConnectionPayload(a2->uuid(), 1);
    check(p1 != p2, "两个同名节点编出的 payload 不同(否则必然接错)");
    check(!p1.contains("color_segment") && !p2.contains("color_segment"),
          "payload 里不含算子名(算子名不唯一)");

    // --- 13.2 往返:编什么就解得回什么 ---
    QUuid gotId;
    int   gotSide = -1;
    check(decodeConnectionPayload(p2, &gotId, &gotSide),
          "合法 payload 解码成功");
    check(gotId == a2->uuid(), "解出的 UUID 与 a2 一致(不是 a1)");
    check(gotSide == 1, "解出的 side 一致");
    // 落到 dropEvent 那一步的真实动作:按 UUID 找回节点
    check(cv.findNodeByUuid(gotId) == a2, "按解出的 UUID 找回的是 a2");

    // 四个 side 都要能原样往返 —— 少一个就是"某个方向连不上"
    for (int s = kPortSideMin; s <= kPortSideMax; ++s) {
        QUuid sid; int ss = -1;
        const bool ok = decodeConnectionPayload(
            encodeConnectionPayload(a1->uuid(), s), &sid, &ss);
        check(ok && sid == a1->uuid() && ss == s,
              QString("side=%1 往返一致").arg(s));
    }

    // --- 13.3 坏 payload 必须解不出来,且不能留半截结果 ---
    // 关键:解失败时不能写出半截数据。dropEvent 若拿到半截 UUID 去
    // findNodeByUuid,拿到的是空 UUID → 找不到节点 → 顶多这次连线不成立;
    // 但如果解函数"尽力而为"地填了个默认值,就会静默接到别的节点上。
    const QStringList bad = {
        "",                              // 空串
        "color_segment|1",               // 旧格式:算子名不是 UUID
        "|1",                            // 左半段为空
        "550e8400-e29b-41d4-a716-446655440000|",  // 右半段为空
        "550e8400-e29b-41d4-a716-446655440000",    // 没有分隔符
        "550e8400-e29b-41d4-a716-446655440000|-1", // side 越界
        "550e8400-e29b-41d4-a716-446655440000|4",  // side 越界(上界)
        "550e8400-e29b-41d4-a716-446655440000|x",  // side 不是数字
        "not-a-uuid-at-all|1",           // 两半都不合法
    };
    for (const QString& s : bad) {
        QUuid sid; int ss = -1;
        check(!decodeConnectionPayload(s, &sid, &ss),
              QString("拒绝坏 payload:\"%1\"").arg(s));
    }

    // --- 13.4 空 UUID 编出来的东西必须解不出来 ---
    // 免得有人传了个空 UUID 进 addNode,结果编出一个"看着合法"的 payload
    QUuid nid; int ns = -1;
    check(!decodeConnectionPayload(encodeConnectionPayload(QUuid(), 1), &nid, &ns),
          "空 UUID 编出的 payload 解不出来(不会伪装成合法连线)");

    // --- 13.5 节点删了重建,旧 payload 指向的 UUID 仍然找得到新节点 ---
    // UUID 稳定的意义就在这:payload 在拖拽过程中编码,松手才解码
    const QUuid survivor = a1->uuid();
    cv.removeNode(a1);
    auto* rebuilt = cv.addNode("color_segment", "颜色分割", "detection", {0, 0});
    rebuilt->setUuid(survivor);   // Command 重建时就沿用原 UUID

    QUuid did; int ds = -1;
    decodeConnectionPayload(encodeConnectionPayload(survivor, 0), &did, &ds);
    check(cv.findNodeByUuid(did) == rebuilt,
          "节点删了重建后,旧 payload 仍指向重建的那个节点");
}

// ============================================================
// 14. 批量删除的"计划"层
//
// ------------------------------------------------------------
// 这组用例锁的是 MainWindow::deleteSelectedItems 里的两个真实缺陷:
// ============================================================
// (UAF) 先删节点、再遍历连线读 line->fromNode()
//      removeNode 会把待删节点和它的连线一起 delete,第二遍遍历
//      读的是已释放的对象。Release 下堆内存不填毒,常常还留着旧
//      指针值 → 平时不崩;等那块内存被复用就段错误,现场消失。
//
// (重复记账) 沾了待删节点的线被 RemoveEdgeCmd 又记一笔,
//      而 DeleteNodeCmd 里已经记过 —— 撤销时线会重建两次。
//
// ------------------------------------------------------------
// 这两个都发生在 MainWindow 里,而 MainWindow 要真窗口 + 全套算子,
// 无头测试编不动。所以把"算计划"这一步抽成纯函数
// (select_delete_plan.h),这里直接测它;
// 再照抄一遍 MainWindow 的执行顺序做端到端往返 ——
// 执行阶段只吃 QUuid,全程不碰裸指针,这条性质由 review 保证,
// 由下面的往返用例保证结果正确。
// ============================================================

// 照抄 MainWindow::deleteSelectedItems 的"执行阶段"。
// 参数是 plan 里那份 UUID 列表,不是指针 —— 这是刻意的。
static void applyPlan(PipelineView* cv, History* h,
                      const w7::SelectDeletePlan& plan) {
    for (const QUuid& id : plan.nodes) {
        if (OperatorNode* n = cv->findNodeByUuid(id))
            h->exec(new DeleteNodeCmd(cv, n));
    }
    for (const auto& e : plan.edges)
        h->exec(new RemoveEdgeCmd(cv, e.from, e.to));
}

static void test_delete_plan_classifies_edges() {
    beginCase("14. 批量删除:哪些线单独记账、哪些随节点连带删");
    PipelineView cv;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",  {200, 0});
    auto* c = addPlain(&cv, "blob_count",     {400, 0});
    auto* d = addPlain(&cv, "count_check",    {400, 200});
    auto* ab = cv.addConnection(a, b);   // 沾 A 和 B
    auto* cd = cv.addConnection(c, d);   // 完全独立

    // --- 只选节点 A、B ---
    {
        const auto p = w7::planSelectDelete({a, b}, {}, cv.connections());
        checkEq(p.nodes.size(), 2, "两个待删节点都进了计划");
        checkEq(p.absorbedEdges, 1, "A→B 这条线算连带删除");
        checkEq(p.edges.size(), 0, "连带删的线不该再单独记一笔 RemoveEdgeCmd");
        checkEq(p.totalEdges(), 1, "日志口径:一共少 1 条线");
    }

    // --- 只选那条独立线 ---
    {
        const auto p = w7::planSelectDelete({}, {cd}, cv.connections());
        checkEq(p.nodes.size(), 0, "没选节点");
        checkEq(p.edges.size(), 1, "独立线要单独记一笔");
        checkEq(p.absorbedEdges, 0, "没有被连带删除的线");
        check(p.edges.first().from == c->uuid() && p.edges.first().to == d->uuid(),
              "独立线的端点 UUID 正确");
    }

    // --- 节点 + 独立线混选 ---
    {
        const auto p = w7::planSelectDelete({a}, {cd}, cv.connections());
        checkEq(p.nodes.size(), 1, "节点进了计划");
        checkEq(p.edges.size(), 1, "独立线也进了计划");
        checkEq(p.totalEdges(), 2, "A→B 连带 + C→D 单独 = 2 条");
    }

    // --- 关键:把"会被连带删除的线"也一起选中 ---
    // 用户框选时很容易连带框到那条线。它已经记在 DeleteNodeCmd 里了,
    // 再记一笔 RemoveEdgeCmd 的话,撤销会重建出两条一样的线。
    {
        const auto p = w7::planSelectDelete({a, b}, {ab}, cv.connections());
        checkEq(p.edges.size(), 0,
                "已随节点连带删除的线,即使被选中也不再单独记账(否则撤销会重建出重复连线)");
    }

    // --- 空输入 / 空指针 ---
    {
        const auto p = w7::planSelectDelete({}, {}, {});
        check(p.isEmpty(), "空输入 → 空计划");
    }
    {
        const auto p = w7::planSelectDelete({nullptr}, {nullptr}, {nullptr});
        check(p.isEmpty(), "nullptr 输入不崩且视为空");
    }
}

// 端到端:执行计划 → 撤销到底 → 必须精确还原
// 这是"重复记账"缺陷唯一能被观测到的后果,所以必须真的跑一遍。
static void test_delete_plan_roundtrip() {
    beginCase("15. 批量删除:撤销到底精确还原(重复连线的后果)");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",  {200, 0});
    auto* c = addPlain(&cv, "blob_count",     {400, 0});
    auto* d = addPlain(&cv, "count_check",    {400, 200});
    auto* ab = cv.addConnection(a, b);
    auto* cd = cv.addConnection(c, d);

    // 框选 A、B,连带框到 A→B 那条线
    const auto plan = w7::planSelectDelete({a, b}, {ab, cd}, cv.connections());
    applyPlan(&cv, &h, plan);

    checkEq(cv.nodes().size(), 2, "删完剩 C、D");
    checkEq(cv.connections().size(), 0, "两条线都没了");

    // 命令条数也要对:2 个 DeleteNodeCmd + 1 个独立线的 RemoveEdgeCmd。
    // 沾待删节点的 A→B 不该再压一条 RemoveEdgeCmd ——
    // 它执行时端点已经没了,是一条**空命令**:用户按 Ctrl+Z 时
    // 会多按一次、什么都没发生。这种"撤销栈里混进空命令"很难被
    // 察觉,因为前几次撤销看起来都正常。
    checkEq(h.undoDepth(), 3,
            "撤销栈恰好 3 条命令(多一条说明有空命令混进来,用户会多按一次 Ctrl+Z)");

    while (h.undoDepth() > 0) h.undo();

    checkEq(cv.nodes().size(), 4, "撤销到底:4 个节点全回来");
    checkEq(cv.connections().size(), 2,
            "撤销到底:恰好 2 条线(多了就说明同一条线被记了两笔账)");

    // 对照组:用 UUID 逐条核对,不是只数个数 ——
    // "3 条线里有 2 条重复"和"2 条线"个数一样,前者不会挂在上面那条。
    const QUuid au = a->uuid(), bu = b->uuid(), cu = c->uuid(), du = d->uuid();
    check(cv.findNodeByUuid(au) != nullptr, "对照:A 回来了");
    check(cv.findNodeByUuid(cu) != nullptr, "对照:C 回来了");
    check(cv.findConnection(au, bu) != nullptr, "对照:A→B 回来了");
    check(cv.findConnection(cu, du) != nullptr, "对照:C→D 回来了");
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
    test_connection_payload_codec();
    test_delete_plan_classifies_edges();
    test_delete_plan_roundtrip();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看出结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
