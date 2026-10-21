// ============================================================
// param_read.h — 带类型检查地读旋钮
// ============================================================
//
// ------------------------------------------------------------
// 这条规则是什么
// ------------------------------------------------------------
//
// 一个旋钮的值有三种来法,三种待遇**不一样**:
//
//   1. key 压根不在     → 用**声明的默认值**
//      用户没配,退回去是诚实的(defaultParams() 里写着那个值)。
//
//   2. key 在,类型对   → 用它
//
//   3. key 在,类型不对 → **报错**,不补默认值
//
// 第 3 种为什么不补默认值 —— 这是 operators/hsv_params.h 里
// 已经定下的规矩,不是本文件新发明的:
//
//   补默认值看起来更"健壮",但那会让用户以为自己的阈值生效了,
//   实际跑的是默认阈值 —— 属于「配置看着对、结果全错」,
//   比直接报错难查一个数量级。
//
// 缺了(第 1 种)退默认是合理的,配坏了(第 3 种)退默认是骗人的 ——
// 这就是为什么这两种情况必须分开。
//
// ------------------------------------------------------------
// 关键事实:不自己看类型,Qt 分不出来
// ------------------------------------------------------------
//
// 实测(2026-10-21,QJsonValue 行为探针,不是照文档猜的):
//
//   QJsonValue::toInt(默认值) 在**任何非整数**上都返回默认值:
//
//     整数 25        → 25
//     小数 25.7      → 默认值    ← **不截断**,整份丢掉
//     字符串 "100"   → 默认值    ← **不解析**!
//     字符串 "abc"   → 默认值
//     布尔 true      → 默认值
//     数组 / 对象    → 默认值
//     null / 缺失    → 默认值
//
// 也就是说,在 QJsonValue 眼里,"类型不对"和"缺失"是**同一件事** ——
// 都返回默认值,连"字符串 '100' 会被当成 100 解析"都不成立。
//
// 要把这两者分开,只能自己先 isDouble()。
// 而"缺失"退默认是合理的,"类型不对"退默认是骗人的 ——
// 所以这个检查非做不可,而且**必须自己写**。
//
// ⚠ 顺带一个反直觉的点:小数 25.7 给 toInt() 不是 25,是默认值。
//   也就是说"坐标给成小数"在旧代码里表现为"整个坐标被丢掉",
//   而不是"坐标被四舍五入"。差别很大:
//   前者算出来的是另一个点,后者算出来的是附近的点。
//
// ------------------------------------------------------------
// 为什么要单独一个文件
// ------------------------------------------------------------
//
// operators/hsv_params.h 里的 readHsvScalar / readHsvTriple 已经是
// 同一套规矩(类型不对就报错),但那套是按"HSV 数组/必填标量"设计的:
// 它们**不**处理"缺失"——缺了必须报错,因为那两个区间是**必填**旋钮。
//
// 标量旋钮(坐标、阈值、倍数、路径)是**可选**旋钮:缺了要能退默认值。
// 形状不一样,不能硬套 —— 所以另立一份。
//
// 但规矩只有一份:本文件的头注释和 hsv_params.h 说的是同一件事。
// 抄第三遍不叫省事,叫埋雷。
// ============================================================
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "hsv_params.h"   // jsonTypeName —— 报错里写"是字符串"比写 isString 有用

namespace w7 {

// ============================================================
// 旋钮该是什么类型
// ============================================================
//
// 三种旋钮,三种 JSON 形态:
//
//   Int    整数   JSON 里的数字,而且**得是整数**
//          25    ✓      25.7  ✗(会被整个丢掉)
//          "100" ✗      true  ✗
//
//   Double 小数   JSON 里的数字(整数也认 —— 25 就是 25.0)
//          0.9   ✓      25    ✓
//          "0.9" ✗      "abc" ✗
//
//   Text   文本   JSON 里的字符串
//          "abc" ✓      123   ✗      ""  ✓(空串是合法文本)
//
// ⚠ 为什么枚举只到三种、不搞"任意 JSON":
//   数组型旋钮(color_segment 的 hsv_low/hsv_high)走
//   hsv_params.h 的 readHsvTriple —— 那是**必填**旋钮,规矩不同。
//   可选旋钮里目前没有数组型的,将来要加,再往这里加第四种。
// ============================================================
enum class KnobKind {
    Int,
    Double,
    Text
};

// ============================================================
// 唯一判据:这个 JSON 值能不能当这个类型的旋钮用?
// ============================================================
//
// 单独抽出来,是因为**三个地方都要用同一个判据**:
//   · readKnob         —— 算子读旋钮时(类型不对 → 报错)
//   · knobValueMatches —— 面板显示旋钮时(类型不对 → 显示声明的默认值)
//   · 审计里的 3i      —— 源码层核对"声明的默认值"类型对不对
//
// 两边判据不一样的话,就会出现"面板显示 A、算子跑 B"——
// 那正是本项目栽过五遍的那个坑。
//
// ⚠ Int 的范围检查必须有:QJsonValue 能装下 1e300,
//   而 static_cast<int>(1e300) 是**未定义行为** ——
//   MSVC 上会得到 0x80000000(INT_MIN),不报错,直接算错。
// ============================================================
inline bool knobValueMatches(KnobKind kind, const QJsonValue& v) {
    switch (kind) {
    case KnobKind::Int: {
        if (!v.isDouble()) return false;
        const double d = v.toDouble();
        if (!(d >= -2147483648.0 && d <= 2147483647.0)) return false;   // 越界
        return d == static_cast<double>(static_cast<int>(d));            // 不是整数
    }
    case KnobKind::Double:
        return v.isDouble();
    case KnobKind::Text:
        return v.isString();
    }
    return false;
}

// ============================================================
// 说清"哪儿错了":点名旋钮 + 实际是什么类型
// ============================================================
//
// 报错必须**点名是哪个旋钮** ——
// "参数不合法"这句话等于没说,用户得自己一个一个试。
// ============================================================
inline QString knobTypeError(const QString& key, KnobKind kind, const QJsonValue& v) {
    switch (kind) {
    case KnobKind::Int:
        if (!v.isDouble())
            return QString("%1 不是数字(实际是%2)").arg(key, jsonTypeName(v.type()));
        if (!(v.toDouble() >= -2147483648.0 && v.toDouble() <= 2147483647.0))
            return QString("%1 超出 int 范围(实际是 %2)").arg(key).arg(v.toDouble());
        return QString("%1 不是整数(实际是 %2)").arg(key).arg(v.toDouble());
    case KnobKind::Double:
        return QString("%1 不是数字(实际是%2)").arg(key, jsonTypeName(v.type()));
    case KnobKind::Text:
        return QString("%1 不是文本(实际是%2)").arg(key, jsonTypeName(v.type()));
    }
    return QString("%1 类型不对").arg(key);
}

// ============================================================
// 读一个旋钮 —— 整个文件的核心
// ============================================================
//
//   缺失     → 退到 defaults 里声明的值(用户没配,退回去是诚实的)
//   类型不对 → 报错,点名是哪个旋钮
//   正常     → 用它
//
// ⚠ defaults 传进来而不是写死,是为了让**兜底值只有一份**:
//   以前每个算子在自己的 run() 里手写 `.toInt(XxxDefaults::kX1)`,
//   那是声明值的第二份;现在退路直接指向 defaultParams() ——
//   声明值改了,兜底跟着改,不用记得两处一起改。
//
// ⚠ 还有一种情况以前是**静默**的:key 既不在 params 也不在 defaults。
//   那不是"用户没配",是**这个旋钮压根没登记** ——
//   退成 0 会让算子拿着一个从没声明过的值在跑。
//   现在它也报错(措辞和"类型不对"不同,因为病因不同)。
// ============================================================
inline bool readKnob(const QJsonObject& params, const QJsonObject& defaults,
                     const QString& key, KnobKind kind,
                     QJsonValue* out, QString* err) {
    if (!params.contains(key)) {
        if (!defaults.contains(key)) {
            *err = QString("%1 既不在参数里,也不在声明的默认值里 —— "
                           "这个旋钮压根没登记,退成 0 只会让它拿着一个"
                           "从没声明过的值在跑").arg(key);
            return false;
        }
        const QJsonValue d = defaults.value(key);
        if (!knobValueMatches(kind, d)) {
            // 声明那一侧坏了 —— 和"用户配坏了"是两回事,措辞得分开。
            *err = QString("%1 的**声明默认值**类型不对(实际是%2)—— "
                           "这是声明那一侧坏了,不是参数配坏了").arg(key, jsonTypeName(d.type()));
            return false;
        }
        *out = d;
        return true;
    }

    const QJsonValue v = params.value(key);
    if (!knobValueMatches(kind, v)) {
        *err = knobTypeError(key, kind, v);
        return false;
    }
    *out = v;
    return true;
}

// ============================================================
// 读一组旋钮 —— 三个类型各一个
// ============================================================
//
// keys     旋钮名(必须和 defaultParams() 里登记的一模一样)
// defaults 算子自己的 defaultParams() —— key 缺失时退到它声明的值
// out      按 keys 的顺序拿到值
//
// 返回 false 时 *err 里是给人看的原因(点名是哪个 key、实际是什么类型),
// 调用方照着报。
//
// ⚠ 什么时候用"一组"、什么时候用"单个":
//   · 一组:旋钮多、类型一样,而且**键名本身是数据**
//     (五个测量算子的坐标、pcb_defect_judge 的 16 个判据阈值)。
//     键名放在一个 QStringList 常量里,算子和面板共用一份。
//   · 单个:旋钮少或者类型不一样(比如 image_source_camera 是
//     文本 + 小数 + 整数各一个)。单个的形式里**键名就写在调用处**,
//     读代码时一眼看得见"这个旋钮叫什么",不用去别处找清单。
//   两种形式共用同一个 readKnob —— 判据只有一处。
// ============================================================

// ---- 单个 ----
inline bool readIntKnob(const QJsonObject& params, const QJsonObject& defaults,
                        const QString& key, int* out, QString* err) {
    QJsonValue v;
    if (!readKnob(params, defaults, key, KnobKind::Int, &v, err)) return false;
    *out = v.toInt();
    return true;
}

inline bool readDoubleKnob(const QJsonObject& params, const QJsonObject& defaults,
                           const QString& key, double* out, QString* err) {
    QJsonValue v;
    if (!readKnob(params, defaults, key, KnobKind::Double, &v, err)) return false;
    *out = v.toDouble();
    return true;
}

inline bool readTextKnob(const QJsonObject& params, const QJsonObject& defaults,
                         const QString& key, QString* out, QString* err) {
    QJsonValue v;
    if (!readKnob(params, defaults, key, KnobKind::Text, &v, err)) return false;
    *out = v.toString();
    return true;
}

// ---- 一组 ----
inline bool readIntKnobs(const QJsonObject& params, const QStringList& keys,
                         const QJsonObject& defaults,
                         QVector<int>* out, QString* err) {
    out->clear();
    out->reserve(keys.size());
    for (const QString& key : keys) {
        QJsonValue v;
        if (!readKnob(params, defaults, key, KnobKind::Int, &v, err)) return false;
        out->append(v.toInt());
    }
    return true;
}

inline bool readDoubleKnobs(const QJsonObject& params, const QStringList& keys,
                            const QJsonObject& defaults,
                            QVector<double>* out, QString* err) {
    out->clear();
    out->reserve(keys.size());
    for (const QString& key : keys) {
        QJsonValue v;
        if (!readKnob(params, defaults, key, KnobKind::Double, &v, err)) return false;
        out->append(v.toDouble());
    }
    return true;
}

inline bool readTextKnobs(const QJsonObject& params, const QStringList& keys,
                          const QJsonObject& defaults,
                          QVector<QString>* out, QString* err) {
    out->clear();
    out->reserve(keys.size());
    for (const QString& key : keys) {
        QJsonValue v;
        if (!readKnob(params, defaults, key, KnobKind::Text, &v, err)) return false;
        out->append(v.toString());
    }
    return true;
}

// ============================================================
// 面板用:这个旋钮该显示什么?
// ============================================================
//
// 规则和 readKnob **对齐**(但更温和):
//   值类型对   → 显示它
//   值类型不对 → 显示**声明的默认值**
//   key 不在   → 调用方自己决定(现有的约定是"控件保持原样")
//
// 为什么类型不对时显示声明的默认值,而不是 0 / 空串:
//   0 和空串是**另一份**默认值,而且大多数旋钮的声明值不是 0 / 空串。
//   面板显示 0、算子按声明值跑 → 界面上摆的和实际跑的不是一回事。
//   用户在面板上看一眼不会觉得有问题,点一下"应用参数"就把 0 存进去了。
//
// 为什么对齐就够,不用也报错:
//   面板没有"报错"这个位置(它只有控件),所以它只能显示一个诚实的值。
//   真正该报错的是算子那侧 —— 那是 readKnob 的活。
// ============================================================
inline QJsonValue knobValueForWidget(const QJsonValue& v, const QJsonObject& defaults,
                                      const QString& key, KnobKind kind) {
    if (knobValueMatches(kind, v)) return v;
    const QJsonValue d = defaults.value(key);
    if (knobValueMatches(kind, d)) return d;
    // 声明值自己也不对 —— 面板只能空着。
    // (这种情况由审计的"声明的默认值类型对不对"那条兜着,不该走到这儿。)
    return QJsonValue();
}

// 三个类型的便捷包装 —— 判据都在 knobValueForWidget 里面,只有一处。
inline int knobIntForWidget(const QJsonValue& v, const QJsonObject& d, const QString& key) {
    return knobValueForWidget(v, d, key, KnobKind::Int).toInt();
}

inline double knobDoubleForWidget(const QJsonValue& v, const QJsonObject& d, const QString& key) {
    return knobValueForWidget(v, d, key, KnobKind::Double).toDouble();
}

inline QString knobTextForWidget(const QJsonValue& v, const QJsonObject& d, const QString& key) {
    return knobValueForWidget(v, d, key, KnobKind::Text).toString();
}

} // namespace w7
