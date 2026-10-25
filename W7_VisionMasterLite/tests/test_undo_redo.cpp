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
#include "pipeline_serialize.h"   // 存盘/读盘(存/读两端的端口方向)

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

// 7b. 单独删连线后撤销:端口方向必须还原
//
// 这组用例锁的缺陷:
//   M7 把"从哪个口拉出来的"存进了 AddEdgeCmd 和 DeleteNodeCmd,
//   但**漏了 RemoveEdgeCmd** —— 它的构造函数只收两个 UUID,
//   undo() 调的是 addConnection(from, to),方向全走默认值。
//
//   于是用户从 A 的**下边**拉出去、接到 B 的**上边**画的那条竖线,
//   单独选中按 Delete、Ctrl+Z 撤销回来,就变成一条横线了。
//
// 为什么这类 bug 特别难靠肉眼发现:
//   和存盘方向丢失(用例 16-19)是同一个毛病 —— 线还在、节点还在、
//   连线关系还在,只有走向变了。对照着截图一像素一像素比才看得出来。
//   而"删除一条线再撤销"这个操作序列,正常使用时谁会去多撤销一次?
//
// 这一条直接构造命令,测 RemoveEdgeCmd 自己;
// 方向是从 planSelectDelete 带过来的(7c 测那条链),所以这里显式传。
static void test_remove_edge_undo_restores_sides() {
    beginCase("7b. 单独删连线后撤销:竖线不能变成横线");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",    {100, 300});   // B 在 A 正下方

    const auto Bottom = OperatorNode::PortSide::Bottom;
    const auto Top    = OperatorNode::PortSide::Top;

    // 用户从 A 的下端口拉出、接到 B 的上端口 —— 一条竖线
    ConnectionLine* line = cv.addConnection(a, b, Bottom, Top);
    check(line != nullptr, "竖线建出来了");
    checkEq(int(line ? line->fromSide() : Bottom), int(Bottom), "建线时起点是 Bottom");
    checkEq(int(line ? line->toSide()   : Top),    int(Top),    "建线时终点是 Top");

    h.exec(new RemoveEdgeCmd(&cv, a->uuid(), b->uuid(), Bottom, Top));
    checkEq(cv.connections().size(), 0, "单独删掉这条线");

    h.undo();
    checkEq(cv.connections().size(), 1, "撤销后连线回来了");

    // 光看"回来了"不够 —— 一条横线也是"回来了"。
    // 必须断言它是原来那条竖线,而不是默认方向的新线。
    ConnectionLine* back = cv.connections().isEmpty() ? nullptr : cv.connections().first();
    check(back != nullptr, "拿得回撤销后那条线");
    if (back) {
        checkEq(int(back->fromSide()), int(Bottom),
                "撤销后起点仍是 Bottom(丢了就说明 undo 走了默认方向)");
        checkEq(int(back->toSide()),   int(Top),
                "撤销后终点仍是 Top(丢了就说明 undo 走了默认方向)");
    }
}

// 7c. 同一条链,走真实删除路径
//
// 7b 直接 new 命令,绕开了 MainWindow 真正会走的那条链:
//     框选线 -> planSelectDelete -> EdgeEndpoint -> RemoveEdgeCmd -> undo
//
// 方向要走过 **planSelectDelete 里的 EdgeEndpoint** 才到得了命令。
// 上一版缺陷其实是两处:
//     EdgeEndpoint 根本没带 side  /  RemoveEdgeCmd 根本没存 side
// 只测 RemoveEdgeCmd 会漏掉前一处,所以这条按真实路径再走一遍。
static void applyPlan(PipelineView* cv, History* h,
                      const w7::SelectDeletePlan& plan);   // 定义在下面,这里先要得到

static void test_remove_edge_undo_restores_sides_via_plan() {
    beginCase("7c. 走真实删除路径:竖线撤销回来还是竖线");
    PipelineView cv;
    History h;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",    {100, 300});

    const auto Bottom = OperatorNode::PortSide::Bottom;
    const auto Top    = OperatorNode::PortSide::Top;

    ConnectionLine* line = cv.addConnection(a, b, Bottom, Top);
    check(line != nullptr, "竖线建出来了");

    // 只框选线,不框节点 —— 这是"单独删一条连线"的用户动作
    const auto plan = w7::planSelectDelete({}, {line}, cv.connections());
    checkEq(plan.nodes.size(),  0, "没有待删节点");
    checkEq(plan.edges.size(),  1, "这条线走独立记账");

    // 方向必须被计划带走,否则后面还原不回去
    checkEq(int(plan.edges.first().fromSide), int(Bottom), "计划里带走了 Bottom");
    checkEq(int(plan.edges.first().toSide),   int(Top),    "计划里带走了 Top");

    applyPlan(&cv, &h, plan);
    checkEq(cv.connections().size(), 0, "删掉了");

    h.undo();
    checkEq(cv.connections().size(), 1, "撤销后回来了");

    ConnectionLine* back = cv.connections().isEmpty() ? nullptr : cv.connections().first();
    check(back != nullptr, "拿得回撤销后那条线");
    if (back) {
        checkEq(int(back->fromSide()), int(Bottom), "整条链走完,起点仍是 Bottom");
        checkEq(int(back->toSide()),   int(Top),    "整条链走完,终点仍是 Top");
    }
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
        h->exec(new RemoveEdgeCmd(cv, e.from, e.to, e.fromSide, e.toSide));
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

    // UUID 必须在**删除之前**抓下来。
    //
    // 这里原来写的是撤销到底之后才 `a->uuid()` —— 而 a 早就被
    // removeNode() delete 掉了,那是货真价实的 use-after-free。
    // 后果不是崩,是**测试自己偶发红**:实测 300 次里 24 次(8%)
    // 读到的 uuid 和真值不同(同一个二进制里两种读法对照,正确的 0/300,
    // 已释放的 24/300,数字完全对上)。
    //
    // 为什么偶发:分配器是否把 A 那块已释放内存复用给撤销时新建的节点,
    // 取决于当次的堆布局 —— 复用到了(92%)读到的还是对的,
    // 没复用(8%)就是别的内容。
    //
    // 所以这条断言挂的从来不是撤销逻辑的锅:**撤销每次都精确还原了**,
    // 节点 4 个、连线 2 条、uuid 逐字吻合。是尺子自己在漂。
    const QUuid au = a->uuid(), bu = b->uuid(), cu = c->uuid(), du = d->uuid();

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
    check(cv.findNodeByUuid(au) != nullptr, "对照:A 回来了");
    check(cv.findNodeByUuid(cu) != nullptr, "对照:C 回来了");
    check(cv.findConnection(au, bu) != nullptr, "对照:A→B 回来了");
    check(cv.findConnection(cu, du) != nullptr, "对照:C→D 回来了");
}

// ============================================================
// 16-19. 存盘 / 读盘:连线的两端方向
//
// ------------------------------------------------------------
// 这组用例锁的缺陷:
// ============================================================
//   v1.0 的存盘只记了"这条线连着谁和谁"(from / to 两个 UUID),
//   **没记它是从哪个口拉出来的**。加载时一律按默认的
//   右出 → 左进 重画。
//
//   所以:用户从上一个节点的**下边**拉出去、从下一个节点的**上边**接进来
//   画的那条竖线,存一次、再打开,就变成一条横线了 ——
//   而且不报错、不打日志,连图都照样显示。你只会觉得"这软件有点怪"。
//
// ------------------------------------------------------------
// 为什么这类 bug 特别难靠肉眼发现:
//   方向存错了之后,**线还在、节点还在、连线关系还在**,
//   只有走向变了。不对照着截图一像素一像素比,根本看不出来。
//   而"存盘 → 重开 → 变成另一个样子"是所有存盘 bug 里最常见的一类。
// ============================================================

// ------------------------------------------------------------
// 一个假的算子工厂。
// pipelineFromJson 不认识 OperatorBase(它只认名字和标签),
// 所以这里自己编一张注册表 —— 也正因为不 include operator_base.h,
// 这个测试目标才能保持"零 OpenCV"。
// ------------------------------------------------------------
static w7::OpDefaults fakeFactory(const QString& name) {
    struct Row { const char* n; const char* d; const char* c; };
    static const Row table[] = {
        {"image_source_dir", "文件夹批量图像", "acquisition"},
        {"color_segment",    "颜色分割",       "detection"},
        {"blob_count",       "Blob 计数",      "detection"},
        {"count_check",      "数量判定",       "detection"},
    };
    for (const auto& r : table)
        if (name == QLatin1String(r.n))
            return {true, QString::fromUtf8(r.d), QString::fromUtf8(r.c)};
    return {};   // 不认识 → known=false → 该节点**留在画布上**但标成「未安装」
}

static QStringList g_loadLog;   // 收集 pipelineFromJson 打的日志

static bool fakeLoad(PipelineView* cv, const QJsonDocument& doc, QString& err) {
    g_loadLog.clear();
    return pipelineFromJson(cv, doc,
        [](const QString& s){ g_loadLog.append(s); },
        fakeFactory, err);
}

// 把 JSON 里的 from_side / to_side 抹掉 —— 模拟 v1.0 的旧文件。
// 这既是"老文件兼容"用例的输入,也顺带当了对照组:
// 老文件加载出来的线确实是默认方向,和修复前的行为一模一样。
static QJsonDocument stripSides(const QJsonDocument& doc) {
    QJsonObject root = doc.object();
    QJsonArray edges;
    for (const auto& v : root["edges"].toArray()) {
        QJsonObject e = v.toObject();
        e.remove("from_side");
        e.remove("to_side");
        edges.append(e);
    }
    root["edges"] = edges;
    return QJsonDocument(root);
}

static QString sideName(w7::OperatorNode::PortSide s) {
    switch (s) {
        case w7::OperatorNode::PortSide::Left:   return "Left";
        case w7::OperatorNode::PortSide::Right:  return "Right";
        case w7::OperatorNode::PortSide::Top:    return "Top";
        case w7::OperatorNode::PortSide::Bottom: return "Bottom";
    }
    return "?";
}

// ------------------------------------------------------------
// 用例 16:竖线往返 —— 这就是 bug 的本体
// ------------------------------------------------------------
static void test_vertical_edge_survives_roundtrip() {
    beginCase("16. 竖线(Bottom→Top)存一次再打开,还是竖的");
    PipelineView cv;

    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",    {0, 200});   // 竖着摆
    cv.addConnection(a, b,
                     OperatorNode::PortSide::Bottom,   // 从 A 的**下边**拉出去
                     OperatorNode::PortSide::Top);      // 接进 B 的**上边**

    const QJsonDocument doc = pipelineToJson(&cv, "D:/imgs/pcb.png");

    // 文件里确实写下了方向
    const QJsonObject e0 = doc.object()["edges"].toArray().first().toObject();
    check(e0.contains("from_side") && e0.contains("to_side"),
          "边的 JSON 里带上了 from_side / to_side 两个字段");
    checkEq(e0.value("from_side").toInt(-1), 3, "from_side 存的是 Bottom(=3)");
    checkEq(e0.value("to_side").toInt(-1),   2, "to_side   存的是 Top(=2)");

    // --- 重新加载到一个空画布 ---
    PipelineView cv2;
    QString err;
    const bool ok = fakeLoad(&cv2, doc, err);
    check(ok, "读盘成功");
    checkEq(cv2.connections().size(), 1, "读回来还是 1 条线");

    ConnectionLine* back = cv2.connections().isEmpty() ? nullptr
                                                       : cv2.connections().first();
    check(back != nullptr, "取到读回来的那条线");
    if (back) {
        check(back->fromSide() == OperatorNode::PortSide::Bottom &&
              back->toSide()   == OperatorNode::PortSide::Top,
              QString("方向保住了:实际是 %1→%2(期望 Bottom→Top)")
                  .arg(sideName(back->fromSide()), sideName(back->toSide())));
    }

    // --- 对照组:同一个文件抹掉两个字段(= v1.0 旧文件)---
    // 老文件加载出来的线一定是默认的 右出→左进,和修复前完全一致。
    // 这条要是也过了,说明我的断言根本测不到东西(比如断言写反了)。
    PipelineView cvOld;
    QString errOld;
    fakeLoad(&cvOld, stripSides(doc), errOld);
    ConnectionLine* old = cvOld.connections().isEmpty() ? nullptr
                                                        : cvOld.connections().first();
    check(old != nullptr, "对照:旧文件也能加载");
    check(old && old->fromSide() == OperatorNode::PortSide::Right &&
               old->toSide()   == OperatorNode::PortSide::Left,
          "对照:没有方向字段时退回 右出→左进 —— 前提属实(旧文件确实会躺成横线)");
}

// ------------------------------------------------------------
// 用例 17:四个方向一个都不能串
// ------------------------------------------------------------
static void test_all_four_sides_roundtrip() {
    beginCase("17. 四种方向分别往返,不能串");
    struct Case { OperatorNode::PortSide from, to; };
    const Case cases[] = {
        {OperatorNode::PortSide::Right,  OperatorNode::PortSide::Left},
        {OperatorNode::PortSide::Bottom, OperatorNode::PortSide::Top},
        {OperatorNode::PortSide::Bottom, OperatorNode::PortSide::Left},
        {OperatorNode::PortSide::Right,  OperatorNode::PortSide::Top},
    };

    for (const auto& c : cases) {
        PipelineView cv;
        auto* a = addPlain(&cv, "image_source_dir", {0, 0});
        auto* b = addPlain(&cv, "color_segment",    {100, 100});
        cv.addConnection(a, b, c.from, c.to);

        PipelineView cv2;
        QString err;
        fakeLoad(&cv2, pipelineToJson(&cv, ""), err);

        const QString want = QString("%1→%2").arg(sideName(c.from), sideName(c.to));
        ConnectionLine* back = cv2.connections().isEmpty() ? nullptr
                                                          : cv2.connections().first();
        check(back && back->fromSide() == c.from && back->toSide() == c.to,
              QString("%1 往返不变").arg(want));
    }
}

// ------------------------------------------------------------
// 用例 18:parseSide 的边界 —— 尤其是"缺字段"不能被当成 Left
// ------------------------------------------------------------
//
// 这里埋着一个很容易写错的坑:
//
//     QJsonValue v;              // 字段不存在
//     v.toInt();                 // → 0
//     0 又是 Left 的合法编码     // → 结论:这是条 Left 线
//
// 也就是说,用 toInt() 判断"字段有没有"会把**所有缺字段的边**
// 全部误判成 Left 线。而 v1.0 的默认是 Right —— 老文件会被悄悄改形状。
// 所以必须先 isDouble()。这条断言就是钉这个坑的。
static void test_parse_side_edges() {
    beginCase("18. 方向字段的解析:缺失 / 越界 / 合法");

    const OperatorNode::PortSide defR = OperatorNode::PortSide::Right;
    const OperatorNode::PortSide defL = OperatorNode::PortSide::Left;

    // --- 缺失 ---
    const QJsonObject noField;
    check(parseSide(noField.value("from_side"), defR) == defR,
          "字段缺失 → 用 fallback(Right),不是被当成 0=Left");
    check(!noField.value("from_side").isDouble(),
          "对照:缺失字段 isDouble() 为 false(所以 toInt() 不能用来判存在)");

    // --- 合法值 0..3 ---
    check(parseSide(QJsonValue(0), defR) == OperatorNode::PortSide::Left,
          "0 → Left(即使 fallback 是 Right,合法的 0 也必须被尊重)");
    check(parseSide(QJsonValue(1), defL) == OperatorNode::PortSide::Right,
          "1 → Right");
    check(parseSide(QJsonValue(2), defL) == OperatorNode::PortSide::Top,
          "2 → Top");
    check(parseSide(QJsonValue(3), defL) == OperatorNode::PortSide::Bottom,
          "3 → Bottom");

    // --- 越界 / 类型不对:退回 fallback,但不能让整条边加载失败 ---
    check(parseSide(QJsonValue(99),   defR) == defR, "99(越界) → fallback");
    check(parseSide(QJsonValue(-1),   defL) == defL, "-1(越界) → fallback");
    check(parseSide(QJsonValue("Top"), defR) == defR,
          "写成字符串 → fallback(不崩就行)");

    // --- 反向方向:老文件必须保持"右出→左进",不能变成 Left→Left ---
    PipelineView cv;
    auto* a = addPlain(&cv, "image_source_dir", {0, 0});
    auto* b = addPlain(&cv, "color_segment",    {200, 0});
    cv.addConnection(a, b);   // 用默认参数画一条横线

    PipelineView cvOld;
    QString err;
    fakeLoad(&cvOld, stripSides(pipelineToJson(&cv, "")), err);
    ConnectionLine* old = cvOld.connections().isEmpty() ? nullptr
                                                        : cvOld.connections().first();
    check(old && old->fromSide() == OperatorNode::PortSide::Right &&
               old->toSide()   == OperatorNode::PortSide::Left,
          "老文件的横线仍然是 右出→左进(没被误判成 Left→Left)");
}

// ------------------------------------------------------------
// 用例 19:搬出 mainwindow 之后,读盘该做的事一样没少
// ------------------------------------------------------------
// 这次为了能无头测,把 pipelineToJson / pipelineFromJson 从 mainwindow.cpp
// 搬到了独立文件。搬文件本身是有风险的 —— 顺手把"读盘没退化"
// 一起钉住:老格式(按 op 名 FIFO 匹配)、未知算子**留在画布上**、参数/UUID/位置还原。
// ------------------------------------------------------------
static void test_load_still_full_featured() {
    beginCase("19. 搬出 mainwindow 之后,读盘没退化");
    PipelineView cv;
    auto* a = addPlain(&cv, "image_source_dir", {11, 22});
    auto* b = addPlain(&cv, "color_segment",    {333, 444});
    a->setParams(QJsonObject{{"threshold", 42}});
    const QUuid bu = b->uuid();
    cv.addConnection(a, b, OperatorNode::PortSide::Right, OperatorNode::PortSide::Left);

    const QJsonDocument doc = pipelineToJson(&cv, "D:/imgs/a.png");
    check(doc.object().value("version").toString() == "1.1",
          "格式版本标成 1.1(老文件读的时候没人查这个字段,只是留个记录)");
    check(doc.object().value("image_path").toString() == "D:/imgs/a.png",
          "image_path 存下来了");

    // --- 正常路径 ---
    PipelineView cv2;
    QString err;
    check(fakeLoad(&cv2, doc, err), "读盘成功: " + err);
    checkEq(cv2.nodes().size(), 2, "2 个节点");
    checkEq(cv2.connections().size(), 1, "1 条线");

    OperatorNode* backB = cv2.findNodeByUuid(bu);
    check(backB != nullptr, "按 UUID 找回原来的 B(同名节点也不会指错)");
    if (backB) {
        checkEq(int(backB->pos().x()), 333, "位置 x 还原");
        checkEq(int(backB->pos().y()), 444, "位置 y 还原");
    }
    OperatorNode* backA = cv2.connections().isEmpty() ? nullptr
                                                      : cv2.connections().first()->fromNode();
    check(backA && backA->params().value("threshold").toInt() == 42,
          "参数原样还原");

    // --- 未知算子:**留在画布上**,而且**标出来** ---
    //
    // ⚠ 2026-10-25 改了行为(欠账 18 加载期 / 欠账 19)。
    //   以前这里是:
    //       checkEq(cv3.nodes().size(), 2, "未知算子被跳过,只留下 2 个真节点");
    //   —— 画布上那个节点**直接没了**。
    //   后果:
    //     · 用户存的什么,画布上就**不是**什么(画布不忠实)
    //     · 指向它的边也跟着没 -> 链断成两截
    //     · 下游拿空输出 -> 默认值兜底 -> 数字正常 -> **静默判 PASS**
    //   一条**绿着**的用例,给 bug 发了通行证。
    //
    //   现在:节点**留着**(画布忠实度),而且**一眼看得出**它没装。
    QJsonObject root = doc.object();
    QJsonArray nodes = root["nodes"].toArray();
    QJsonObject ghost = nodes.first().toObject();
    ghost["op"] = "no_such_operator_xyz";
    nodes.append(ghost);
    root["nodes"] = nodes;

    PipelineView cv3;
    QString err3;
    check(fakeLoad(&cv3, QJsonDocument(root), err3),
          "含未知算子的文件仍能读(不是整个失败)");
    checkEq(cv3.nodes().size(), 3,
            "三个节点都在画布上(那个没装的**也在** —— 以前它是 2)");

    // 留着的还不够,**得看得出**它没装:
    // 一个灰扑扑的方块,用户会当成正常节点。
    bool markedMissing = false;
    for (OperatorNode* n : cv3.nodes())
        if (n && n->opName() == "no_such_operator_xyz" && n->isOperatorMissing())
            markedMissing = true;
    check(markedMissing, "那个节点被标成「未安装」(用户一眼看得出少了什么)");

    bool logged = false;
    for (const QString& s : g_loadLog) if (s.contains("no_such_operator_xyz")) logged = true;
    check(logged, "日志里点了那个算子的名字(不是静默丢弃)");

    // --- v1.0 老格式:from/to 存的是 op 名,没有 uuid 字段 ---
    QJsonObject legacy;
    legacy["version"] = "1.0";
    legacy["image_path"] = "";
    legacy["nodes"] = QJsonArray{
        QJsonObject{{"op","image_source_dir"},{"x",0},{"y",0}},
        QJsonObject{{"op","color_segment"},{"x",200},{"y",0}},
    };
    legacy["edges"] = QJsonArray{
        QJsonObject{{"from","image_source_dir"},{"to","color_segment"}},
    };
    PipelineView cv4;
    QString err4;
    check(fakeLoad(&cv4, QJsonDocument(legacy), err4),
          "v1.0 老文件(按 op 名匹配)仍能读:" + err4);
    checkEq(cv4.connections().size(), 1, "老文件的连线没丢");
    ConnectionLine* l4 = cv4.connections().isEmpty() ? nullptr
                                                     : cv4.connections().first();
    check(l4 && l4->fromSide() == OperatorNode::PortSide::Right &&
               l4->toSide()   == OperatorNode::PortSide::Left,
          "老文件的连线是默认 右出→左进");
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
    test_remove_edge_undo_restores_sides();
    test_remove_edge_undo_restores_sides_via_plan();
    test_redo_cleared_by_new_op();
    test_command_survives_missing_node();
    test_uuids_unique();
    test_same_name_edges_by_uuid();
    test_no_dangling_after_clear();
    test_connection_payload_codec();
    test_delete_plan_classifies_edges();
    test_delete_plan_roundtrip();
    test_vertical_edge_survives_roundtrip();
    test_all_four_sides_roundtrip();
    test_parse_side_edges();
    test_load_still_full_featured();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    // ASCII 摘要:控制台编码不确定时也能一眼看出结果
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
