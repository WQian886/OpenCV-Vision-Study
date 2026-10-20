#include "board_recipe.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace w7 {

const QStringList& knownChecks() {
    // 判定算子里真正有实现的规则。配方里写了表以外的,
    // 是拼错了或者算法还没写 —— 两种都必须报错。
    //
    // ⚠ 定义在 common/pcb_core/ 里(那边 W6 也要用),这里只是转成
    //   QStringList 给界面用。**不重新定义一遍** —— 同一批规则名
    //   写两处,改一边忘一边就是下一次事故。
    static QStringList kKnown;
    if (kKnown.isEmpty()) {
        kKnown.reserve(static_cast<int>(pcbcore::knownChecks().size()));
        for (const auto& c : pcbcore::knownChecks()) kKnown << QString::fromUtf8(c.c_str());
    }
    return kKnown;
}

namespace {

// ============================================================
// Qt 结构体 <-> 共享核心结构体
//
// 共享核心(common/pcb_core/)是**不依赖 Qt** 的,因为 W6 命令行项目
// 不引 Qt。W7 这边只做搬运:Qt 结构体 <-> 纯 C++ 结构体。
//
// 这层转换只搬数据,不碰语义 —— 判据、校验规则全在核心那边,
// 两边不可能各有一份。
// ============================================================

w7::RecipeSite toQtSite(const pcbcore::RecipeSite& s) {
    RecipeSite out;
    out.label  = QString::fromUtf8(s.label.c_str());
    out.kind   = QString::fromUtf8(s.kind.c_str());
    out.cx     = s.cx;
    out.cy     = s.cy;
    out.bodyW  = s.bodyW;
    out.bodyH  = s.bodyH;
    out.checks.clear();
    for (const auto& c : s.checks) out.checks << QString::fromUtf8(c.c_str());
    out.shiftTolPct = s.shiftTolPct;
    out.polaritySide = QString::fromUtf8(s.polaritySide.c_str());
    return out;
}

w7::BoardRecipe toQtRecipe(const pcbcore::BoardRecipe& r) {
    BoardRecipe out;
    out.name  = QString::fromUtf8(r.name.c_str());
    out.title = QString::fromUtf8(r.title.c_str());
    out.refW  = r.refW;
    out.refH  = r.refH;
    out.buffer = r.buffer;
    out.sites.clear();
    for (const auto& s : r.sites) out.sites.append(toQtSite(s));
    return out;
}

} // namespace

bool parseRecipe(const QJsonObject& root, BoardRecipe* out, QString* reason) {
    // 走核心那一个解析器。QJsonObject 先压成紧凑 JSON 文本再喂进去 ——
    // 这样"配方的格式"只有一处定义(核心的 mini_json + pcb_core)。
    // 本文件原来自己有一份 QJsonDocument 解析 + 字段校验,那是**第二份**,
    // 两份必然漂开(本项目已栽四遍)。
    const QByteArray compact = QJsonDocument(root).toJson(QJsonDocument::Compact);
    pcbcore::BoardRecipe core;
    std::string coreReason;
    if (!pcbcore::parseRecipeJson(compact.toStdString(), &core, &coreReason)) {
        if (reason) *reason = QString::fromUtf8(coreReason.c_str());
        return false;
    }
    *out = toQtRecipe(core);
    return true;
}

bool loadRecipe(const QString& path, BoardRecipe* out, QString* reason) {
    pcbcore::BoardRecipe core;
    std::string coreReason;
    if (!pcbcore::loadRecipeFile(path.toStdString(), &core, &coreReason)) {
        if (reason) *reason = QString::fromUtf8(coreReason.c_str());
        return false;
    }
    *out = toQtRecipe(core);
    return true;
}

QStringList listRecipes(const QString& dir) {
    QStringList out;
    QDir d(dir);
    if (!d.exists()) return out;
    const QStringList files = d.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& f : files) out.append(QDir(dir).filePath(f));
    out.sort();
    return out;
}

pcbcore::BoardRecipe toCoreRecipe(const BoardRecipe& r) {
    pcbcore::BoardRecipe out;
    out.name   = r.name.toStdString();
    out.title  = r.title.toStdString();
    out.refW   = r.refW;
    out.refH   = r.refH;
    out.buffer = r.buffer;
    out.sites.clear();
    for (const auto& s : r.sites) {
        pcbcore::RecipeSite cs;
        cs.label  = s.label.toStdString();
        cs.kind   = s.kind.toStdString();
        cs.cx     = s.cx;
        cs.cy     = s.cy;
        cs.bodyW  = s.bodyW;
        cs.bodyH  = s.bodyH;
        cs.checks.clear();
        for (const auto& c : s.checks) cs.checks.push_back(c.toStdString());
        cs.shiftTolPct = s.shiftTolPct;
        cs.polaritySide = s.polaritySide.toStdString();
        out.sites.push_back(cs);
    }
    return out;
}

} // namespace w7
