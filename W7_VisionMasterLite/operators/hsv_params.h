// ============================================================
// 安全地从 params 里读一个 HSV 三元组
//
// ## 崩溃是怎么发生的(2026-10-09 变异实测,不是照文档猜的)
//
// `QJsonArray::operator[]` 有**两个重载**,安全性不一样:
//
//   非 const 数组的 a[i]  -> 返回 QJsonValueRef。数组为空时它指向一个
//                            空容器,再 toInt() 就是解引用空指针。
//                            实测 0xC0000005(2026-10-08,color_segment.cpp
//                            的 run();2026-10-09 同样的崩法在 pcb_inspect.cpp:83)。
//   const 数组的 a[i]     -> 返回 QJsonValue。越界时返回 Undefined,
//                            **不解引用任何东西**,安全。
//
// 变异验证(把下面这三行依次改坏,重跑 w7_pcb_test):
//   const 去掉 + 长度检查去掉   -> 0xC0000005 崩   <- 真正杀人的是这个组合
//   只去长度检查(const 还在)   -> 测试全绿,越界被 Undefined + isDouble 挡住
//   只去掉 isDouble            -> 3 条断言红(字符串那档)
//
// 结论:两个检查都得留着,但**起作用的是 const 数组**。
// 长度检查负责把报错写成人话("只有 0 个元素"而不是"第 1 个不是数字"),
// isDouble 检查负责挡字符串 —— 那个连 const 都挡不住,因为 toInt()
// 对字符串返回 0,而 0 是**合法阈值**,于是会安静地跑成全黑范围。
//
// ## 为什么要抽成共享头文件
//
// 同一个坑**已经漏过一次**。color_segment.cpp 修好之后,
// pcb_inspect.cpp:83 还在写 `lo[0].toInt(85)` —— 一样的崩法、一样的行。
// 这个项目里已经吃过两次"同一个数被抄了一份"的亏:
//   EXPECTS[6] 抄了一份 ROI 几何中心
//   lo[0] 抄了一份越界守卫
// 抄第三遍不叫省事,叫埋雷。守卫只有一个定义。
//
// ## 另一个决定:形状不对时报 error,**不补默认值**
//
// 补默认值看起来更"健壮",但那会让用户以为自己的阈值生效了,
// 实际跑的是默认阈值 —— 属于"配置看着对、结果全错",
// 比直接报错难查一个数量级。
// ============================================================
#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QString>

namespace w7 {

// QJsonValue::typeName() 是 Qt 5 的 API,Qt 6 里没了,所以自己映射一张表。
// 报错里写"是字符串"比写"isString"有用 —— 用户看的是自己写下的东西。
inline QString jsonTypeName(QJsonValue::Type t) {
    switch (t) {
        case QJsonValue::Null:      return QStringLiteral("null");
        case QJsonValue::Bool:      return QStringLiteral("布尔值");
        case QJsonValue::Double:    return QStringLiteral("数字");
        case QJsonValue::String:    return QStringLiteral("字符串");
        case QJsonValue::Array:     return QStringLiteral("数组");
        case QJsonValue::Object:    return QStringLiteral("对象");
        case QJsonValue::Undefined: return QStringLiteral("未定义");
    }
    return QStringLiteral("未知类型");
}

// 读 out[3]。失败时把原因写进 err,由调用方决定怎么报。
//
// 元素也逐个查类型:size() >= 3 只保证"够长",不保证"是数字"。
// QJsonValue::toInt() 对字符串返回 **0**,而 0 是合法阈值 ——
// 于是 "hsv_low": ["85","80","80"] 会安静地变成 (0,0,0),
// 界面上看着是自己填的值,实际跑的是全黑范围。这类"配置看着对、
// 结果全错"只能靠 isDouble() 挡。
inline bool readHsvTriple(const QJsonValue& v, const QString& key,
                          int out[3], QString* err) {
    if (!v.isArray()) {
        *err = QString("%1 不是数组(实际是%2)").arg(key, jsonTypeName(v.type()));
        return false;
    }
    const QJsonArray a = v.toArray();
    if (a.size() < 3) {
        *err = QString("%1 只有 %2 个元素,至少要 3 个(H/S/V)").arg(key).arg(a.size());
        return false;
    }
    for (int i = 0; i < 3; ++i) {          // const 数组的 operator[] 返回 QJsonValue,不越界
        if (!a[i].isDouble()) {
            *err = QString("%1 第 %2 个不是数字(实际是%3)")
                     .arg(key).arg(i + 1).arg(jsonTypeName(a[i].type()));
            return false;
        }
        out[i] = a[i].toInt();
    }
    return true;
}

// 读一个标量阈值(H 下界、S 下界、倍数系数这类)。
//
// 单独写一个而不是硬套 readHsvTriple:那套守卫是按"数组里可能塞进
// 字符串"设计的,而标量这边真正要挡的是**另一种**坑 ——
// toInt() 对字符串和对未定义值都返回 0,而 0 在多数阈值里是合法值
// (mask_v_mul=0 会让掩膜变全开,比报错更难查)。
//
// 同样遵守上面那条规矩:形状不对就报错,**不补默认值**。
inline bool readHsvScalar(const QJsonValue& v, const QString& key,
                          double* out, QString* err) {
    if (!v.isDouble()) {
        *err = QString("%1 不是数字(实际是%2)").arg(key, jsonTypeName(v.type()));
        return false;
    }
    *out = v.toDouble();
    return true;
}

} // namespace w7