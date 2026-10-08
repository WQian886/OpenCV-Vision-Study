#pragma once
// ============================================================
// 算子库分组 —— 把"每个算子都必须出现在某个分组里"变成可测的不变式
// ============================================================
//
// 以前的写法是硬编码一串 if/else 把 category 映射到分组节点:
//
//     if      (op.category == "acquisition")    parent = acquisition;
//     else if (op.category == "positioning")    parent = positioning;
//     ...                                        ← 一共 5 个分支
//     if (parent) { 建树项 }
//
// 而 registerOperators() 里写了 category = "dl" 的 dl_infer。
// 6 个分类只认 5 个 → parent 停在 nullptr → `if (parent)` 不成立 →
// **算子凭空消失**,左边算子库里根本看不到,连日志都不打一句。
//
// ============================================================
// 为什么这不只是"少写一个 else if"
// ============================================================
// 它的形态是:**新增分类不会报错,只会让那个分类下的算子静默消失。**
// 补上 dl 分支只是把这一次的症状压下去,下一个新分类还会再犯。
//
// 所以这里改成:**给定分类,要么落在已知分组,要么现建一个分组。**
// 无论传进来什么,没有算子会不见;现建出来的 key 会被回报给调用方,
// 让"有算子用了没命名的分类"这件事变成一条能看见的信息。
//
// 纯函数、头文件内联、只依赖 QtCore —— 不需要 QTreeWidget 就能测。
// ============================================================

#include <QList>
#include <QHash>
#include <QString>
#include <QStringList>

namespace w7 {

struct OperatorInfoLite {
    QString name;        // 英文名(注册到 OperatorBase::name)
    QString display;     // 中文显示
    QString category;
};

struct LibraryGroup {
    QString             key;      // category 原值
    QString             title;    // 分组显示名
    QList<QString>      operators;   // 算子英文名,按传入顺序
};

// ------------------------------------------------------------
// knownGroups —— 已知的 (category, 显示名) 对,顺序即显示顺序。
//   unknownCategories —— 出参:本次用到但不在 knownGroups 里的分类。
//   返回值           —— 按 knownGroups 顺序排好的分组;
//                       表里没用到的分组也保留(显示空的分组,
//                       对齐 VM 的固定模块布局)。
// ------------------------------------------------------------
inline QList<LibraryGroup> buildOperatorLibraryGroups(
        const QList<OperatorInfoLite>&           ops,
        const QList<QPair<QString, QString>>&    knownGroups,
        QStringList*                             unknownCategories) {

    if (unknownCategories) unknownCategories->clear();

    QList<LibraryGroup> out;
    QHash<QString, int> indexOfKey;
    for (const auto& kg : knownGroups) {
        LibraryGroup g;
        g.key   = kg.first;
        g.title = kg.second;
        indexOfKey.insert(g.key, out.size());
        out.append(g);
    }

    for (const auto& op : ops) {
        int at = indexOfKey.value(op.category, -1);
        if (at < 0) {
            // 未知分类:**现建一个分组**,而不是把算子丢掉。
            // 以前这一步是 `parent 停在 nullptr`,算子直接蒸发。
            if (unknownCategories && !unknownCategories->contains(op.category))
                unknownCategories->append(op.category);
            LibraryGroup g;
            g.key   = op.category;
            g.title = op.category;      // 没有显示名就用 key 顶上,至少看得见
            indexOfKey.insert(g.key, out.size());
            out.append(g);
            at = indexOfKey.value(op.category);
        }
        if (!op.name.isEmpty())
            out[at].operators.append(op.name);
    }
    return out;
}

} // namespace w7