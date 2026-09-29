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

} // namespace w7
