#include "board_recipe.h"
#include "hsv_params.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace w7 {

const QStringList& knownChecks() {
    // 判定算子里真正有实现的规则。配方里写了表以外的,
    // 是拼错了或者算法还没写 —— 两种都必须报错。
    static const QStringList kKnown = {
        QString::fromUtf8(recipe_rule::kMissing),
        QString::fromUtf8(recipe_rule::kShifted),
        QString::fromUtf8(recipe_rule::kWrong),
        QString::fromUtf8(recipe_rule::kPolarity),
        QString::fromUtf8(recipe_rule::kBridge),
        QString::fromUtf8(recipe_rule::kInsufficient),
    };
    return kKnown;
}

namespace {

// 必填整数。**不给默认值** —— 见 hsv_params.h 里那条规矩:
// 读不到就报错,不要悄悄拿一个默认值顶上,那会让用户以为
// 自己配的数在起作用(实际跑的是兜底值)。
//
// 范围检查在 parseRecipe 里按字段做(坐标要落在图内,尺寸要 > 0),
// 这里只管"是不是一个能用的整数"。
bool reqInt(const QJsonObject& o, const QString& key, int* out, QString* reason) {
    const QJsonValue v = o.value(key);
    if (v.isUndefined()) {
        *reason = QString("站点缺字段 %1").arg(key);
        return false;
    }
    if (!v.isDouble()) {
        *reason = QString("字段 %1 不是数字(实际是%2)")
                      .arg(key, jsonTypeName(v.type()));
        return false;
    }
    const double d = v.toDouble();
    // toDouble 拿得到小数,而坐标/尺寸取整后小数部分就是"存错了"
    if (d != static_cast<double>(static_cast<int>(d))) {
        *reason = QString("字段 %1 是小数 %2 —— 像素坐标必须是整数").arg(key).arg(d);
        return false;
    }
    *out = static_cast<int>(d);
    return true;
}

bool reqString(const QJsonObject& o, const QString& key,
               QString* out, QString* reason) {
    const QJsonValue v = o.value(key);
    if (v.isUndefined()) {
        *reason = QString("站点缺字段 %1").arg(key);
        return false;
    }
    if (!v.isString()) {
        *reason = QString("字段 %1 不是字符串(实际是%2)")
                      .arg(key, jsonTypeName(v.type()));
        return false;
    }
    *out = v.toString();
    if (out->isEmpty()) {
        *reason = QString("字段 %1 是空字符串").arg(key);
        return false;
    }
    return true;
}

} // namespace

bool parseRecipe(const QJsonObject& root, BoardRecipe* out, QString* reason) {
    BoardRecipe r;

    if (!reqString(root, "name", &r.name, reason)) return false;
    if (!reqInt(root, "ref_w", &r.refW, reason)) return false;
    if (!reqInt(root, "ref_h", &r.refH, reason)) return false;

    // title / buffer 可选:标题只影响显示,余量缺了给约定值。
    r.title = root.value("title").toString(r.name);
    r.buffer = root.value("buffer").isDouble() ? root.value("buffer").toInt() : 10;
    if (r.buffer < 0) {
        *reason = QString("buffer 不能是负数(实际 %1)").arg(r.buffer);
        return false;
    }

    const QJsonValue sv = root.value("sites");
    if (!sv.isArray()) {
        *reason = QString("缺 sites 数组(实际是%1) —— "
                          "没有站点表就等于没有板型真值")
                      .arg(jsonTypeName(sv.type()));
        return false;
    }
    const QJsonArray sites = sv.toArray();
    if (sites.isEmpty()) {
        // 空站点表比缺字段更危险:它不报错,判定环节会输出"0 个元件,
        // 零缺陷,PASS"。这块板等于什么都没查却报合格。
        *reason = QString("sites 是空数组 —— 0 个站点会一路走到\"零缺陷判定合格\","
                          "宁可加载失败也不能让它过");
        return false;
    }

    for (int i = 0; i < sites.size(); ++i) {
        const QJsonValue sv1 = sites[i];
        const QString at = QString("第 %1 个站点").arg(i + 1);
        if (!sv1.isObject()) {
            *reason = QString("%1 不是对象(实际是%2)")
                          .arg(at, jsonTypeName(sv1.type()));
            return false;
        }
        const QJsonObject so = sv1.toObject();

        RecipeSite s;
        if (!reqString(so, "label", &s.label, reason)) {
            *reason = QString("%1 的 %2").arg(at, *reason);
            return false;
        }
        if (!reqString(so, "kind", &s.kind, reason)) {
            *reason = QString("%1(%2) 的 %3").arg(at, s.label, *reason);
            return false;
        }
        if (!reqInt(so, "cx", &s.cx, reason) || !reqInt(so, "cy", &s.cy, reason)) {
            *reason = QString("%1(%2) 的 %3").arg(at, s.label, *reason);
            return false;
        }
        if (!reqInt(so, "body_w", &s.bodyW, reason)
            || !reqInt(so, "body_h", &s.bodyH, reason)) {
            *reason = QString("%1(%2) 的 %3").arg(at, s.label, *reason);
            return false;
        }

        // 坐标与尺寸的合理性:本体宽高必须为正(否则 ROI 面积为 0,
        // 判定环节会 continue 掉,又一次"没查却报合格"),
        // 中心要落在参考图内(否则这个站点在这块板上根本不存在)。
        if (s.bodyW <= 0 || s.bodyH <= 0) {
            *reason = QString("%1(%2) 的本体尺寸 %3x%4 必须为正")
                          .arg(at, s.label).arg(s.bodyW).arg(s.bodyH);
            return false;
        }
        if (s.cx < 0 || s.cy < 0 || s.cx > r.refW || s.cy > r.refH) {
            *reason = QString("%1(%2) 的中心 (%3,%4) 不在参考图 %5x%6 内")
                          .arg(at, s.label).arg(s.cx).arg(s.cy).arg(r.refW).arg(r.refH);
            return false;
        }

        // checks:没写就当"这个元件不跑任何规则" —— 这是有意义的
        // (接线端子、LED 之类本来就没规则可查),不是错误。
        const QJsonValue cv = so.value("checks");
        if (!cv.isUndefined()) {
            if (!cv.isArray()) {
                *reason = QString("%1(%2) 的 checks 不是数组(实际是%3)")
                              .arg(at, s.label, jsonTypeName(cv.type()));
                return false;
            }
            for (const auto& c : cv.toArray()) {
                if (!c.isString()) {
                    *reason = QString("%1(%2) 的 checks 里有非字符串项(实际是%3)")
                                  .arg(at, s.label, jsonTypeName(c.type()));
                    return false;
                }
                const QString rule = c.toString();
                if (!knownChecks().contains(rule)) {
                    // 拼错规则名 = 那条规则一条都不会跑,而且不报错。
                    // 这里必须拦下来,否则配方看着配好了,实际是空转。
                    *reason = QString("%1(%2) 要查的规则 \"%3\" 判定算子没实现。"
                                      "已实现的:%4")
                                  .arg(at, s.label, rule,
                                       knownChecks().join(" / "));
                    return false;
                }
                if (!s.checks.contains(rule)) s.checks.append(rule);
            }
        }

        // shift_tol_pct 可选:不同封装的偏移容忍度差很多
        // (小电阻挪 20px 可能还没出框,QFP64 挪 20px 就是大事),
        // 写死在算子里等于"所有元件一样灵"。
        if (so.value("shift_tol_pct").isDouble()) {
            const double t = so.value("shift_tol_pct").toDouble();
            if (t <= 0 || t >= 50) {
                *reason = QString("%1(%2) 的 shift_tol_pct=%3 不合理"
                                  "(应为 0~50 之间的小数,表示占 ROI 宽高的百分比)")
                              .arg(at, s.label).arg(t);
                return false;
            }
            s.shiftTolPct = t;
        }

        r.sites.append(s);
    }

    // 位号重复会让报告里出现两个 "R1",而判定结果挂在位号上 ——
    // 排查时看到哪个 R1 出问题就成了猜谜。
    for (int i = 0; i < r.sites.size(); ++i) {
        for (int j = i + 1; j < r.sites.size(); ++j) {
            if (r.sites[i].label == r.sites[j].label) {
                *reason = QString("位号 \"%1\" 重复(第 %2、%3 个站点)")
                              .arg(r.sites[i].label).arg(i + 1).arg(j + 1);
                return false;
            }
        }
    }

    *out = r;
    return true;
}

bool loadRecipe(const QString& path, BoardRecipe* out, QString* reason) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *reason = QString("读不到配方文件 %1(%2)")
                      .arg(path, f.errorString());
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError) {
        *reason = QString("配方 %1 不是合法 JSON:第 %2 行第 %3 列 %4")
                      .arg(path).arg(perr.offset).arg(0).arg(perr.errorString());
        return false;
    }
    if (!doc.isObject()) {
        *reason = QString("配方 %1 顶层不是对象(是数组或空)")
                      .arg(path);
        return false;
    }
    if (!parseRecipe(doc.object(), out, reason)) {
        *reason = QString("配方 %1: %2").arg(path, *reason);
        return false;
    }
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

} // namespace w7