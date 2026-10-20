#include "pcb_core.h"
#include "pcb_color_ref.h"
#include "mini_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace pcbcore {

// ============================================================
// 小工具
// ============================================================
namespace {

// 按 QString::arg(x, 0, 'f', n) 的口径格式化,保证 W6 / W7 两边
// 同一份判据产出的文字一致(报告要能互相对照)。
std::string fmt(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

std::string fmtInt(long long v) { return std::to_string(v); }

// JSON 值的类型名(报错时给人话)
const char* typeName(const mini_json::Value& v) {
    switch (v.type()) {
        case mini_json::Value::Type::Null:   return "null";
        case mini_json::Value::Type::Bool:   return "布尔值";
        case mini_json::Value::Type::Number: return "数字";
        case mini_json::Value::Type::String: return "字符串";
        case mini_json::Value::Type::Array:  return "数组";
        case mini_json::Value::Type::Object: return "对象";
    }
    return "未知类型";
}

// 必填整数。**不给默认值** —— 读不到就报错,
// 不要悄悄拿一个默认值顶上,那会让用户以为自己配的数在起作用。
bool reqInt(const mini_json::Value& o, const char* key, int* out, std::string* reason) {
    const mini_json::Value* v = o.find(key);
    if (!v) {
        *reason = std::string("站点缺字段 ") + key;
        return false;
    }
    if (!v->isNumber()) {
        *reason = std::string("字段 ") + key + " 不是数字(实际是" + typeName(*v) + ")";
        return false;
    }
    const double d = v->asNumber();
    if (d != static_cast<double>(static_cast<int>(d))) {
        *reason = std::string("字段 ") + key + " 是小数 " + fmt(d, 4) + " —— 像素坐标必须是整数";
        return false;
    }
    *out = static_cast<int>(d);
    return true;
}

bool reqString(const mini_json::Value& o, const char* key,
               std::string* out, std::string* reason) {
    const mini_json::Value* v = o.find(key);
    if (!v) {
        *reason = std::string("站点缺字段 ") + key;
        return false;
    }
    if (!v->isString()) {
        *reason = std::string("字段 ") + key + " 不是字符串(实际是" + typeName(*v) + ")";
        return false;
    }
    *out = v->asString();
    if (out->empty()) {
        *reason = std::string("字段 ") + key + " 是空字符串";
        return false;
    }
    return true;
}

// 把一个逐像素判据铺成 0/255 mask。
//
// 四条颜色规则(暗/错色/白/质心)都要"逐像素分类再统计占比",
// 各自写一遍双层循环就是第四次抄副本。
template <typename Pred>
cv::Mat maskBy(const cv::Mat& roi, Pred pred) {
    cv::Mat m(roi.rows, roi.cols, CV_8U, cv::Scalar(0));
    for (int y = 0; y < roi.rows; ++y) {
        const cv::Vec3b* src = roi.ptr<cv::Vec3b>(y);
        uchar* dst = m.ptr<uchar>(y);
        for (int x = 0; x < roi.cols; ++x) dst[x] = pred(src[x]) ? 255 : 0;
    }
    return m;
}

// 没有 checks 时,从封装类别推一套默认规则。
//
// 只在**上游没给 checks** 时用 —— 老存档、auto_contour 产物。
// 配方明确写了 checks(哪怕是空数组)则以配方为准。
//
// 为什么不能"没给就当不查任何规则":那会让一条规则都不跑,
// 于是**所有缺陷图一路 PASS** —— 这正是原来七处 label== 全落空
// 时的行为(实测:三块好板全判废,两块废板输出与好板逐字相同)。
//
// 为什么按 kind 而不是按 label:label 是"这块板上的第几个元件",
// 换块板就对不上;kind 是"这个封装长什么样",换板子也对得上 ——
// 那是物理事实,不是标定值。
std::vector<std::string> knownRulesForKind(const std::string& kind) {
    std::string k;
    for (char c : kind) k.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    if (k == "RES" || k == "R") {
        // 电阻:黑本体,两侧金焊盘。dark -> missing/shifted;
        // 单色本体,换色看得出来 -> wrong;金条连片 -> bridge。
        return {rule::kMissing, rule::kShifted, rule::kWrong, rule::kBridge};
    }
    if (k == "IC" || k == "TO220" || k == "QFP64" || k == "SOIC8") {
        // IC:黑本体 + 几十个金焊盘(有中位数才谈得上 insufficient)。
        return {rule::kMissing, rule::kInsufficient, rule::kBridge};
    }
    if (k == "C" || k == "CAP" || k == "CAP_SMALL" || k == "CAP_BIG") {
        // 电容:本体是彩色/灰色而不是黑,所以 missing 规则**不适用**
        // (会被整框判缺件);但有白色极性条,也有金焊盘。
        return {rule::kPolarity, rule::kBridge};
    }
    // 不认识的封装(TERMINAL / LED / BUTTON / XTAL ...):
    // **一条规则都不跑,但要说得出来**。
    //
    // ⚠ 这里**故意没有 short**:短路判据要量"两个端子之间的绝缘缝",
    //   而端子位置只存在于**配方**(生成器登记 -> 导出脚本带出来)。
    //   走兜底这条路(老存档 / auto_contour 产物)时根本没有端子位置,
    //   硬塞一条进去 = 每个站点都报"没查",把真正的"没查"淹掉。
    //   配方说了算 —— 配方里写了 short 的元件才查。
    return {};
}

// missing 规则该问哪个问题:"本体够不够暗",还是"框里有没有东西"?
//
// 这不是风格选择,是两种**物理事实**,实测差别极大(2026-10-14):
//
//   黑本体(RES/DIODE/TO220/QFP64/SOIC8):暗像素就是本体本身,
//     实测 power 好板 RES 28.69% -> 模糊到 k=6 仍 19.18%。
//
//   彩色/灰色本体(CAP*):**暗像素只剩那一圈 1px 黑色描边**。
//     实测 power 好板 CAP_SMALL 5.08%,而 σ 只有图宽千分之一(k=1)
//     就掉到 **0.00%** —— 一步归零,不是斜坡。
//
// 换成"框里有没有非板面像素"(inspect 已经算好的 body_pct)之后,
// 同一批图实测(power C2 缺件 / 好板,body_pct%):
//     k=0  7.7 / 49.2      k=3  22.2 / 89.9
//     k=1 13.2 / 73.1      k=6  36.5 / 97.6
// 40px 的圆盘在同样大小的模糊下几乎无损 —— 差的不是阈值,是问的问题。
//
// ⚠ 只对实测过的封装生效。LED / HEADER / INDUCTOR / TERMINAL / BUTTON
//   同样不是黑本体(dark 实测 0.00~4.77%),**但没有缺件真值图**,
//   改它们等于凭空改判据,所以这里一律不改。
bool missingAsksBodyPresence(const std::string& kind) {
    std::string k;
    for (char c : kind) k.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    return k == "C" || k == "CAP" || k == "CAP_SMALL" || k == "CAP_BIG";
}

// 这个元件跑哪些规则 —— **判定"配方说了算"的唯一入口**。
//
// ⚠ "checks 字段不存在"和"checks 是空数组"是**两件不同的事**,
//   以前这里把两者当成同一件,于是 demo 板的 C2 明明在配方里写了
//   checks: [](意思是"这个元件我不管它"),却掉进兜底,凭空多出
//   polarity+bridge —— 好板 baseline.png 在**所有缩放**下都报焊桥。
//
//     字段不存在    -> 上游没给配方信息(老存档 / auto_contour 产物)
//                     按封装类别推一套,免得一条规则都不跑 → 假 PASS。
//     字段存在但是空 -> 配方**明确说这个元件不参与判定**。照做。
std::vector<std::string> rulesForSite(const InspectSite& s) {
    if (!s.checks.empty() || s.checksExplicit) {
        return s.checks;
    }
    return knownRulesForKind(s.kind);
}

} // namespace

// ============================================================
// knownChecks
// ============================================================
const std::vector<std::string>& knownChecks() {
    static const std::vector<std::string> kKnown = {
        rule::kMissing, rule::kShifted, rule::kWrong,
        rule::kPolarity, rule::kBridge, rule::kInsufficient,
        rule::kShort,
    };
    return kKnown;
}

// ============================================================
// 配方解析
// ============================================================
bool parseRecipeJson(const std::string& text, BoardRecipe* out, std::string* reason) {
    BoardRecipe r;

    mini_json::Value root;
    std::string perr;
    if (!mini_json::parse(text, &root, &perr)) {
        if (reason) *reason = "不是合法 JSON:" + perr;
        return false;
    }
    if (!root.isObject()) {
        if (reason) *reason = std::string("顶层不是对象(是") + typeName(root) + ")";
        return false;
    }

    if (!reqString(root, "name", &r.name, reason)) return false;
    if (!reqInt(root, "ref_w", &r.refW, reason)) return false;
    if (!reqInt(root, "ref_h", &r.refH, reason)) return false;

    // title / buffer 可选:标题只影响显示,余量缺了给约定值。
    if (const mini_json::Value* t = root.find("title")) r.title = t->asString(r.name);
    else                                                  r.title = r.name;
    if (const mini_json::Value* b = root.find("buffer")) {
        if (b->isNumber()) r.buffer = static_cast<int>(b->asNumber());
    }
    if (r.buffer < 0) {
        if (reason) *reason = "buffer 不能是负数(实际 " + fmtInt(r.buffer) + ")";
        return false;
    }

    const mini_json::Value* sv = root.find("sites");
    if (!sv || !sv->isArray()) {
        if (reason) {
            *reason = std::string("缺 sites 数组(实际是") +
                      (sv ? typeName(*sv) : "未定义") +
                      ") —— 没有站点表就等于没有板型真值";
        }
        return false;
    }
    if (sv->asArray().empty()) {
        // 空站点表比缺字段更危险:它不报错,判定环节会输出"0 个元件,
        // 零缺陷,PASS"。这块板等于什么都没查却报合格。
        if (reason) {
            *reason = "sites 是空数组 —— 0 个站点会一路走到\"零缺陷判定合格\","
                      "宁可加载失败也不能让它过";
        }
        return false;
    }

    const std::vector<mini_json::Value>& sites = sv->asArray();
    for (size_t i = 0; i < sites.size(); ++i) {
        const mini_json::Value& sv1 = sites[i];
        const std::string at = "第 " + fmtInt(static_cast<long long>(i) + 1) + " 个站点";
        if (!sv1.isObject()) {
            if (reason) *reason = at + " 不是对象(实际是" + typeName(sv1) + ")";
            return false;
        }

        RecipeSite s;
        std::string sub;
        if (!reqString(sv1, "label", &s.label, &sub)) {
            if (reason) *reason = at + " 的 " + sub;
            return false;
        }
        if (!reqString(sv1, "kind", &s.kind, &sub)) {
            if (reason) *reason = at + "(" + s.label + ") 的 " + sub;
            return false;
        }
        if (!reqInt(sv1, "cx", &s.cx, &sub) || !reqInt(sv1, "cy", &s.cy, &sub)) {
            if (reason) *reason = at + "(" + s.label + ") 的 " + sub;
            return false;
        }
        if (!reqInt(sv1, "body_w", &s.bodyW, &sub)
            || !reqInt(sv1, "body_h", &s.bodyH, &sub)) {
            if (reason) *reason = at + "(" + s.label + ") 的 " + sub;
            return false;
        }

        // 坐标与尺寸的合理性:本体宽高必须为正(否则 ROI 面积为 0,
        // 判定环节会 continue 掉,又一次"没查却报合格"),
        // 中心要落在参考图内(否则这个站点在这块板上根本不存在)。
        if (s.bodyW <= 0 || s.bodyH <= 0) {
            if (reason) {
                *reason = at + "(" + s.label + ") 的本体尺寸 " +
                          fmtInt(s.bodyW) + "x" + fmtInt(s.bodyH) + " 必须为正";
            }
            return false;
        }
        if (s.cx < 0 || s.cy < 0 || s.cx > r.refW || s.cy > r.refH) {
            if (reason) {
                *reason = at + "(" + s.label + ") 的中心 (" + fmtInt(s.cx) + "," +
                          fmtInt(s.cy) + ") 不在参考图 " + fmtInt(r.refW) + "x" +
                          fmtInt(r.refH) + " 内";
            }
            return false;
        }

        // checks:没写就当"这个元件不跑任何规则" —— 这是有意义的
        // (接线端子、LED 之类本来就没规则可查),不是错误。
        if (const mini_json::Value* cv = sv1.find("checks")) {
            if (!cv->isArray()) {
                if (reason) {
                    *reason = at + "(" + s.label + ") 的 checks 不是数组(实际是" +
                              typeName(*cv) + ")";
                }
                return false;
            }
            for (const mini_json::Value& c : cv->asArray()) {
                if (!c.isString()) {
                    if (reason) {
                        *reason = at + "(" + s.label + ") 的 checks 里有非字符串项(实际是" +
                                  typeName(c) + ")";
                    }
                    return false;
                }
                const std::string rn = c.asString();
                const auto& known = knownChecks();
                if (std::find(known.begin(), known.end(), rn) == known.end()) {
                    // 拼错规则名 = 那条规则一条都不会跑,而且不报错。
                    // 这里必须拦下来,否则配方看着配好了,实际是空转。
                    std::string list;
                    for (size_t k = 0; k < known.size(); ++k) {
                        if (k) list += " / ";
                        list += known[k];
                    }
                    if (reason) {
                        *reason = at + "(" + s.label + ") 要查的规则 \"" + rn +
                                  "\" 判定算子没实现。已实现的:" + list;
                    }
                    return false;
                }
                if (std::find(s.checks.begin(), s.checks.end(), rn) == s.checks.end()) {
                    s.checks.push_back(rn);
                }
            }
        }

        // shift_tol_pct 可选:不同封装的偏移容忍度差很多
        // (小电阻挪 20px 可能还没出框,QFP64 挪 20px 就是大事),
        // 写死在算子里等于"所有元件一样灵"。
        if (const mini_json::Value* t = sv1.find("shift_tol_pct")) {
            if (t->isNumber()) {
                const double v = t->asNumber();
                if (v <= 0 || v >= 50) {
                    if (reason) {
                        *reason = at + "(" + s.label + ") 的 shift_tol_pct=" +
                                  fmt(v, 2) +
                                  " 不合理(应为 0~50 之间的小数,表示占 ROI 宽高的百分比)";
                    }
                    return false;
                }
                s.shiftTolPct = v;
            }
        }

        // polarity_side 可选:"left"/"right",相对元件本体。
        //
        // ⚠ 写错的值必须**报错**,不能当"没写"处理:
        //   "没写" = 判不了极性(会报"没查",算诚实);
        //   写错   = 判据拿着一个看不懂的基准去比,可能把好板判成反装。
        //   后者是**静默误报**,比漏检更难查。
        if (const mini_json::Value* pv = sv1.find("polarity_side")) {
            if (!pv->isString()) {
                if (reason) {
                    *reason = at + "(" + s.label + ") 的 polarity_side 不是字符串(实际是" +
                              typeName(*pv) + ")";
                }
                return false;
            }
            const std::string side = pv->asString();
            if (!side.empty() && side != "left" && side != "right") {
                if (reason) {
                    *reason = at + "(" + s.label + ") 的 polarity_side=\"" + side +
                              "\" 不认(只能是 left / right,或空串表示没有极性条)";
                }
                return false;
            }
            s.polaritySide = side;
        }

        // pads 可选:这个元件的端子(焊盘)位置,格式 [[x, y, r], ...]。
        //
        // ⚠ 为什么要有这个字段:短路判据要回答"**这两个端子之间**
        //   有没有金属"。而"哪两个端子属于同一个元件、它们在哪儿"
        //   只有**画焊盘的那一行代码**知道 —— 和 polarity_side 同理:
        //   期望侧由知道真相的那一方登记,判据不猜。
        //
        //   以前判据是"在全图里找最大的蓝紫连通域,看它像不像一个
        //   特定大小的圆环" —— 那量的是**生成器画给人眼看的标记圈**,
        //   不是短路本身(见 pcb_core.h 里 JudgeParams 那段)。
        //
        // 字段缺省 = 这个元件没登记端子 -> short 规则判不了,
        // 判定时会报"没查",不会装作查过了。
        if (const mini_json::Value* pv = sv1.find("pads")) {
            if (!pv->isArray()) {
                if (reason) {
                    *reason = at + "(" + s.label + ") 的 pads 不是数组(实际是" +
                              typeName(*pv) + ")";
                }
                return false;
            }
            const std::vector<mini_json::Value>& parr = pv->asArray();
            for (size_t k = 0; k < parr.size(); ++k) {
                const mini_json::Value& one = parr[k];
                const std::string atPad = at + "(" + s.label + ") 的第 " +
                                          fmtInt(static_cast<long long>(k) + 1) + " 个端子";
                if (!one.isArray()) {
                    if (reason) {
                        *reason = atPad + " 不是数组(实际是" + typeName(one) +
                                  ") —— 端子要写成 [x, y, 焊盘半宽]";
                    }
                    return false;
                }
                const std::vector<mini_json::Value>& f = one.asArray();
                if (f.size() != 3) {
                    if (reason) {
                        *reason = atPad + " 必须正好 3 个数 [x, y, 焊盘半宽](实际 " +
                                  fmtInt(static_cast<long long>(f.size())) + " 个)";
                    }
                    return false;
                }
                int c3[3] = {0, 0, 0};
                for (int j = 0; j < 3; ++j) {
                    if (!f[static_cast<size_t>(j)].isNumber()) {
                        if (reason) {
                            *reason = atPad + " 的第 " + fmtInt(j + 1) +
                                      " 项不是数字(实际是" +
                                      typeName(f[static_cast<size_t>(j)]) + ")";
                        }
                        return false;
                    }
                    const double d = f[static_cast<size_t>(j)].asNumber();
                    if (d != static_cast<double>(static_cast<int>(d))) {
                        if (reason) {
                            *reason = atPad + " 的第 " + fmtInt(j + 1) +
                                      " 项是小数 " + fmt(d, 4) + " —— 像素坐标必须是整数";
                        }
                        return false;
                    }
                    c3[j] = static_cast<int>(d);
                }
                if (c3[0] < 0 || c3[1] < 0) {
                    if (reason) {
                        *reason = atPad + " 的坐标 (" + fmtInt(c3[0]) + "," +
                                  fmtInt(c3[1]) + ") 是负数";
                    }
                    return false;
                }
                if (c3[2] <= 0) {
                    if (reason) {
                        *reason = atPad + " 的焊盘半宽 " + fmtInt(c3[2]) +
                                  " 必须为正 —— 半宽用来把\"焊盘自己\"从"
                                  "\"两脚之间的绝缘缝\"里掐掉,0 的话缝就量不准了";
                    }
                    return false;
                }
                Pad pd;
                pd.x = c3[0];
                pd.y = c3[1];
                pd.r = c3[2];
                s.pads.push_back(pd);
            }
        }

        r.sites.push_back(s);
    }

    // 位号重复会让报告里出现两个 "R1",而判定结果挂在位号上 ——
    // 排查时看到哪个 R1 出问题就成了猜谜。
    for (size_t i = 0; i < r.sites.size(); ++i) {
        for (size_t j = i + 1; j < r.sites.size(); ++j) {
            if (r.sites[i].label == r.sites[j].label) {
                if (reason) {
                    *reason = "位号 \"" + r.sites[i].label + "\" 重复(第 " +
                              fmtInt(static_cast<long long>(i) + 1) + "、" +
                              fmtInt(static_cast<long long>(j) + 1) + " 个站点)";
                }
                return false;
            }
        }
    }

    *out = r;
    return true;
}

bool loadRecipeFile(const std::string& path, BoardRecipe* out, std::string* reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (reason) *reason = "读不到配方文件 " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    f.close();

    if (!parseRecipeJson(text, out, reason)) {
        if (reason) *reason = "配方 " + path + ": " + *reason;
        return false;
    }
    return true;
}

// ============================================================
// 定位
// ============================================================
InspectResult inspect(const cv::Mat& img, const BoardRecipe& recipe,
                      const pcbcolor::HsvBand& band) {
    InspectResult res;
    res.marked = img.clone();

    // 板面基色**逐图估计**。相对判据的右端也必须带 k 才叫相对 ——
    // 拿一张参考图的板色当常量写进配置,等式右边就没有 k 了。
    res.boardBase = pcbcolor::estimateBoardBase(img);
    res.boardBaseOk = true;

    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    res.goldMask = pcbcolor::opened(pcbcolor::bandMask(hsv, res.boardBase, band),
                                    pcbcolor::kernelScale(img));

    // 站点几何按图像尺寸等比缩放。**基准从固定的 1000x700 换成了
    // 配方自己的 ref_w/ref_h** —— 换板型时基准也跟着换,
    // 否则一块 1200x900 的板会拿 1000x700 的坐标去切,全错。
    //
    // 余量取**较小**的那个缩放比:ROI 撑大只会稀释下游的比例阈值,
    // 留窄一点最多是贴边,不会把元件切掉 —— 宁可保守。
    const double sx = static_cast<double>(img.cols) / recipe.refW;
    const double sy = static_cast<double>(img.rows) / recipe.refH;
    const int buf = (std::max)(1, static_cast<int>(
        std::lround(recipe.buffer * (std::min)(sx, sy))));

    for (const auto& s : recipe.sites) {
        const int cx = static_cast<int>(std::lround(s.cx * sx));
        const int cy = static_cast<int>(std::lround(s.cy * sy));
        const int bw = static_cast<int>(std::lround(s.bodyW * sx));
        const int bh = static_cast<int>(std::lround(s.bodyH * sy));
        cv::Rect roi(cx - bw / 2 - buf, cy - bh / 2 - buf,
                     bw + 2 * buf, bh + 2 * buf);
        // ROI 与图像边界求交,换分辨率也不会越界
        roi &= cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0 || roi.height <= 0) {
            res.skippedSites.push_back(s.label);
            continue;
        }

        InspectSite site;
        site.label = s.label;
        site.kind  = s.kind;
        site.roi   = roi;
        site.checks = s.checks;
        site.checksExplicit = true;   // 配方说了算
        site.hasShiftTol = s.hasShiftTol();
        site.shiftTolPct = s.shiftTolPct;
        site.polaritySide = s.polaritySide;

        // 端子位置**跟着这张图缩放**。和站点几何同一个基准(配方的
        // ref_w/ref_h),不然换分辨率就会量到别的地方去 —— 短路判据
        // 量的就是"这两个端子之间",位置错了结论就是错的。
        site.pads.clear();
        site.pads.reserve(s.pads.size());
        for (const auto& p : s.pads) {
            Pad pd;
            pd.x = static_cast<int>(std::lround(p.x * sx));
            pd.y = static_cast<int>(std::lround(p.y * sy));
            pd.r = (std::max)(1, static_cast<int>(std::lround(
                              p.r * (std::min)(sx, sy))));
            site.pads.push_back(pd);
        }

        // ROI 内的金像素占比。与下游所有阈值同一量纲。
        site.goldPct = 100.0 * cv::countNonZero(res.goldMask(roi))
                     / (roi.width * roi.height);

        // --- 这道闸:这份配方跟这张图是一块板吗? ---
        //
        // 框里**完全**是板面基色 = 这个站点上没有元件。
        // 一块板上一个都找不到,只可能是配方配错了图。
        //
        // 以前没有这道闸,实测后果(2026-10-09,三块好板):
        //   16 个站点全空 -> 下游 16 条 "missing" -> 很自信地判废,
        //   而废板报的是同样的 missing,两者**逐字相同**。
        //   程序错了,但表现得像在正常工作。
        //
        // 判据用"与板面基色的距离",不写死像素差 —— 后者就是
        // 我们这个项目反复踩的绝对值坑:换个亮度/材质就整片失效。
        const cv::Vec3b bb = res.boardBase;
        int nonBoard = 0;
        for (int y = roi.y; y < roi.y + roi.height; ++y) {
            const cv::Vec3b* row = img.ptr<cv::Vec3b>(y);
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                const cv::Vec3b& p = row[x];
                if (std::abs(p[0] - bb[0]) > 12 ||
                    std::abs(p[1] - bb[1]) > 12 ||
                    std::abs(p[2] - bb[2]) > 12) {
                    ++nonBoard;
                }
            }
        }
        site.bodyPct = 100.0 * nonBoard / (roi.width * roi.height);
        site.hasBodyPct = true;
        // 1% 是保守取值:真元件再怎么小,本体也远不止占框内 1%。
        // 定高了会把"元件被整体擦掉"的真缺陷也当成"配方不匹配",
        // 那就变成漏检了 —— 两边都要顾。
        if (site.bodyPct >= 1.0) ++res.sitesWithBody;

        res.sites.push_back(site);

        cv::rectangle(res.marked, roi, cv::Scalar(0, 220, 0), 2);
        cv::putText(res.marked, s.label + "(" + s.kind + ")",
                    cv::Point(roi.x, (std::max)(12, roi.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 220, 0), 1);
    }
    return res;
}

// ============================================================
// 判定
// ============================================================
JudgeResult judge(const cv::Mat& img,
                  const std::vector<InspectSite>& sites,
                  const JudgeParams& params,
                  const cv::Mat& goldMask,
                  const cv::Vec3b& boardBase) {
    JudgeResult res;
    res.marked = img.clone();

    // ---------- 覆盖度检查:一个元件都没有规则可跑时不给结论 ----------
    //
    // 判据是"这个元件身上有没有配规则",这是配方说的,
    // 换板型时配方跟着换,这里就不会再误判。
    int withRules = 0;
    for (const auto& s : sites) {
        if (!rulesForSite(s).empty()) ++withRules;
    }
    res.withRules = withRules;
    res.totalComponents = static_cast<int>(sites.size());
    if (withRules == 0) {
        res.refused = true;
        res.refuseReason = fmtInt(static_cast<long long>(sites.size())) +
            " 个元件里没有一个配了判定规则,判定跑不起来 —— 不给结论,而不是报 PASS。"
            "请在板型配方里给元件写上 \"checks\"(可查的规则:";
        for (size_t k = 0; k < knownChecks().size(); ++k) {
            if (k) res.refuseReason += " / ";
            res.refuseReason += knownChecks()[k];
        }
        res.refuseReason += ")。";
        return res;
    }

    // ---------- 撞 8 位色深上限时拒绝判定,而不是硬撑一个 PASS ----------
    //
    // 错色判据要"绿通道 >= offColorMul x 板面绿通道"。板面被照亮到这个
    // 程度时,那个倍数已经超过 255,uint8 里根本不存在这么亮的像素,
    // 判据永远返回 false —— 不报错、不告警,直接报 PASS,废板判合格。
    if (pcbcolor::ceilingBreached(boardBase, params.offColorMul)) {
        res.refused = true;
        res.refuseReason =
            "曝光过强,无法判定:板面基色 BGR(" + fmtInt(boardBase[0]) + "," +
            fmtInt(boardBase[1]) + "," + fmtInt(boardBase[2]) +
            "),错色判据要求绿通道 >= " + fmt(params.offColorMul, 2) + " 倍板面 = " +
            fmtInt(static_cast<long long>(params.offColorMul * boardBase[1])) +
            ",已超过 8 位色深上限 255。此时判据永远不成立,若继续跑会报出一个"
            "没有依据的 PASS。实测这条线约在亮度 " +
            fmt(pcbcolor::ceilingHeadroomK(boardBase, params.offColorMul), 2) +
            " 倍处(当前板面还能撑到 " +
            fmt(pcbcolor::ceilingHeadroomK(boardBase, params.offColorMul), 2) +
            " 倍)。请降低曝光后重拍。";
        return res;
    }

    // 全板 body_pct 的中位数 —— 归一化判据的分母。
    //
    // 只统计**上游真给了 body_pct 的站点**:老存档 / auto_contour 产物
    // 没有这个字段,把它们的 0 混进来算中位数会把分母拉到接近 0,
    // 于是比值爆表,全板一起误报缺件 —— 症状看起来像"阈值太小"。
    std::vector<double> bodyAll;
    bodyAll.reserve(sites.size());
    for (const auto& s : sites) {
        if (s.hasBodyPct) bodyAll.push_back(s.bodyPct);
    }
    double bodyMedian = 0.0;
    if (!bodyAll.empty()) {
        std::sort(bodyAll.begin(), bodyAll.end());
        bodyMedian = bodyAll[bodyAll.size() / 2];
    }

    for (const auto& s : sites) {
        const std::string& label = s.label;
        const std::vector<std::string> rules = rulesForSite(s);

        // shift_tol_pct:配方可按封装单列(小电阻和 QFP64 的容忍度差很多),
        // 没给就用算子默认值。
        const double cShiftX = s.hasShiftTol ? s.shiftTolPct : params.shiftXPct;
        const double cShiftY = s.hasShiftTol ? s.shiftTolPct : params.shiftYPct;

        cv::Rect roi = s.roi;
        roi &= cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0 || roi.height <= 0) continue;

        // 本 ROI 的"尺子"。所有阈值都拿它当基准,不再出现像素数。
        const double roiArea = static_cast<double>(roi.width) * roi.height;
        if (roiArea <= 0) continue;

        auto hasRule = [&](const char* r) {
            return std::find(rules.begin(), rules.end(), std::string(r)) != rules.end();
        };

        // missing / shifted 两条规则看的是同一张暗像素 mask,算一遍就够。
        cv::Mat darkMask;
        if (hasRule(rule::kMissing) || hasRule(rule::kShifted)) {
            darkMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                return pcbcolor::isDark(p, boardBase, params.darkVMul);
            });
        }

        // ---------- missing: 框里没有元件 ----------
        if (hasRule(rule::kMissing)) {
            const double darkCount = cv::countNonZero(darkMask);
            const double darkPct   = 100.0 * darkCount / roiArea;
            const bool   byBody    = missingAsksBodyPresence(s.kind);
            const double bodyPct   = s.bodyPct;
            const bool   haveBodyPct = s.hasBodyPct;
            // 归一化:除以**同一张图上所有站点的 body_pct 中位数**。
            //
            // 代价是**站点之间耦合了**:一个站点的判定要看别的站点。
            // 站点太少时中位数不可靠(只有一个站点时比值恒等于 1,永远不报缺件),
            // 所以带 body_min_sites 兜底,退回绝对判据。
            const bool enoughSites = static_cast<int>(sites.size()) >= params.bodyMinSites;
            const bool useRatio    = byBody && haveBodyPct && enoughSites;
            const bool useBodyAbs  = byBody && haveBodyPct && !enoughSites;
            const double value = useRatio  ? bodyPct / (std::max)(1e-9, bodyMedian)
                             : useBodyAbs ? bodyPct
                             :              darkPct;
            const double threshold = useRatio  ? params.bodyRatioMin
                             : useBodyAbs ? params.bodyMinPct
                             :              params.darkMinPct;

            res.metrics.push_back({label, "dark_pct", darkPct, params.darkMinPct, "% ROI"});
            // unit 里写清这次走的是哪条路。归一化之后这三行的数字**不可比**。
            std::string unit = useRatio  ? "比同板中位数"
                            : useBodyAbs ? "% ROI(非板面,站点不足 " +
                                           fmtInt(params.bodyMinSites) + " 走绝对)"
                            :              "% ROI(暗)";
            res.metrics.push_back({label, rule::kMissing, value, threshold, unit});
            if (value < threshold) {
                std::string detail;
                if (useRatio) {
                    detail = "框内非板面像素占同板中位数的 " + fmt(value, 3) +
                             " < 阈值 " + fmt(threshold, 3) +
                             "(本体疑似被擦除;body_pct " + fmt(bodyPct, 1) +
                             "%,同板中位数 " + fmt(bodyMedian, 1) + "%)";
                } else if (useBodyAbs) {
                    detail = "框内非板面像素占 ROI " + fmt(value, 2) + "% < 阈值 " +
                             fmt(threshold, 2) + "%(本体疑似被擦除;本板站点数 " +
                             fmtInt(static_cast<long long>(sites.size())) + " < " +
                             fmtInt(params.bodyMinSites) +
                             ",比值不可靠,退回绝对判据)";
                } else {
                    detail = "暗像素 " + fmtInt(static_cast<long long>(darkCount)) +
                             "(占 ROI " + fmt(value, 2) + "%)< 阈值 " + fmt(threshold, 2) +
                             "%(元件疑似被擦除;暗 = V < " + fmt(params.darkVMul, 2) +
                             " x 板面V,板面 BGR " + fmtInt(boardBase[0]) + "," +
                             fmtInt(boardBase[1]) + "," + fmtInt(boardBase[2]) + ")";
                }
                res.defects.push_back({label, "missing", detail});
                cv::rectangle(res.marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- wrong: 本体上错色 ----------
        if (hasRule(rule::kWrong)) {
            const cv::Mat offMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                return pcbcolor::isOffColor(p, boardBase, params.offColorMul,
                                            params.offColorRMax);
            });
            const double yellowPct = 100.0 * cv::countNonZero(offMask) / roiArea;
            res.metrics.push_back({label, rule::kWrong, yellowPct,
                                   params.yellowMinPct, "% ROI"});
            if (yellowPct > params.yellowMinPct) {
                res.defects.push_back({label, "wrong",
                    "错色像素占 ROI " + fmt(yellowPct, 2) + "% > 阈值 " +
                    fmt(params.yellowMinPct, 2) + "%(应为黑色电阻;判据 B,G >= " +
                    fmt(params.offColorMul, 2) + " x 板面 且 R <= " +
                    fmt(params.offColorRMax, 2) + " x 板面,板面 BGR " +
                    fmtInt(boardBase[0]) + "," + fmtInt(boardBase[1]) + "," +
                    fmtInt(boardBase[2]) + ")"});
                cv::rectangle(res.marked, roi, cv::Scalar(0, 0, 255), 3);
            }
        }

        // ---------- polarity: 极性条在不在该在的那一侧 ----------
        //
        // ⚠⚠ 原来的判据(白丝印质心横向位置 > 60%)**只能抓到一半的极性反**,
        //   而且它漏掉的那一半是**结构性的**,调阈值调不出来:
        //
        //   · 判据是**单边**的(只在 cxPct > polarityRatio 时报),
        //     而"反装"是把极性条挪到**另一边** ——
        //     挪到右边撞上单边阈值(抓得到),挪到左边根本不看那边(抓不到)。
        //   · 直觉上改成双边,实测**没有可用的容差**:
        //     不误报 power D1(好板 41.84) 要容差 < 8.16,
        //     抓到 signal D1(废板 29.87) 要容差 > 20.13 —— 8.16 < T < 20.13 无解。
        //
        //   根因**不是阈值,是量错了东西**:
        //   原来量的是"ROI 里所有白丝印的质心",而极性条只是其中一小条
        //   (4~7px 宽)。旁边还有丝印外框(1px 细线)、二极管符号线、
        //   电解电容的十字线 —— 全是白的,全在抢这个质心。
        //   实测:signal 板上**没有极性条**的电容,质心正好 50.0%
        //   (丝印外框对称,把质心拽回中间)。
        //
        // 现在量的是**极性条本身**:
        //   ① 白丝印掩膜
        //   ② **腐蚀**掉 1px 的细线(丝印外框/符号线/十字线全灭,
        //      4~7px 宽的极性条完整留下)—— W3 形态学的老本行
        //   ③ 连通域取最大块 = 极性条
        //   ④ 它完全落在 ROI 中线的哪一侧
        //
        // "应该在哪一侧"由**配方**给(site.polaritySide):
        // 哪边是对的取决于板的设计(电容负极标左、二极管阴极标右),
        // 生成器知道,配方带下来。**判据不猜** ——
        // 猜错就是把好板判成反装,那是静默误报,比漏检更难查。
        if (hasRule(rule::kPolarity)) {
            if (s.polaritySide.empty()) {
                // 配方没写期望侧 -> 不知道该往哪边比 -> 不判定。
                // 这跟"查过了没问题"是两回事,必须报出来。
                res.skipped.push_back({label, rule::kPolarity,
                    "配方没写这个元件的极性条应该在哪一侧,没基准可比,本规则不判定"});
            } else {
                // ① 白丝印掩膜
                const cv::Mat whiteMask = maskBy(img(roi), [&](const cv::Vec3b& p) {
                    return pcbcolor::isWhiteSilk(p, boardBase, params.brightVMul,
                                                 params.neutralMax);
                });
                // ② 腐蚀:1px 细线全灭,4~7px 的极性条留下
                cv::Mat eroded;
                cv::erode(whiteMask, eroded,
                          cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
                // ③ 连通域取最大块
                cv::Mat plabels, pstats, pcentroids;
                const int pn = cv::connectedComponentsWithStats(eroded, plabels, pstats,
                                                               pcentroids);
                int best = -1, bestArea = 0;
                for (int i = 1; i < pn; ++i) {
                    const int a = pstats.at<int>(i, cv::CC_STAT_AREA);
                    if (a > bestArea) { bestArea = a; best = i; }
                }
                if (best < 0) {
                    res.skipped.push_back({label, rule::kPolarity,
                        "腐蚀掉丝印细线后 ROI 里没有实心白块 —— 读不到极性条,本规则不判定"});
                } else {
                    const int bl = pstats.at<int>(best, cv::CC_STAT_LEFT);
                    const int bw = pstats.at<int>(best, cv::CC_STAT_WIDTH);
                    const int bh = pstats.at<int>(best, cv::CC_STAT_HEIGHT);
                    const int mid = roi.width / 2;
                    // ④ 块必须**完全**落在中线一侧。
                    //    跨中线说明位置说不清 -> 不猜(宁可不判,也不判错)。
                    std::string got;
                    if (bl + bw <= mid)      got = "left";
                    else if (bl >= mid)       got = "right";
                    if (got.empty()) {
                        res.skipped.push_back({label, rule::kPolarity,
                            "极性条横跨 ROI 中线,说不清在哪一侧,本规则不判定"});
                    } else {
                        // 读数 = 极性条中心相对 ROI 中线的**带符号**偏移。
                        // 负=偏左,正=偏右。调阈值/看板上表现时用这个。
                        const double centre = bl + bw / 2.0;
                        const double offsetPct =
                            roi.width > 0 ? 100.0 * (centre - mid) / roi.width : 0.0;
                        res.metrics.push_back({label, rule::kPolarity, offsetPct, 0.0,
                            std::string("% ROI 宽(带符号,负=偏左 正=偏右;期望在") +
                            (s.polaritySide == "left" ? "左" : "右") + ")"});
                        if (got != s.polaritySide) {
                            res.defects.push_back({label, "polarity",
                                std::string("极性条在") + (got == "left" ? "左" : "右") +
                                "边,期望在" + (s.polaritySide == "left" ? "左" : "右") +
                                "边(极性反;实测 " + fmtInt(bw) + "x" + fmtInt(bh) +
                                " 实心块,中心相对中线 " + fmt(offsetPct, 1) + "%)"});
                            cv::rectangle(res.marked, roi, cv::Scalar(255, 0, 255), 3);
                        }
                    }
                }
            }
        }

        // ---------- bridge: 焊盘间金条连片 ----------
        //
        // ⚠⚠ 这条判据有个**结构性的适用范围**:它量的是"ROI 里最长一行
        //   的金像素个数"。只要有一条正常的金走线从元件 ROI 里横穿过去,
        //   好板这一行就会占满整个宽度 —— 任何阈值都分不开。
        //   所以 bridge 只对"ROI 里没有走线横穿"的元件成立。
        if (hasRule(rule::kBridge)) {
            cv::Mat roiMask = goldMask(roi);
            int maxWidth = 0, maxY = 0;
            for (int y = 0; y < roiMask.rows; ++y) {
                int w = cv::countNonZero(roiMask.row(y));
                if (w > maxWidth) { maxWidth = w; maxY = y; }
            }
            const double runPct = 100.0 * maxWidth / roi.width;
            res.metrics.push_back({label, rule::kBridge, runPct,
                                   params.bridgeMinPct, "% ROI 宽(最长金段)"});
            if (runPct > params.bridgeMinPct) {
                res.defects.push_back({label, "bridge",
                    "最长金段占 ROI 宽 " + fmt(runPct, 1) + "% > 阈值 " +
                    fmt(params.bridgeMinPct, 1) + "%(焊桥)@y=" + fmtInt(maxY)});
                cv::rectangle(res.marked, roi, cv::Scalar(0, 140, 255), 3);
            }
        }

        // ---------- insufficient: 某焊盘面积远小于中位数 ----------
        if (hasRule(rule::kInsufficient)) {
            cv::Mat roiMask = goldMask(roi);
            cv::Mat labels, stats, centroids;
            int n = cv::connectedComponentsWithStats(roiMask, labels, stats, centroids);
            std::vector<int> areas;
            for (int i = 1; i < n; ++i) {
                int a = stats.at<int>(i, cv::CC_STAT_AREA);
                // 过滤边缘噪声。门槛是 ROI 面积的比例 —— 原来是裸的 80px,
                // 图一小,正常焊盘就掉到门槛以下被滤光,判定必然失效。
                if (100.0 * a / roiArea >= params.padMinPct) areas.push_back(a);
            }
            if (areas.size() >= 4) {
                std::sort(areas.begin(), areas.end());
                const int median = areas[areas.size() / 2];
                const int minArea = areas.front();
                const double dropPct =
                    median > 0 ? 100.0 * (median - minArea) / median : 0.0;
                if (median > 0) {
                    res.metrics.push_back({label, rule::kInsufficient, dropPct,
                                           params.padDropPct, "% (最小焊盘低于中位数)"});
                }
                if (median > 0 && dropPct > params.padDropPct) {
                    res.defects.push_back({label, "insufficient",
                        "最小焊盘 " + fmtInt(minArea) + "px² 比中位数 " +
                        fmtInt(median) + "px² 低 " + fmt(dropPct, 1) + "% > 阈值 " +
                        fmt(params.padDropPct, 1) + "%(焊少)"});
                    cv::rectangle(res.marked, roi, cv::Scalar(255, 200, 0), 3);
                }
            } else {
                // 配方点名要查,但这块 ROI 的金层凑不出 4 个连通域,
                // 取不了中位数 -> 规则**整条没跑**,却照样 PASS。
                res.skipped.push_back({label, rule::kInsufficient,
                    "金层连通域只有 " + fmtInt(static_cast<long long>(areas.size())) +
                    " 个(< 4),取不了焊盘中位数,本规则不判定"});
            }
        }

        // ---------- shifted: 本体质心偏离应站中心 ----------
        //
        // 期望质心就是 ROI 的几何中心,不需要一张"每个元件该长什么样"的表。
        // 以前 EXPECTS[6] 里 R2 写死的 (60,24) 正是 120x48 这个 ROI 的中心
        // —— 同一个数,只是被抄了一份,抄错了也不会有人发现。
        // ROI 是由元件本体框四周等量扩出来的,所以两者恒等。
        if (hasRule(rule::kShifted)) {
            cv::Moments m = cv::moments(darkMask, true);
            const double darkPct = 100.0 * m.m00 / roiArea;
            // m.m00 > 0 必须单独判:dark_min_pct 被调到 0 时,
            // "暗像素占比 >= 0%" 对**一张全黑都没有的 ROI 也成立**,
            // 于是下面 m.m10/m.m00 变成 0/0 = NaN。
            // NaN 和任何数比较都是 false,判定会静默失效 —— 又是一个不报错的错。
            if (m.m00 > 0 && darkPct >= params.darkMinPct) {   // 没本体就交给 missing 判
                const double expCx = roi.width  * 0.5;
                const double expCy = roi.height * 0.5;
                const double cx = m.m10 / m.m00, cy = m.m01 / m.m00;
                const double dx = cx - expCx, dy = cy - expCy;
                const double tolX = roi.width  * cShiftX / 100.0;
                const double tolY = roi.height * cShiftY / 100.0;
                // 取 x/y 两个方向里更接近阈值的那一个来报:
                // 偏移量本身没有"正常/异常"的绝对值,只有相对容差的大小。
                const double over = (std::max)(std::abs(dx) / (std::max)(tolX, 1e-9),
                                             std::abs(dy) / (std::max)(tolY, 1e-9));
                res.metrics.push_back({label, rule::kShifted, over, 1.0,
                                       "倍(>1 即超容差)"});
                if (std::abs(dx) > tolX || std::abs(dy) > tolY) {
                    res.defects.push_back({label, "shifted",
                        "质心(" + fmtInt(static_cast<long long>(cx)) + "," +
                        fmtInt(static_cast<long long>(cy)) + ") 偏离 ROI 中心(" +
                        fmtInt(static_cast<long long>(expCx)) + "," +
                        fmtInt(static_cast<long long>(expCy)) + ") 超容差(" +
                        fmtInt(static_cast<long long>(tolX)) + "," +
                        fmtInt(static_cast<long long>(tolY)) + ")"});
                    cv::rectangle(res.marked, roi, cv::Scalar(0, 255, 255), 3);
                }
            } else {
                // "没本体就交给 missing 判"是对的,但**这条规则确实没跑**,
                // 仍然要报出来。否则"交给 missing"就变成了静默跳过的借口。
                res.skipped.push_back({label, rule::kShifted,
                    "ROI 里暗像素只占 " + fmt(darkPct, 2) + "%(低于 " +
                    fmt(params.darkMinPct, 2) + "%),没有本体可算质心,"
                    "本规则不判定(缺件由 missing 负责)"});
            }
        }

        // ---------- short: 两个端子被金属连通(短路) ----------
        //
        // 短路 = **这个元件的两个端子之间,绝缘缝里出现了金属**。
        //
        // ⚠⚠ 为什么不是"全图扫一块蓝斑,看它像不像特定大小的圆环":
        //   旧判据量的是**生成器画给人眼看的标记圈**(蓝紫圆环),
        //   而那**不是短路**。实测两个方向都错:
        //     power_short.png(真短路,画的是一条金条)-> 整板 PASS(漏检)
        //     mcu_bridge.png(焊桥,画的是焊锡连片)  -> 多报一条"短路"(误报)
        //   病根是**量错了对象**,不是阈值没调好 —— 所以旧的那六个
        //   旋钮(面积/宽/高的上下限)一起删掉了,不是调它们。
        //
        // 量的东西:两个端子中心连线,掐掉两端各一个焊盘半宽
        // (焊盘本来就是金属,不是缺陷),剩下那段"绝缘缝"逐点采样,
        // 看有没有金属。焊盘位置由**生成器**登记、配方带下来
        // (见 RecipeSite::pads)—— 判据不猜。
        if (hasRule(rule::kShort)) {
            if (s.pads.size() != 2) {
                // 配方没登记端子(或登记的不是两个) -> 没有可比对的基准。
                // 这跟"查过了没问题"是两回事,必须报出来。
                res.skipped.push_back({label, rule::kShort,
                    "配方没有登记这个元件的两个端子位置(实际登记了 " +
                    fmtInt(static_cast<long long>(s.pads.size())) +
                    " 个),判不出\"两脚之间有没有金属\",本规则不判定"});
            } else {
                // ⚠⚠ **前置条件:元件本体的颜色不能落在金属色带里。**
                //
                //   为什么:生成器的调色板常量是 RGB 顺序但注释按 BGR 写
                //   (见 gen_pcb_dataset.py 的调色板段),于是"电容黄"
                //   实际渲染成青蓝色,落在金层色带 H[85,115] 里 ——
                //   和铜箔几乎同色(H 94.7 vs 96.3),分不开。
                //   这种元件的"绝缘缝"本来就是金属色,缝里有没有金属
                //   根本判不出来,硬判就是**每一块好板都报短路**。
                //
                //   为什么判据**不自己查**:本体中心那块像素,在好板上是
                //   本体,在**缺陷图上是那根短路铜条** —— 两种情况长得
                //   一模一样,靠图像分不出来(这坑实测踩过一次:
                //   守门把"短路铜条盖住本体中心"当成了"本体是金属色",
                //   于是漏检又回来了)。
                //
                //   所以这条前置条件由**配方层**保证:
                //     · 谁给哪个元件配 short —— tools/export_recipes.py
                //       的 CHECKS 表(那里写着每个封装的本体是什么颜色);
                //     · 配了以后对不对 —— 回归测试在**好板**上量本体
                //       中心的颜色,断言它不在金属色带里
                //       (见 common/pcb_core/tests/test_pcb_core.cpp)。
                //   配错了不会静默:好板会整片报短路,测试当场红。
                const Pad& pa = s.pads[0];
                const Pad& pb = s.pads[1];
                const double dx = static_cast<double>(pb.x) - pa.x;
                const double dy = static_cast<double>(pb.y) - pa.y;
                const double len = std::sqrt(dx * dx + dy * dy);
                // 掐掉两端各一个焊盘半宽 —— 剩下那段才是"绝缘缝"。
                // ⚠ 再各多掐 1px:焊盘自己带 1px 描边,那圈描边**也可能是金属**
                //   (现在这批图的描边是暗金色,不在金属色带里,所以掐不掐读数
                //   一样,实测都是 0% —— 原来这条注释写的是"不掐会变成 4.8%",
                //   实测发现那是**错的**,注释改成了实话)。
                //   留这 1px 不是为了现在的读数,是为了**不依赖描边的颜色**:
                //   哪天描边变亮了,量到的仍然是"缝",不是焊盘自己。
                //   真出事了不用改判据 —— 回归测试里"好板读数必须恰好 0"
                //   那条会当场红。
                const double ra = pa.r + 1, rb = pb.r + 1;
                if (len <= 0.5 || ra + rb >= len - 1.0) {
                    res.skipped.push_back({label, rule::kShort,
                        "两个端子几乎贴在一起(间距 " + fmt(len, 1) +
                        "px,焊盘半宽合计 " + fmt(ra + rb, 1) +
                        "px),没有可测的绝缘缝,本规则不判定"});
                } else {
                    const double ux = dx / len, uy = dy / len;
                    const double ax = pa.x + ux * ra, ay = pa.y + uy * ra;
                    const double bx = pb.x - ux * rb, by = pb.y - uy * rb;
                    const double glen = std::sqrt((bx - ax) * (bx - ax) +
                                                 (by - ay) * (by - ay));
                    const int n = (std::max)(1, static_cast<int>(std::lround(glen)));
                    // 垂直方向的取样半宽:容一点"画歪了"的余量,
                    // 但要小到不会碰到旁边横穿的走线。
                    const int half = (std::max)(1, static_cast<int>(
                        std::lround(0.2 * (std::min)(ra, rb))));
                    int metal = 0;
                    for (int i = 0; i < n; ++i) {
                        const double t = (n == 1) ? 0.0
                            : static_cast<double>(i) / (n - 1);
                        const double px = ax + (bx - ax) * t;
                        const double py = ay + (by - ay) * t;
                        for (int k = -half; k <= half; ++k) {
                            const int qx = static_cast<int>(std::lround(px - uy * k));
                            const int qy = static_cast<int>(std::lround(py + ux * k));
                            if (qx < 0 || qy < 0 ||
                                qx >= img.cols || qy >= img.rows) continue;
                            if (goldMask.at<uchar>(qy, qx) != 0) { ++metal; break; }
                        }
                    }
                    const double pct = 100.0 * metal / n;
                    res.metrics.push_back({label, rule::kShort, pct,
                                           params.shortGapMinPct,
                                           "% 端子间隙(有金属的采样点)"});
                    if (pct >= params.shortGapMinPct) {
                        res.defects.push_back({label, "short",
                            "两个端子 (" + fmtInt(pa.x) + "," + fmtInt(pa.y) +
                            ")-(" + fmtInt(pb.x) + "," + fmtInt(pb.y) +
                            ") 之间的绝缘缝里 " + fmtInt(metal) + "/" +
                            fmtInt(n) + " 个采样点是金属(" + fmt(pct, 1) +
                            "%),超过阈值 " + fmt(params.shortGapMinPct, 1) +
                            "%(两脚被金属连通,短路)"});
                        cv::line(res.marked, cv::Point(pa.x, pa.y),
                                 cv::Point(pb.x, pb.y), cv::Scalar(0, 0, 255), 3);
                    }
                }
            }
        }
    }

    return res;
}

} // namespace pcbcore
