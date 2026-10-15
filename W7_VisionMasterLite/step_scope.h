#pragma once
// ============================================================
// step_scope.h — 单步执行:「只跑到某个节点为止」
//
// ============================================================
// 为什么要这个文件
//
// 调参数是流程图编辑器用得最多的动作,而调参数时你真正想回答的问题是
// "**这一步**为什么给这个数",不是"整条链跑完为什么给这个数"。
// 改一个阈值 -> Run -> 等整条链跑完 -> 翻日志找是哪一步出的问题,
// 这个回路太长。这就是本项目"能跑通但不好用"的主要来源。
//
// 为什么是纯函数
//
// onRunClicked() 里已经有拓扑排序,而"只跑到某节点"看着像图算法,
// 其实**不需要另写一遍**:拓扑序的定义就保证"每个节点的所有上游都在
// 它前面",所以"跑到 target 为止的那个前缀"天然包含全部上游。
//
// 但这件事有个坑,而且是本项目栽过的坑:
// 拓扑序返回的是**裸指针**,而 Run 引擎特意在帧循环前把它们换成
// QPointer —— 因为帧循环是长循环,期间画布还是活的(右键删、拖节点、
// Load 工程),裸指针会在节点中途消失时变成 use-after-free,
// 表现是随机的卡死或闪退。
//
// 所以这里用 **索引**而不是指针做接口,让调用方拿着裸指针进来、
// 拿到索引出去,换成 QPointer 的那一步仍然留在引擎里 ——
// 纯函数自己碰不到指针,也就不会漏掉这个防护。
//
// 三个函数对应三个真问题:
//   stepScopePrefix   跑到哪为止(含不含 target)
//   describeStepScope 给用户看的说明(别只显示一个数字)
// ============================================================

#include <QList>
#include <QString>

namespace w7 {

// ============================================================
// 单步执行的范围 = 拓扑序的一个前缀
//
// order   : 完整拓扑序(已确保是有效拓扑序,调用方先做环检测)
// targetId: 目标节点的 uuid;不在 order 里时返回空
//
// 约定:**含** target 本身 —— 用户右键了它,就是要看它跑出来的结果,
// 停在它前面等于什么都没发生。
// ============================================================
inline QList<int> stepScopePrefix(const QList<QString>& order,
                                  const QString& targetId) {
    QList<int> idx;
    for (int i = 0; i < order.size(); ++i) {
        idx.append(i);
        if (order.at(i) == targetId) {
            return idx;   // 含 target,立刻返回
        }
    }
    return {};            // target 不在序里 —— 交给调用方报错,不要静默跑全链
}

// ============================================================
// 不变式自检:前缀里的每个节点,它的**全部**上游都必须也在前缀里
//
// 这是本函数真正的价值所在:上面 stepScopePrefix 只有一行,
// 凭"拓扑序的定义"论证它对,是**推演**不是实测。
// 这个函数把那条推演变成了可执行的检查 ——
// 拿一条**已知正确**的顺序进去,必须成立;拿一条顺序被打乱的进去,
// 必须报出来。
//
// 为什么值得单独钉:
// 万一哪天拓扑排序被改(比如为了"让判定算子先跑"而调整入队顺序),
// 前缀就不再自动包含全部上游了,而这个错误**不会崩、不报错** ——
// 下游拿到空的 nodeOutputs,读到的是"上一帧的残留"或空值,
// 于是又是一次静默失效。
//
// 返回 true = 所有上游都在范围内(可以放心单步跑)。
// ============================================================
inline bool stepScopeHasAllUpstream(
        const QList<QString>& order,
        const QList<int>&    scope,
        const QList<QPair<QString, QString>>& edges) {  // (fromId, toId)

    // edges 里出现、但 toId 在范围内的节点,它的 fromId 也必须在范围内
    for (const auto& e : edges) {
        const bool toInScope   = scope.contains(order.indexOf(e.second));
        const bool fromInScope = scope.contains(order.indexOf(e.first));
        if (toInScope && !fromInScope) {
            return false;   // 范围内某节点的上游在范围外 —— 前缀切错了
        }
    }
    return true;
}

// ============================================================
// 给用户看的说明 —— 别只弹一个 "3"
//
// 单步执行最忌讳只给个数字:用户点右键是想知道"我要看的这一步,
// 前面还有几步要跑"。这句话同时也是**范围声明**:
// 如果范围切错了(少了上游),这句话会先给出可核对的答案。
// ============================================================
inline QString describeStepScope(const QString& targetDisplay,
                                int totalNodes, int scopeSize) {
    if (scopeSize <= 0) {
        return QStringLiteral("找不到节点「%1」,无法单步执行。")
            .arg(targetDisplay);
    }
    const int skipped = totalNodes - scopeSize;
    if (skipped == 0) {
        return QStringLiteral("「%1」是最后一个节点,单步执行 = 完整运行。")
            .arg(targetDisplay);
    }
    return QStringLiteral("单步执行「%1」:本次只跑到它为止,"
                          "跳过后面的 %2 个节点(整条链共 %3 个)。")
        .arg(targetDisplay).arg(skipped).arg(totalNodes);
}

} // namespace w7
