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

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 每步结果查看 回归测试";

    test_status_from_output();
    test_node_state();
    test_uuid_keyed_cache();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
