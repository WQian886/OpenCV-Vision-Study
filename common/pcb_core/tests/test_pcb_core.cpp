// ============================================================
// common/pcb_core 的无头回归测试
//
// ============================================================
//
// ## 为什么这份测试必须存在
//
// 2026-10-20,W7 的判定逻辑搬到了 common/pcb_core/,让 W6 和 W7 共用一份。
// 搬的过程中**新写了两块代码**,而它们当时一行测试都没有:
//
//   1. mini_json —— 200 行手写 JSON 解析器。
//      解析器是典型的"平时对、边界炸"的东西:一个逗号位置算错,
//      表现是**静默解析出错的配方**,而配方错了 → 判定输出"零缺陷 PASS"
//      → 废板判合格。整条链上没有任何一处会报错。
//
//   2. parseRecipeJson 的 6 类错误分支。
//      W7 原来那份 board_recipe.cpp 里也有同样的校验,但**同样没被测过**
//      (grep 过:现有套件只走 happy path)。
//
// 这份测试把它们逐条钉住。W7 那边还**多一层**价值:那边是"算子外壳 +
// 核心"一起测(外壳可能把 body_pct 之类的字段在搬运时弄丢),
// 这边是**直接调核心** —— 证明核心能脱离 Qt 单独用(这正是 W6 的用法)。
//
// ## 和 W7 套件的分工
//
//   W7 的 pcb_operators : 算子外壳(QJson ↔ 结构体)+ 核心,端到端
//   这份               : 核心直接调,没有外壳
//
// 两边都绿才说明"外壳没把核心弄坏,而且核心自己是对的"。
//
// ## 用例清单从哪来
//
// **不抄一份**。demo 板读 W6 的 images/manifest.csv,
// complex 板读 images/complex/complex_manifest.csv ——
// 那两张表本来就是真值表。加一张新图、改一条期望,测试自动跟着变。
//
// ============================================================

#include "pcb_core.h"
#include "mini_json.h"

#include <opencv2/opencv.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace pcbcore;

// ============================================================
// 极简断言(与 W7 各套件同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static std::string g_case;

static void beginCase(const std::string& name) {
    g_case = name;
    std::cout << "\n=== " << name << " ===" << std::endl;
}

static void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::cout << "  [PASS] " << what << std::endl;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << what << "  (case:" << g_case << ")" << std::endl;
    }
}

// ============================================================
// 找仓库根
//
// 同一个测试文件会被链进 W6 和 W7 两个目标,两个目标的 ctest
// 工作目录不同(CWD = 各自源码目录)。所以**不能写死相对路径** ——
// 写死的话,在其中一个项目里跑就会"找不到文件",而找不到文件时
// 断言会红(而不是空转),但红的原因很难看懂。
//
// 往上找,直到看见 W6_PCB_DefectProject 和 W7_VisionMasterLite。
// ============================================================
static fs::path findRepoRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "W6_PCB_DefectProject") &&
            fs::exists(p / "W7_VisionMasterLite")) {
            return p;
        }
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return fs::current_path();
}

static fs::path g_root;

// ============================================================
// 极简 CSV 读(真值表里没有引号和逗号转义)
// ============================================================
static std::vector<std::vector<std::string>> readCsv(const fs::path& p) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(p);
    if (!f) return rows;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> cells;
        std::string cur;
        std::istringstream ss(line);
        while (std::getline(ss, cur, ',')) cells.push_back(cur);
        rows.push_back(cells);
    }
    return rows;
}

// ============================================================
// 用例 1:mini_json
// ============================================================
//
// 解析器最容易出的三类错:
//   (a) **位置算错** —— 报错信息指向错误的字符位置(难查,但不致命)
//   (b) **静默解析错** —— 把坏 JSON 解析成"看起来对"的值(致命)
//   (c) **边界** —— 空容器、转义、UTF-8、深嵌套
//
// (b) 是重点:解析器返回 false 时上层会报"配方非法",这是好事;
// 解析器**返回 true 但值错了**,那上层拿着错的配方往下跑,
// 报出来的结论是编的。所以每条 happy path 都要**核对值**,
// 不能只 check(!failed)。
// ============================================================
static void test_mini_json() {
    beginCase("mini_json:值对不对(不是“能解析”就行)");

    // --- 基本类型 ---
    {
        mini_json::Value v;
        std::string err;
        const bool ok = mini_json::parse(
            R"({"s":"abc","n":42,"f":1.5,"neg":-7,"b":true,"nul":null})", &v, &err);
        check(ok, "正常对象能解析" + (ok ? "" : ("(" + err + ")")));
        if (ok) {
            check(v.isObject(), "顶层是对象");
            check(v.find("s") && v.find("s")->asString() == "abc", "字符串值对");
            check(v.find("n") && v.find("n")->asNumber() == 42.0, "整数对");
            check(v.find("f") && v.find("f")->asNumber() == 1.5, "小数对");
            check(v.find("neg") && v.find("neg")->asNumber() == -7.0, "负数对");
            check(v.find("b") && v.find("b")->asBool(), "布尔对");
            check(v.find("nul") && v.find("nul")->isNull(), "null 对");
            check(v.find("不存在") == nullptr, "不存在的键返回 nullptr(不是默认值)");
        }
    }

    // --- 数组与嵌套 ---
    {
        mini_json::Value v;
        std::string err;
        const bool ok = mini_json::parse(
            R"({"a":[1,2,{"b":"c"}],"empty":[],"eo":{}})", &v, &err);
        check(ok, "嵌套数组/对象能解析");
        if (ok) {
            check(v.find("a") && v.find("a")->asArray().size() == 3, "数组长度对");
            check(v.find("a") && v.find("a")->at(2).isObject(),
                  "数组第 3 项是对象");
            check(v.find("a") && v.find("a")->at(2).find("b") &&
                  v.find("a")->at(2).find("b")->asString() == "c",
                  "嵌套两层的字符串取得到");
            check(v.find("empty") && v.find("empty")->asArray().empty(),
                  "空数组是空数组(不是解析失败)");
            check(v.find("eo") && v.find("eo")->size() == 0, "空对象是空对象");
        }
    }

    // --- UTF-8:配方里就有中文标题 ---
    {
        mini_json::Value v;
        std::string err;
        const bool ok = mini_json::parse(
            "{\"t\":\"\xE7\x94\xB5\xE6\xBA\x90\xE7\xA8\xB3\xE5\x8E\x8B\xE6\x9D\xBF\"}", &v, &err);
        check(ok, "UTF-8 中文能解析");
        if (ok) {
            check(v.find("t") && v.find("t")->asString() ==
                  "\xE7\x94\xB5\xE6\xBA\x90\xE7\xA8\xB3\xE5\x8E\x8B\xE6\x9D\xBF",
                  "中文原样进出(字节不被改写)");
        }
    }

    // --- \uXXXX 转义 → UTF-8 ---
    {
        mini_json::Value v;
        std::string err;
        const bool ok = mini_json::parse(R"({"t":"电"})", &v, &err);
        check(ok, "\\uXXXX 转义能解析");
        if (ok) {
            check(v.find("t") && v.find("t")->asString() ==
                  "\xE7\x94\xB5", "\\uXXXX 转成 UTF-8 字节(电)");
        }
    }

    // --- 转义字符 ---
    {
        mini_json::Value v;
        std::string err;
        const bool ok = mini_json::parse(R"({"t":"a\"b\\c\nd\te"})", &v, &err);
        check(ok, "转义字符能解析");
        if (ok) {
            check(v.find("t") && v.find("t")->asString() == "a\"b\\c\nd\te",
                  "引号/反斜杠/换行/制表符转义对");
        }
    }

    // --- 非法 JSON:必须全部返回 false ---
    //
    // 这些是最要紧的:**解析器必须拒绝**,不能"尽力而为地解析出半个东西"。
    // 返回 true 但值错 -> 上层拿着错配方跑 -> 结论是编的。
    {
        struct Bad { const char* text; const char* what; };
        const Bad bads[] = {
            {"",                      "空文本"},
            {"{",                     "只有左括号"},
            {"}",                     "只有右括号"},
            {"{\"a\":1,}",            "尾随逗号"},
            {"{\"a\" 1}",             "缺冒号"},
            {"{a:1}",                "键没加引号"},
            {"[1,2",                 "数组没闭合"},
            {"{\"a\":1} extra",       "结尾有多余字符"},
            {"{\"a\":}",              "值缺失"},
            {"{'a':1}",              "用了单引号"},
            {"{\"a\":1,}",            "对象尾随逗号"},
            {"{\"a\":\"\\x",         "字符串没闭合"},
            {"{\"a\":tru}",           "布尔值不完整"},
            {"{\"a\":01}",            "数字有前导零"},
            {"{\"a\":1x}",            "数字后面接了字母"},
        };
        for (const auto& b : bads) {
            mini_json::Value v;
            std::string err;
            const bool ok = mini_json::parse(b.text, &v, &err);
            check(!ok, std::string("非法 JSON 被拒绝:") + b.what);
            if (!ok) {
                check(!err.empty(),
                      std::string("报错信息不能是空的:") + b.what);
            }
        }
    }

    // --- 报错信息要指向**位置** ---
    //
    // 用户改配方时靠这句话定位是哪一处写错了。
    // 只说"解析失败"等于没说 —— 配方有几十行,人得自己一行行试。
    {
        mini_json::Value v;
        std::string err;
        mini_json::parse("{\"a\":1,\n\"b\":}", &v, &err);
        const size_t pos = err.find("第 ");
        const size_t num = err.find(" 个字符处");
        check(pos != std::string::npos && num != std::string::npos && num > pos,
              "报错要带字符位置(实际 \"" + err + "\")");
    }
}

// ============================================================
// 用例 2:配方校验
// ============================================================
//
// 这一组是这个仓库最关心的那类东西:**静默失效**。
//
// 配方写坏了 —— 比如规则名拼错、位号重复、sites 是空的 ——
// 后果不是"报错",而是:
//   规则一条都不跑 -> defects 为空 -> 判定输出 PASS -> 废板判合格。
// 所以**每一个"加载成功"的路径都必须真的正确**,
// 每一个"加载失败"的路径都必须真的失败。
// ============================================================
static void test_recipe_validation() {
    beginCase("配方校验:坏配方必须加载失败(不然会报假 PASS)");

    // --- 一条最小可用配方 ---
    const std::string kGood = R"({
        "name": "t", "title": "测试板", "ref_w": 1000, "ref_h": 700, "buffer": 10,
        "sites": [
            {"label":"R1","kind":"RES","cx":150,"cy":200,"body_w":100,"body_h":28,
             "checks":["missing","shifted"],"shift_tol_pct":15.0},
            {"label":"C1","kind":"CAP_SMALL","cx":400,"cy":200,"body_w":70,"body_h":28,
             "checks":[]}
        ]})";

    {
        BoardRecipe r;
        std::string reason;
        const bool ok = parseRecipeJson(kGood, &r, &reason);
        check(ok, "合法配方能加载" + (ok ? "" : ("(" + reason + ")")));
        if (ok) {
            check(r.name == "t", "name 对");
            check(r.title == "测试板", "title 对(UTF-8)");
            check(r.refW == 1000 && r.refH == 700, "ref_w/ref_h 对");
            check(r.buffer == 10, "buffer 对");
            check(r.sites.size() == 2, "2 个站点");
            if (r.sites.size() == 2) {
                check(r.sites[0].label == "R1" && r.sites[0].kind == "RES",
                      "站点 0 的 label/kind 对");
                check(r.sites[0].cx == 150 && r.sites[0].cy == 200, "站点 0 中心对");
                check(r.sites[0].bodyW == 100 && r.sites[0].bodyH == 28, "站点 0 尺寸对");
                check(r.sites[0].checks.size() == 2, "站点 0 有 2 条规则");
                check(r.sites[0].hasShiftTol() && r.sites[0].shiftTolPct == 15.0,
                      "站点 0 的 shift_tol_pct 对");
                // ⚠ "checks 是空数组"和"checks 字段不存在"是两件不同的事。
                //   配方明确写 [] = "这个元件我不管它"。
                check(r.sites[1].checks.empty(), "站点 1 的 checks 是空数组");
            }
        }
    }

    // --- 必填字段缺失 ---
    {
        struct Miss { const char* field; const char* json; const char* what; };
        const Miss miss[] = {
            {"name",    R"({"ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10}]})", "缺 name"},
            {"ref_w",   R"({"name":"t","ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10}]})", "缺 ref_w"},
            {"ref_h",   R"({"name":"t","ref_w":1000,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10}]})", "缺 ref_h"},
            {"sites",   R"({"name":"t","ref_w":1000,"ref_h":700})", "缺 sites"},
            {"label",   R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10}]})", "站点缺 label"},
            {"kind",    R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","cx":1,"cy":1,"body_w":10,"body_h":10}]})", "站点缺 kind"},
            {"cx",      R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cy":1,"body_w":10,"body_h":10}]})", "站点缺 cx"},
            {"cy",      R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"body_w":10,"body_h":10}]})", "站点缺 cy"},
            {"body_w",  R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_h":10}]})", "站点缺 body_w"},
            {"body_h",  R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10}]})", "站点缺 body_h"},
        };
        for (const auto& m : miss) {
            BoardRecipe r;
            std::string reason;
            const bool ok = parseRecipeJson(m.json, &r, &reason);
            check(!ok, std::string("缺字段必须失败:") + m.what);
            if (!ok) {
                check(reason.find(m.field) != std::string::npos,
                      std::string("报错要点名缺哪个字段:") + m.what +
                      "(实际 \"" + reason + "\")");
            }
        }
    }

    // --- 结构性错误 ---
    {
        struct Bad { const char* json; const char* what; const char* must; };
        const Bad bads[] = {
            // ⚠ 空站点表比缺字段更危险:它不报错,判定会输出
            //   "0 个元件,零缺陷,PASS" —— 什么都没查却报合格。
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[]})",
             "sites 是空数组", "空数组"},
            // ⚠ 规则名拼错 = 那条规则一条都不会跑,而且不报错。
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10,"checks":["misssing"]}]})",
             "规则名拼错", "没实现"},
            // 位号重复 -> 报告里出现两个 R1,排查时成了猜谜
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[
                {"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10},
                {"label":"R1","kind":"RES","cx":2,"cy":2,"body_w":10,"body_h":10}]})",
             "位号重复", "重复"},
            // 本体尺寸 <= 0 -> ROI 面积为 0 -> 判定环节 continue 掉
            //   -> 又一次"没查却报合格"
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":0,"body_h":10}]})",
             "body_w 为 0", "必须为正"},
            // 中心超出参考图 -> 这个站点在这块板上根本不存在
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":9999,"cy":1,"body_w":10,"body_h":10}]})",
             "中心超出参考图", "不在参考图"},
            // 坐标是小数 -> 像素坐标必须是整数
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1.5,"cy":1,"body_w":10,"body_h":10}]})",
             "cx 是小数", "整数"},
            // shift_tol_pct 越界
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10,"shift_tol_pct":0}]})",
             "shift_tol_pct 为 0", "不合理"},
            {R"({"name":"t","ref_w":1000,"ref_h":700,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10,"shift_tol_pct":80}]})",
             "shift_tol_pct 为 80", "不合理"},
            // buffer 负数
            {R"({"name":"t","ref_w":1000,"ref_h":700,"buffer":-5,"sites":[{"label":"R1","kind":"RES","cx":1,"cy":1,"body_w":10,"body_h":10}]})",
             "buffer 为负", "负数"},
            // 顶层不是对象
            {R"([1,2,3])", "顶层是数组", "不是对象"},
            // JSON 本身非法
            //
            // ⚠ 这里**不能**写成 R"({"name":)" —— 原始字符串字面量的
            //   结束符就是 )",内容会被截成 `{"name":`,
            //   于是测的其实是"截断"而不是"括号不配对"。
            //   这种错误测出来是绿的,测的东西却不是想测的 ——
            //   断言恒真比没有还坏。
            {"{\"name\":)", "JSON 括号不配对", "合法"},
            {"{\"name\":", "JSON 截断", "结束了"},
        };
        for (const auto& b : bads) {
            BoardRecipe r;
            std::string reason;
            const bool ok = parseRecipeJson(b.json, &r, &reason);
            check(!ok, std::string("必须失败:") + b.what);
            if (!ok) {
                check(reason.find(b.must) != std::string::npos,
                      std::string("报错要说清原因:") + b.what +
                      "(期望含 \"" + b.must + "\",实际 \"" + reason + "\")");
            }
        }
    }

    // --- polarity_side 的校验 ---
    //
    // ⚠ 这一段是 2026-10-20 补的,补的原因是**变异验证发现的**:
    //   判据里有"polarity_side 写错必须报错"这条守卫,但**没有任何断言**
    //   钉它 —— 把那条守卫删掉,整套测试照样全绿。
    //   一条没人验证的守卫,等于没有守卫。
    //
    // 为什么"写错"必须报错、不能当"没写"处理:
    //   "没写" = 判不了极性 -> 报"没查",至少是诚实的;
    //   写错   = 判据拿着一个看不懂的基准去比 -> 可能把**好板**判成反装。
    //   后者是静默误报,比漏检更难查(漏检会被"没查"暴露出来)。
    {
        // --- 合法值必须真的落到字段上 ---
        //
        // 用普通字符串拼,不用 R"(...)" —— 原始字符串字面量的结束符是 )",
        // 而这里要拼的 JSON 里恰好会出现 )",提前结束就测了个不存在的东西
        // (断言恒真比没有还坏)。
        struct Good { const char* side; };
        const Good goods[] = {{"left"}, {"right"}, {""}};
        for (const auto& g : goods) {
            const std::string json =
                std::string("{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":[")
                + "{\"label\":\"C1\",\"kind\":\"CAP_SMALL\",\"cx\":400,\"cy\":200,"
                + "\"body_w\":70,\"body_h\":28,\"polarity_side\":\"" + g.side + "\"}]}";
            BoardRecipe r;
            std::string reason;
            const bool ok = parseRecipeJson(json, &r, &reason);
            check(ok, std::string("polarity_side=\"") + g.side + "\" 是合法值,必须能加载" +
                       (ok ? "" : ("(" + reason + ")")));
            if (ok) {
                check(!r.sites.empty() && r.sites[0].polaritySide == g.side,
                      std::string("polarity_side=\"") + g.side + "\" 必须落到字段上(实际 \"" +
                      (r.sites.empty() ? std::string("(没站点)") : r.sites[0].polaritySide) + "\")");
            }
        }

        // --- 非法值必须加载失败,而且要说清是哪一条 ---
        //
        // 报错消息要能直接定位到人:哪个站点、写的是什么值。
        struct Bad { const char* json; const char* what; };
        const Bad bads[] = {
            {"{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":["
             "{\"label\":\"C1\",\"kind\":\"CAP_SMALL\",\"cx\":400,\"cy\":200,"
             "\"body_w\":70,\"body_h\":28,\"polarity_side\":\"up\"}]}",
             "polarity_side=\"up\""},
            {"{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":["
             "{\"label\":\"C1\",\"kind\":\"CAP_SMALL\",\"cx\":400,\"cy\":200,"
             "\"body_w\":70,\"body_h\":28,\"polarity_side\":\"Left\"}]}",
             "polarity_side=\"Left\"(大小写不对)"},
            {"{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":["
             "{\"label\":\"C1\",\"kind\":\"CAP_SMALL\",\"cx\":400,\"cy\":200,"
             "\"body_w\":70,\"body_h\":28,\"polarity_side\":123}]}",
             "polarity_side 是数字(不是字符串)"},
        };
        for (const auto& b : bads) {
            BoardRecipe r;
            std::string reason;
            const bool ok = parseRecipeJson(b.json, &r, &reason);
            check(!ok, std::string("必须失败:") + b.what);
            if (!ok) {
                check(reason.find("polarity_side") != std::string::npos,
                      std::string("报错要点名字段:") + b.what +
                      "(实际 \"" + reason + "\")");
                check(reason.find("C1") != std::string::npos,
                      std::string("报错要点名站点:") + b.what +
                      "(实际 \"" + reason + "\")");
            }
        }
    }

    // --- 文件读不到 ---
    {
        BoardRecipe r;
        std::string reason;
        const bool ok = loadRecipeFile("根本没有这个文件.json", &r, &reason);
        check(!ok, "读不到文件必须失败");
        check(!reason.empty(), "读不到文件要说原因");
    }

    // --- knownChecks 与 rule:: 名字必须一致 ---
    //
    // 配方里写规则名,判定里按名字分派。两边对不上 =
    // 那条规则一条都不会跑,而且不报错。
    {
        const auto& known = knownChecks();
        check(known.size() == 6,
              "knownChecks 有 6 条规则(实际 " + std::to_string(known.size()) + ")");
        const char* names[] = {rule::kMissing, rule::kShifted, rule::kWrong,
                               rule::kPolarity, rule::kBridge, rule::kInsufficient};
        for (const char* n : names) {
            bool found = false;
            for (const auto& k : known) {
                if (k == n) { found = true; break; }
            }
            check(found, std::string("knownChecks 里有 ") + n);
        }
    }
}

// ============================================================
// 用例 3:拒绝判定
// ============================================================
//
// 核心有两条"拒绝给结论"的路。这两条要是失效,后果是
// **给出一个没有依据的结论**,而不是"没结论":
//
//   (a) 一个元件都没配规则 -> 规则一条都跑不了 -> defects 为空 -> PASS
//   (b) 撞 8 位色深上限   -> 判据恒假 -> defects 为空 -> PASS
//
// 两者都是"废板判合格",而且都不报错。所以必须专门测。
// ============================================================
static void test_refuse_paths() {
    beginCase("拒绝判定:拿不到依据时不给结论(而不是报 PASS)");

    // --- (a) 没有规则可跑 ---
    {
        cv::Mat img(200, 200, CV_8UC3, cv::Scalar(40, 90, 40));
        InspectSite s;
        s.label = "R1";
        s.kind  = "RES";
        s.roi   = cv::Rect(10, 10, 40, 20);
        s.checksExplicit = true;
        s.checks.clear();          // 配方明确说"这个元件不参与判定"
        s.hasBodyPct = true;
        s.bodyPct = 50.0;

        const JudgeParams p;
        const JudgeResult r = judge(img, {s}, p, cv::Mat(), cv::Vec3b(40, 90, 40));
        check(r.refused, "没有规则可跑 -> 拒绝判定");
        check(!r.refuseReason.empty(), "拒绝时要说原因");
        check(r.defects.empty(), "拒绝时不产出缺陷(空的 defects 会被当成 PASS)");
    }

    // --- (b) 撞 8 位色深上限 ---
    {
        // 板面被照亮到 250:1.6 x 250 = 400 > 255,uint8 里根本不存在
        // 这么亮的像素,错色判据永远返回 false -> 报 PASS。
        cv::Mat img(200, 200, CV_8UC3, cv::Scalar(250, 250, 250));
        InspectSite s;
        s.label = "R1";
        s.kind  = "RES";
        s.roi   = cv::Rect(10, 10, 40, 20);
        s.checksExplicit = true;
        s.checks.push_back(rule::kWrong);
        s.hasBodyPct = true;
        s.bodyPct = 50.0;

        const JudgeParams p;
        const JudgeResult r = judge(img, {s}, p, cv::Mat(), cv::Vec3b(250, 250, 250));
        check(r.refused, "撞 8 位色深上限 -> 拒绝判定");
        check(!r.refuseReason.empty(), "拒绝时要说原因");
        // 报错里要提"曝光",让用户知道是重拍而不是改阈值
        check(r.refuseReason.find("曝光") != std::string::npos,
              "拒绝原因里要点明是曝光问题(实际 \"" + r.refuseReason + "\")");
    }

    // --- 对照:正常亮度下同样的图**要**给结论 ---
    {
        cv::Mat img(200, 200, CV_8UC3, cv::Scalar(40, 90, 40));
        InspectSite s;
        s.label = "R1";
        s.kind  = "RES";
        s.roi   = cv::Rect(10, 10, 40, 20);
        s.checksExplicit = true;
        s.checks.push_back(rule::kWrong);
        s.hasBodyPct = true;
        s.bodyPct = 50.0;

        const JudgeParams p;
        const JudgeResult r = judge(img, {s}, p, cv::Mat(), cv::Vec3b(40, 90, 40));
        check(!r.refused, "正常亮度下**不**拒绝(对照组:否则上面两条能靠“永远拒绝”蒙混)");
    }
}

// ============================================================
// 用例 4:端到端 —— 核心直接调(不经过 Qt 算子)
// ============================================================
//
// 这一组证明核心能脱离 Qt 单独用 —— 这正是 W6 的用法。
// 用例清单**从真值表读**,不抄一份。
// ============================================================
static void test_end_to_end() {
    beginCase("端到端:核心直接调(无 Qt 外壳),用例清单读真值表");

    const fs::path w6 = g_root / "W6_PCB_DefectProject";
    const fs::path recipes = g_root / "W7_VisionMasterLite" / "recipes";
    if (!fs::exists(w6) || !fs::exists(recipes)) {
        check(false, "找得到 W6 数据目录和配方目录(找不到就跳过全部端到端用例)");
        return;
    }

    // --- demo 板:读 images/manifest.csv ---
    {
        const auto rows = readCsv(w6 / "images" / "manifest.csv");
        check(!rows.empty(), "读得到 images/manifest.csv");
        if (!rows.empty()) {
            int fileCol = -1, defectCol = -1, expCol = -1;
            for (size_t i = 0; i < rows[0].size(); ++i) {
                if (rows[0][i] == "file")   fileCol = static_cast<int>(i);
                if (rows[0][i] == "defect") defectCol = static_cast<int>(i);
                if (rows[0][i] == "expected") expCol = static_cast<int>(i);
            }
            check(fileCol >= 0 && defectCol >= 0 && expCol >= 0,
                  "manifest.csv 有 file/defect/expected 三列");

            BoardRecipe recipe;
            std::string reason;
            const bool loaded = loadRecipeFile((recipes / "demo.json").string(),
                                               &recipe, &reason);
            check(loaded, "加载 demo 配方" + (loaded ? "" : ("(" + reason + ")")));

            for (size_t r = 1; r < rows.size() && loaded; ++r) {
                const auto& row = rows[r];
                if (static_cast<int>(row.size()) <= expCol) continue;
                const std::string file = row[fileCol];
                const std::string wantDefect = row[defectCol];
                const std::string wantResult = row[expCol];

                const cv::Mat img =
                    cv::imread((w6 / "images" / file).string(), cv::IMREAD_COLOR);
                if (img.empty()) {
                    check(false, "读得到 " + file);
                    continue;
                }
                const JudgeParams p;
                const InspectResult ir =
                    inspect(img, recipe, pcbcolor::goldBand(p.maskVMul));
                const JudgeResult jr = judge(img, ir.sites, p, ir.goldMask, ir.boardBase);

                const std::string gotResult = jr.refused ? "REFUSED"
                                           : (jr.defects.empty() ? "PASS" : "FAIL");
                check(gotResult == wantResult,
                      file + " 期望 " + wantResult + "(实际 " + gotResult + ")");

                if (wantResult == "FAIL" && wantDefect != "none") {
                    bool hit = false;
                    std::string types;
                    for (const auto& d : jr.defects) {
                        types += d.type + " ";
                        if (d.type == wantDefect) hit = true;
                    }
                    check(hit, file + " 必须命中 " + wantDefect +
                               "(实际: " + types + ")");
                }
            }
        }
    }

    // --- complex 板:读 images/complex/complex_manifest.csv ---
    //
    // ⚠ 两条**已知漏检**写在这里,是为了让它**可见**。
    //   漏检在界面上跟真合格一模一样 —— 光看 result=="PASS" 分不出
    //   "查过了没问题"和"根本没看见"。
    //   哪天判据重做完了,把 knownMiss 改成 false,断言会立刻转红,
    //   提醒你补上"必须命中"的断言。
    {
        const fs::path cdir = w6 / "images" / "complex";
        const auto rows = readCsv(cdir / "complex_manifest.csv");
        check(!rows.empty(), "读得到 images/complex/complex_manifest.csv");
        if (!rows.empty()) {
            int fileCol = -1, kindCol = -1, expCol = -1;
            for (size_t i = 0; i < rows[0].size(); ++i) {
                if (rows[0][i] == "file")     fileCol = static_cast<int>(i);
                if (rows[0][i] == "kind")     kindCol = static_cast<int>(i);
                if (rows[0][i] == "expected") expCol = static_cast<int>(i);
            }

            // 已知漏检:原因见 common/pcb_core/pcb_core.cpp 里对应规则的注释
            //
            // ⚠ 这张表是**当前真实行为**的登记,不是期望行为的清单。
            //   哪天判据重做完了,把条目删掉,断言会立刻转红,
            //   逼你补上"必须命中"那条 —— 否则就是改好了却没人知道。
            //
            // 📌 2026-10-20:signal_polarity.png 从这张表里**划掉了**。
            //   旧判据量的是"ROI 里所有白丝印的质心",而极性条只是其中
            //   4~7px 宽的一小条,旁边还有丝印外框、二极管符号线在抢 ——
            //   实测"根本没有极性条"的电容,质心也正好 50.0%,单边阈值
            //   根本没有余量。现在改成:腐蚀出极性条本身 -> 定侧 -> 和
            //   配方给的 polarity_side 比。这张图现在必须命中 polarity。
            struct KnownMiss { const char* file; const char* why; };
            const KnownMiss misses[] = {
                {"power_short.png",
                 "短路判据找的是蓝紫**圆环**,而 power 这张图画的是一条**金条** —— "
                 "面积 0.1992% 撞出上界 0.08%,宽 41.4% 对角线也撞出上界 8%。"
                 "不是阈值没调好,是缺陷定义与判据不匹配。"},
            };
            // 命中已知漏检时把**原因一起带出来**。
            //
            // ⚠ why 这个字段不能只摆着不用:
            //   一个从不被人读的"原因"就是**会骗人的注释** ——
            //   判据改好了它不会变,半年后没人知道该不该删这条。
            //   把它塞进断言消息,谁看到红就知道当时为什么漏、现在该不该修。
            //   返回空串 = 不是已知漏检。
            auto knownMissWhy = [&](const std::string& f) {
                for (const auto& m : misses) if (f == m.file) return std::string(m.why);
                return std::string();
            };

            for (size_t r = 1; r < rows.size(); ++r) {
                const auto& row = rows[r];
                if (static_cast<int>(row.size()) <= expCol) continue;
                const std::string file = row[fileCol];
                const std::string kind = row[kindCol];
                const std::string want = row[expCol];
                const bool wantFail = want.rfind("FAIL", 0) == 0;
                const std::string wantType =
                    wantFail ? want.substr(want.find(':') + 1) : std::string();

                BoardRecipe recipe;
                std::string reason;
                if (!loadRecipeFile((recipes / (kind + ".json")).string(),
                                    &recipe, &reason)) {
                    check(false, kind + " 配方加载失败:" + reason);
                    continue;
                }
                const cv::Mat img = cv::imread((cdir / file).string(), cv::IMREAD_COLOR);
                if (img.empty()) {
                    check(false, "读得到 " + file);
                    continue;
                }
                const JudgeParams p;
                const InspectResult ir =
                    inspect(img, recipe, pcbcolor::goldBand(p.maskVMul));
                const JudgeResult jr = judge(img, ir.sites, p, ir.goldMask, ir.boardBase);

                const std::string gotResult = jr.refused ? "REFUSED"
                                           : (jr.defects.empty() ? "PASS" : "FAIL");
                const std::string who = kind + "/" + file;

                const std::string missWhy = knownMissWhy(file);
                if (!missWhy.empty()) {
                    // 记的是**当前真实行为**,不是期望行为。
                    //
                    // 这条断言同时是一张**过期检测器**:
                    // 一旦判据修好了(这张图开始 FAIL),这条会立刻转红,
                    // 提醒你把它从 misses 里删掉,换成上面的"必须命中"那条。
                    // 不这么做的话,修好了也没人知道,漏检就变成了第二张好板。
                    check(gotResult == "PASS",
                          who + " 是【已知漏检】,当前表现为 PASS(实际 " +
                          gotResult + ")。当初漏的原因:" + missWhy +
                          " —— 如果判据修好了,这条会转红,把它从 misses 里删掉");
                    continue;
                }
                check(gotResult == (wantFail ? "FAIL" : "PASS"),
                      who + " 期望 " + (wantFail ? "FAIL" : "PASS") +
                      "(实际 " + gotResult + ")");
                if (wantFail && !wantType.empty()) {
                    bool hit = false;
                    std::string types;
                    for (const auto& d : jr.defects) {
                        types += d.type + " ";
                        if (d.type == wantType) hit = true;
                    }
                    check(hit, who + " 必须命中 " + wantType +
                               "(实际: " + types + ")");
                }
                // 定位到的元件数要与真值表一致
                check(static_cast<int>(ir.sites.size()) ==
                          std::stoi(row[2]),
                      who + " 定位到 " + row[2] + " 个元件(实际 " +
                      std::to_string(ir.sites.size()) + ")");
            }
        }
    }
}

int main() {
    g_root = findRepoRoot();

    std::cout << "=== common/pcb_core 回归测试 ===" << std::endl;
    std::cout << "仓库根: " << g_root.string() << std::endl;

    test_mini_json();
    test_recipe_validation();
    test_refuse_paths();
    test_end_to_end();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
