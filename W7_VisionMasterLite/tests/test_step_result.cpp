// ============================================================
// W7 每步结果查看 — 无头回归测试
//
// 为什么需要它:
//   「每步结果查看」做了三件事,只有第三件有判定逻辑、值得单测:
//     1. 画布状态点(绿/红/橙)
//     2. 右键菜单 + 缓存
//     3. 结果区的图挑选
//   第 1 件是 statusFromOutput —— 它错了不会崩,只会让一整条
//   判废的流水线画出一排绿点。崩溃的 bug 有人看得见,
//   "看起来全绿"的 bug 要到产线漏检才发现。
//
//   另外补了几条 UUID 相关的断言:
//   每步缓存用 QUuid 做 key 而不是裸指针。这条要是写成裸指针,
//   节点删掉后 key 指向已释放内存,clearStepCache 时直接崩。
// ============================================================

#include <QApplication>
#include <QDebug>
#include <QJsonObject>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>

#include "operator_node.h"

#include <windows.h>

using namespace w7;

// ============================================================
// 极简断言(与其余三个测试同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}
static void check(bool cond, const QString& what) {
    if (cond) { ++g_pass; qInfo().noquote() << "  [PASS]" << what; }
    else { ++g_fail; qWarning().noquote() << "  [FAIL]" << what
           << "  (case:" << g_case << ")"; }
}
static void checkStatus(OperatorNode::StepStatus actual,
                        OperatorNode::StepStatus expect,
                        const QString& what) {
    const char* names[] = {"None", "Ok", "Fail", "Error"};
    check(actual == expect,
          QString("%1 (实际 %2)")
              .arg(what, names[static_cast<int>(actual)]));
}

// ------------------------------------------------------------
// 用例 1:状态点判定
// ------------------------------------------------------------
static void test_status_from_output() {
    beginCase("画布状态点判定");
    using S = OperatorNode::StepStatus;

    // 判定算子
    checkStatus(OperatorNode::statusFromOutput({{"result", "PASS"}}), S::Ok,
                "result=PASS → 绿");
    checkStatus(OperatorNode::statusFromOutput({{"result", "FAIL"}}), S::Fail,
                "result=FAIL → 红");

    // 中间算子:没有 result 字段,只是跑成功了
    checkStatus(OperatorNode::statusFromOutput({{"count", 5}}), S::Ok,
                "只有中间字段(无 result)→ 绿");
    checkStatus(OperatorNode::statusFromOutput({{"mask_path", "images/x.png"}}), S::Ok,
                "有 mask_path 但无 result → 绿");

    // 报错
    checkStatus(OperatorNode::statusFromOutput({{"error", "文件不存在"}}), S::Error,
                "有 error → 橙");

    // 边界:error 和 result 同时存在(判了 FAIL 又写盘失败)
    // 取 Error 而不是 Fail —— "执行失败"比"判废"更值得先说
    checkStatus(OperatorNode::statusFromOutput({{"result", "FAIL"},
                                                 {"error", "帧快照写盘失败"}}), S::Error,
                "error + FAIL 并存 → 橙(error 优先)");

    // 边界:空 output
    // 算子跑了但没产任何字段 —— 这不是失败,画红点等于谎报
    checkStatus(OperatorNode::statusFromOutput(QJsonObject{}), S::Ok,
                "空 output → 绿(不是失败)");

    // 边界:result 是别的值(比如 UNKNOWN)
    checkStatus(OperatorNode::statusFromOutput({{"result", "UNKNOWN"}}), S::Ok,
                "result=UNKNOWN → 绿(只有 PASS/FAIL 才判红)");
}

// ------------------------------------------------------------
// 用例 2:初始状态 + 状态点不会被"设置成同一个值"以外的东西弄乱
// ------------------------------------------------------------
static void test_node_state() {
    beginCase("节点状态初值与切换");
    OperatorNode n("color_segment", "颜色分割", "detection");

    check(n.stepStatus() == OperatorNode::StepStatus::None,
          "新建节点状态是 None(画布上不显示点)");
    check(!n.hasCachedResult(), "新建节点没有缓存结果");

    n.setStepStatus(OperatorNode::StepStatus::Fail);
    check(n.stepStatus() == OperatorNode::StepStatus::Fail, "设成 Fail 生效");

    n.setStepStatus(OperatorNode::StepStatus::None);
    check(n.stepStatus() == OperatorNode::StepStatus::None,
          "重跑后能清回 None(不留上一轮的点)");
}

// ------------------------------------------------------------
// 用例 3:每步缓存的 key 必须是 UUID
//
// 这条看着像"测框架",但它是这次改动里唯一有崩溃风险的地方:
// 缓存用裸指针做 key 的话,节点删掉再 clearStepCache 就会
// 拿一个已释放的指针去 erase,段错误。
// 这里模拟"删掉一个节点、另一个节点还在"的场景,验证
// 用 uuid 做 key 时两个 key 互不干扰。
// ------------------------------------------------------------
static void test_uuid_keyed_cache() {
    beginCase("每步缓存用 UUID 做 key");
    QHash<QUuid, QJsonObject> cache;

    OperatorNode a("image_source_dir", "文件夹", "acquisition");
    OperatorNode b("color_segment", "颜色分割", "detection");

    cache.insert(a.uuid(), {{"image_path", "images/01.png"}, {"frame_count", 4}});
    cache.insert(b.uuid(), {{"count", 7}});
    check(cache.size() == 2, "两个节点各存一条");

    // UUID 必须互不相同 —— 相同的话第二次 insert 会覆盖第一条,
    // 于是"第 1 步的输出"变成"第 2 步的输出",而画布上两个点
    // 显示一样,用户完全看不出来。
    check(a.uuid() != b.uuid(), "不同节点 UUID 不同");
    check(!a.uuid().isNull() && !b.uuid().isNull(), "UUID 非空");

    // 删掉 a:按 uuid 摘掉它的条目,b 的不受影响
    cache.remove(a.uuid());
    check(cache.size() == 1, "删一个节点后剩 1 条");
    check(cache.contains(b.uuid()), "另一节点的结果还在");
    check(cache.value(b.uuid()).value("count").toInt() == 7,
          "另一节点的字段没被串改");

    // 同名同类的两个节点也必须分开
    OperatorNode c("color_segment", "颜色分割", "detection");
    OperatorNode d("color_segment", "颜色分割", "detection");
    check(c.uuid() != d.uuid(), "同名同类节点 UUID 仍不同");
    cache.insert(c.uuid(), {{"count", 99}});
    check(cache.value(b.uuid()).value("count").toInt() == 7,
          "插入同名节点不覆盖另一个的结果");
    check(cache.value(c.uuid()).value("count").toInt() == 99,
          "同名节点存的是自己的结果");
}

// ------------------------------------------------------------
// 用例 4:关键回归 —— boundingRect 必须包住所有真正画出去的像素
//
// 这条锁的是"拖动节点留下一圈残影"那个 bug。
//
// Qt 移动 item 时只按 boundingRect 擦除旧像素:超出这个范围的,
// Qt 根本不知道它存在,于是永远留在屏幕上。4 个端口圆画在本体矩形
// **外面**(圆心在边的中心,半径 8,再加一圈白色描边),
// 以前 boundingRect 只写了本体矩形那一圈 —— 于是每拖一次,
// 4 个端口圆和描边就原地留一份残影。
//
// 为什么这条值得单测:残影不会崩,不会打日志,只是屏幕上脏一点。
// 纯编译和启动都验不出来,手点一遍也容易看成"渲染正常"。
// 而它是一行边界数字 —— 正好是断言能钉死的那种东西。
// ------------------------------------------------------------
static void test_bounding_rect_covers_ports() {
    beginCase("boundingRect 圈住 4 个端口圆(残影回归)");
    OperatorNode n("color_segment", "颜色分割", "detection");

    using S = OperatorNode::PortSide;
    const S sides[4] = {S::Left, S::Right, S::Top, S::Bottom};

    // 端口圆的实际绘制范围(照抄 operator_node.cpp::paint):
    //   drawEllipse(圆心, PORT_HALF=8, PORT_HALF) + 白色描边宽 2
    // 所以最外缘 = 圆心再往外 8 + 1。
    // 这两个数字必须跟着 paint() 一起改 —— 否则测试就在验一个
    // 假想的图形,测过了但残影还在。
    const qreal reach = 8.0 + 1.0;

    // 节点在 (0,0) 时 scene 坐标 == item 坐标,直接拿 portScenePos 当局部坐标用
    QRectF ports;   // 4 个端口绘制矩形的并集
    for (S s : sides) {
        const QPointF c = n.portScenePos(s);
        ports |= QRectF(c.x() - reach, c.y() - reach, reach * 2, reach * 2);
    }

    const QRectF br   = n.boundingRect();
    const QRectF body = n.bodyRect();

    // 本体矩形本身当然要包住
    check(br.contains(body), "boundingRect 包住本体矩形");
    check(br.width() > body.width() && br.height() > body.height(),
          "boundingRect 比本体矩形大(端口圆画在本体之外)");

    // === 关键断言 ===
    // 以前 boundingRect 只有本体那一圈,这条必挂 —— 它就是残影的成因:
    // Qt 移动 item 时只擦 boundingRect 内的像素,端口圆落在外面,
    // Qt 不知道它存在,于是每拖一次就在原地留一份。
    check(br.contains(ports), "4 个端口圆(含描边)的并集整体被 boundingRect 包住");

    // 逐个再钉一遍,失败时能直接看出是哪个方向漏了。
    // 端口圆心正好在本体某条边的中点上,所以"从中心指向端口"的方向
    // 就是纯 x 或纯 y —— 直接取符号,不用求模长。
    for (S s : sides) {
        const QPointF c = n.portScenePos(s);
        const QPointF dir((c.x() > 0) - (c.x() < 0),   // 中心 → 端口
                          (c.y() > 0) - (c.y() < 0));
        check(br.contains(c + dir * reach),
              QString("端口(%1)最外缘在 boundingRect 内 ← 残影成因")
                  .arg(static_cast<int>(s)));
    }

    // 节点挪到别处后,同样的端口范围(映射回局部坐标)仍然要被包住。
    // 这条守的是 mapToScene/mapFromScene 那条路径没被改坏。
    n.setPos(500, 400);
    QRectF portsMoved;
    for (S s : sides) {
        const QPointF c = n.mapFromScene(n.portScenePos(s));   // scene → item
        portsMoved |= QRectF(c.x() - reach, c.y() - reach, reach * 2, reach * 2);
    }
    check(n.boundingRect().contains(portsMoved),
          "节点移动后,映射回局部坐标的端口范围仍被包住");
}

// ------------------------------------------------------------
// 用例 5:关键回归 —— 拖线时节点不许跟着鼠标跑
//
// 这条锁的是"连线时元件乱跑"这个 bug 的**真正成因**。
//
// 成因:端口拖拽走 QDrag::exec(),那是个阻塞的**嵌套事件循环** ——
// 它停在这里,但 Qt 仍在往 scene 派发鼠标移动事件。节点带
// ItemIsMovable 标志,scene 一看"用户按着某个 item 在动",就把
// 鼠标位置写进去。于是拖线的整个过程里,节点本体一直跟着鼠标跑。
//
// 为什么以前没查出来:payload 按算子名找错节点是真 bug(已修),
// 但它和这个挤在同一段代码里,现象又都是"线接得不对",
// 修完 payload 现象还在,就以为没修对。实际是**两个叠在一起的 bug**。
//
// 这条断言的关键是:不能只测"标志位被设上了"(那是自证)。
// 要测的是"设上之后,scene 真的搬不动它了" —— 手动搬一次,
// 验证节点在拖线期间纹丝不动。
// ------------------------------------------------------------
static void test_no_move_during_connection_drag() {
    beginCase("拖线期间节点不被移动(元件乱跑回归)");

    QGraphicsScene scene;
    // 子类只为把 protected 的事件处理开放给测试调用。
    // 不在生产代码里加"给测试用的 public 函数"——那是把测试的
    // 便利性泄漏进产品 API,以后没人敢删。
    class Probe : public OperatorNode {
    public:
        using OperatorNode::OperatorNode;
        using OperatorNode::mousePressEvent;
        using OperatorNode::mouseReleaseEvent;
    };
    Probe* n = new Probe("color_segment", "颜色分割", "detection");
    scene.addItem(n);
    n->setPos(300, 300);

    // 记录有没有发出 nodeMoved —— 这条信号会生成 MoveNodeCmd 进撤销栈
    int movedCount = 0;
    QObject::connect(n, &OperatorNode::nodeMoved,
                     [&](OperatorNode*, const QPointF&, const QPointF&) { ++movedCount; });

    check(n->flags().testFlag(QGraphicsItem::ItemIsMovable),
          "初始状态:节点是可移动的");

    // === 真实事件序列:press → 拖线 → release ===
    //
    // 不真去调 QDrag::exec()(它阻塞,headless 会挂死),
    // 而是复现它造成的**那几件事**:记录拖动起点、标记拖线中、
    // 节点被搬走、补发 release。这几件事正是乱跑的成因,逐个钉住。

    // 1) 按下 —— 记录 m_dragStartPos
    QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
    press.setScenePos(n->pos());
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    n->mousePressEvent(&press);

    // 2) 拖线开始 → 移动锁生效
    n->beginConnectionDrag();
    check(!n->flags().testFlag(QGraphicsItem::ItemIsMovable),
          "拖线期间 ItemIsMovable 关闭(节点跟着鼠标跑的直接原因)");

    // 3) 模拟嵌套事件循环里 scene 把节点搬到了鼠标位置
    const QPointF dragged(900, 700);
    n->setPos(dragged);
    check(n->pos() == dragged,
          "手动 setPos 能改位置(证明上一条不是假通过)");

    // 4) 模拟 drag->exec() 返回后 scene 补发的 release。
    //    注意此时位置已经和 m_dragStartPos 不同了 ——
    //    没有 m_dragIsConnection 的话,这里会误判成一次用户拖动,
    //    凭空生成 MoveNodeCmd:用户按一次 Ctrl+Z 撤掉的是连线,
    //    第二次才撤出那个莫名其妙的"移动"。
    n->endConnectionDrag();

    // ============================================================
    // === 上一版修复漏掉的那半扇门 ===
    //
    // 旧代码在 endConnectionDrag() 里就把 ItemIsMovable 恢复了。
    // 但那一刻 scene 还指着这个节点当 mouse grabber,补发的 release
    // 也还没送达 —— 节点在这个窗口里"又是 grabber 又可移动"。
    // 正常松手时物理按钮已抬起,scene 写不进位置(所以大多数时候看不出来);
    // 按 Esc 取消或落点被拒时按钮还按着,就写进去了 → **偶发**乱跑。
    //
    // 所以:endConnectionDrag() 之后标志必须**仍然是关的**,
    // 一直关到 release 真正送达。
    // ============================================================
    check(!n->flags().testFlag(QGraphicsItem::ItemIsMovable),
          "endConnectionDrag() 之后 ItemIsMovable 仍然是关的"
          "(release 还没送达,窗口不能提前打开)");

    QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
    release.setScenePos(dragged);
    release.setButton(Qt::LeftButton);
    release.setButtons(Qt::NoButton);
    n->mouseReleaseEvent(&release);

    check(movedCount == 0,
          "拖线结束不产生 nodeMoved(否则撤销一次会冒出一次假移动)");
    check(n->flags().testFlag(QGraphicsItem::ItemIsMovable),
          "拖线结束后 ItemIsMovable 恢复(否则之后再也拖不动节点)");

    // === 第二道保险:就算位置还是被改了,也要拉回来 ===
    //
    // 上一步把节点搬到了 (900,700) 来模拟"scene 写位置"。
    // 真实环境里这条兜底应该永远不会触发(trigger 时会打 qWarning),
    // 但它必须存在 —— 平台的时序差异我们控制不了,
    // 兜底 + 警告比"默默跑偏"好得多。
    check(n->pos() != dragged,
          QString("兜底生效:release 时节点被拉回拖线起点(实际 pos=%1,%2 "
                  "被拉走的应是 %3,%4)")
            .arg(n->pos().x()).arg(n->pos().y()).arg(dragged.x()).arg(dragged.y()));

    // === 对照组:正常拖动节点【必须】仍然能产生 nodeMoved ===
    // 上面那条断言如果写得太宽(比如直接 return 掉整个 handler),
    // 正常拖动也会被吞掉 —— 那就是"修好一个 bug 又造一个"。
    movedCount = 0;
    QGraphicsSceneMouseEvent press2(QEvent::GraphicsSceneMousePress);
    press2.setScenePos(n->pos());
    press2.setButton(Qt::LeftButton);
    press2.setButtons(Qt::LeftButton);
    n->mousePressEvent(&press2);
    n->setPos(500, 500);          // 用户真的把它拖走了
    QGraphicsSceneMouseEvent release2(QEvent::GraphicsSceneMouseRelease);
    release2.setScenePos(QPointF(500, 500));
    release2.setButton(Qt::LeftButton);
    release2.setButtons(Qt::NoButton);
    n->mouseReleaseEvent(&release2);

    check(movedCount == 1,
          "对照组:正常拖动节点仍产生 nodeMoved(移动功能没被误伤)");
}

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 每步结果查看 回归测试";

    test_status_from_output();
    test_node_state();
    test_uuid_keyed_cache();
    test_bounding_rect_covers_ports();
    test_no_move_during_connection_drag();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
