#pragma once
// ============================================================
// 板型配方(recipe):一块板的真值,从 JSON 读
//
// ## 为什么要从数据读而不是写在源码里
//
// 原来 pcb_inspect.cpp 里有一张 SITES[6],pcb_defect_judge.cpp 里另有
// 六处 `if (label == "R2")`。**这是同一份板型真值抄了两遍**,分居两个文件。
//
// 实测代价(2026-10-09,拿 W6 遗留的三种板型直接跑):
//   - 元件数恒等于 6,与图上有什么无关
//   - 三块**好板**全被判废(误报率 3/3)
//   - 两块废板的输出与好板**逐字相同**,等于没检
//
// 根因不是"坐标写死了",而是**规则也写死了** —— 换一块布局不同的板,
// 坐标不对,规则更是一条都跑不了。
//
// ## 这份 JSON 同时管住两件事
//
//   sites[].cx/cy/body_w/body_h  ->  元件**在哪**   (原来那张 SITES[6])
//   sites[].checks[]             ->  元件**查哪几条规则** (原来那七处 label==)
//
// 两件事必须在同一份数据里:分开存就会各自漂开,而漂开之后
// 上层看着"配好了",实际一条规则都没跑,还不报错。
//
// ## 配方不是源,是产物
//
// recipes/*.json 由 tools/export_recipes.py 从 W6 的生成器导出。
// **改配方要去改 W6 生成器再重跑导出脚本**,不要手改 JSON ——
// 手改就是抄副本,这个项目已经因为抄副本吃过四遍亏。
// ============================================================

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "pcb_core.h"

namespace w7 {

// 一条规则的名字。判定算子按这个字符串分派,不再认元件标签。
// ⚠ 这些字符串是**算子与配方之间的接口**:加一条规则要同时改两边,
//   所以每个名字都必须真有一条同名分支在跑 —— 见 kKnownChecks。
//
// ⚠ 2026-10-20:定义搬到 common/pcb_core/pcb_core.h 去了(那边不依赖 Qt,
//   W6 也要用同一份)。这里只是**转发**,不重新定义一遍 ——
//   同一批字符串写两遍,改一边忘一边就是下一次事故。
//
//   注意类型是 `const char*` 而不是 `const char[]`:
//   constexpr 数组**不能**拿另一个 constexpr 数组来初始化
//   (编译器报 C2131:表达式的计算结果不是常数)。
//   用指针就只是一个别名,调用方 `QString::fromUtf8(recipe_rule::kMissing)`
//   的写法一个字都不用改。
namespace recipe_rule {
inline constexpr const char* kMissing      = pcbcore::rule::kMissing;
inline constexpr const char* kShifted      = pcbcore::rule::kShifted;
inline constexpr const char* kWrong        = pcbcore::rule::kWrong;
inline constexpr const char* kPolarity     = pcbcore::rule::kPolarity;
inline constexpr const char* kBridge       = pcbcore::rule::kBridge;
inline constexpr const char* kInsufficient = pcbcore::rule::kInsufficient;
inline constexpr const char* kShort        = pcbcore::rule::kShort;
} // namespace recipe_rule

// 判定算子真正实现了的规则。配方里出现表以外的规则名 = 拼错了,
// 必须报错而不是"安静地不跑" —— 后者会让缺陷图看着像合格。
const QStringList& knownChecks();

struct RecipeSite {
    QString label;       // 位号,只用于显示和报告,判定不再依赖它
    QString kind;        // 封装类别(RES / CAP_SMALL / QFP64 ...)
    int cx = 0, cy = 0;  // 本体中心(参考分辨率下)
    int bodyW = 0, bodyH = 0;
    QStringList checks;      // 该元件身上要跑的规则
    double shiftTolPct = 0;  // shifted 容差(% ROI 宽/高);<=0 表示用算子默认
    bool hasShiftTol() const { return shiftTolPct > 0; }
    // 极性条应该在哪一侧("left"/"right",相对**元件本体**)。
    //
    // "反装"= 极性条挪到了另一边,而**哪边是对**取决于板的设计
    // (电容负极标左、二极管阴极标右)。判据不猜 —— 猜错就是把好板
    // 判成反装,那是静默误报,比漏检更难查。所以由配方给。
    //
    // 空串 = 这个元件没有极性条(陶瓷电容) -> polarity 规则不判定,
    // 但会在报告里报"没查",不能装作查过了。
    QString polaritySide;

    // 这个元件的端子(焊盘)位置,短路判据的唯一依据。
    //
    // 由**生成器**登记(画焊盘的那一行代码自己登记),配方带下来 ——
    // 和 polarity_side 同一个道理:期望侧由知道真相的那一方给,
    // 判据不猜(猜错就是把好板判成废板)。
    //
    // 空 = 没登记端子 -> short 规则判不了,会报"没查"。
    //
    // ⚠ 直接用 pcbcore::Pad,不另定义一个 Qt 版 —— 同一个东西两个结构体
    //   就是**抄副本**,本项目在这件事上栽过四遍。
    QVector<pcbcore::Pad> pads;
};

struct BoardRecipe {
    QString name;      // 板型标识,和文件名一致
    QString title;     // 给人看的板名
    int refW = 0, refH = 0;  // 标定时的参考分辨率
    int buffer = 10;          // ROI 在本体框四周多留的余量(参考分辨率下的像素)
    QVector<RecipeSite> sites;

    bool isEmpty() const { return sites.isEmpty(); }
};

// 从 JSON 对象读配方。失败时 reason 写成人话,由调用方决定怎么报。
//
// 严格程度:**少一个必填字段就报错,不做部分加载**。
// 部分加载最阴险 —— 加载出 5 个站点、另外 3 个静默丢了,
// 判定环节照样输出结论,只是少查了三块板。
bool parseRecipe(const QJsonObject& root, BoardRecipe* out, QString* reason);

// 从文件读配方。读不到 / 解析失败都返回 false。
// path 相对当前工作目录;工作目录由调用方保证(算子的既有约定)。
bool loadRecipe(const QString& path, BoardRecipe* out, QString* reason);

// 列出 recipes/ 目录下可用的板型,供界面下拉框用。
// 目录读不到就返回空列表 —— 这时界面自己构造一个"自定义路径"输入框,
// 不因为"没有列表"就让算子起不来。
QStringList listRecipes(const QString& dir = QStringLiteral("recipes"));

// Qt 结构体 -> 共享核心结构体。
//
// 只搬数据,不改语义。判据、校验规则全在核心那边。
// 放在这里是因为**转换也只留一份** —— 转换写两遍,改一边忘一边
// 同样是抄副本(这个项目栽过四遍)。
pcbcore::BoardRecipe toCoreRecipe(const BoardRecipe& r);

} // namespace w7