#include "pin_types.h"

namespace w7 {

QString pinTypeName(PinType t) {
    switch (t) {
        case PinType::Any:     return QStringLiteral("任意");
        case PinType::Image:   return QStringLiteral("图像");
        case PinType::Data:    return QStringLiteral("数据");
        case PinType::Text:    return QStringLiteral("文本");
    }
    return QStringLiteral("任意");
}

QString checkPinCompatibility(PinType outType, PinType inType,
                              bool toIsSource,
                              const QString& fromDisplay,
                              const QString& toDisplay,
                              bool selfConnect) {
    // 1) 自己连自己 —— 无意义的自环,而且会让拓扑排序的环检测报警
    if (selfConnect) {
        return QStringLiteral("不能把节点连到自己(自环会让流水线无法排序)");
    }

    // 2) 往图像源上连 —— 图像源没有输入口,它只负责"给图"
    if (toIsSource) {
        return QStringLiteral("“%1”是图像源,没有输入端口。\n"
                              "图像源只能放在流水线最前面,不能接在别的算子后面。")
            .arg(toDisplay);
    }

    // 3) 类型不符
    //    Any 一律放行:新算子还没声明类型时不能被拦住 ——
    //    "没写声明"应该意味着"不检查",而不是"拒绝"。
    if (outType != PinType::Any && inType != PinType::Any && outType != inType) {
        return QStringLiteral("类型不匹配:\n"
                              "  %1 输出的是「%2」\n"
                              "  %3 需要的是「%4」\n\n"
                              "连线不会自动转换类型,请换个算子或调整流水线顺序。")
            .arg(fromDisplay, pinTypeName(outType),
                 toDisplay,   pinTypeName(inType));
    }

    return {};   // 可以连
}

QString checkMultiplePredecessors(const QString& toDisplay,
                                  const QString& existingFromDisplay,
                                  const QString& newFromDisplay) {
    // 还没有前驱 —— 第一条线,永远放行
    if (existingFromDisplay.isEmpty()) return {};

    // 已经有一条了 —— 第二条要拦。
    //
    // 为什么不做"合并策略"(按字段优先级 / 允许用户选)而是直接拒绝:
    // 合并策略意味着"下游读到的值取决于用户没看见的某个设置",
    // 而 Run 引擎目前是**无状态**的,没有地方存这个选择。
    // 先拦住,让用户自己用下游显式表达(比如插一个 merge 算子),
    // 比偷偷替用户决定好。
    return QStringLiteral(
        "“%1”已经接了一条输入线,不能再接第二条。\n\n"
        "  已有:%2\n"
        "  新增:%3\n\n"
        "原因:本项目的 Run 引擎合并多个前驱时是「后写覆盖」——\n"
        "同一个输入口接两条线,下游读到的值取决于画布里连线的遍历顺序,\n"
        "而不是「谁先连的」。挪动一下节点位置就可能换一个答案。\n\n"
        "两个上游算子都会显示运行成功(状态点是绿的),不报错、不打日志,\n"
        "只有下游那个结果是错的 —— 到产线上就是漏检或误判。\n\n"
        "解决办法:\n"
        "  · 下游只接一条线,另一个结果用别的分支处理\n"
        "  · 或者拆成两条支路,分别接到各自的判定算子")
        .arg(toDisplay, existingFromDisplay, newFromDisplay);
}

} // namespace w7
