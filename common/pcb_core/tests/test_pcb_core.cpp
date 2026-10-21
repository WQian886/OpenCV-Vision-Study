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
// 工作目录不同(CWD = 各自的测试工作目录)。所以**不能写死相对路径** ——
// 写死的话,在其中一个项目里跑就会"找不到文件",而找不到文件时
// 断言会红(而不是空转),但红的原因很难看懂。
//
// ⚠⚠ 判据是 **common/ 存在**,不是"工程目录名存在"。
//
//   原来是"看见 W6_PCB_DefectProject 和 W7_VisionMasterLite 就算根"。
//   2026-10-20 上 CI 第一次跑就把它骗了:
//   CI 的构建目录是 build/ci/<工程名>/,于是 build/ci/ 下面
//   **正好有一份 W6_PCB_DefectProject 和 W7_VisionMasterLite**
//   (构建树的根,一个工程一个子目录)。
//   往上找的循环在 build/ci/ 就停了,把**构建目录当成了仓库根**。
//
//   后果不是崩,是"找不到图":
//     [FAIL] 读得到 E:\...\build\ci\W5_OCR_QRCode\images\qrcode_test.png
//   报的是"文件不存在",而真正的原因是"根找错了"——
//   查错方向完全不同。
//
// common/ 只有仓库根有(共享判定核心和共享 QR/OCR 核心都在那),
// 构建树里不可能有同名子目录。用它当判据,就不会被"长得像"骗到。
//
// ============================================================
static fs::path findRepoRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "common" / "pcb_core" / "pcb_core.h") &&
            fs::exists(p / "W6_PCB_DefectProject") &&
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
        check(known.size() == 7,
              "knownChecks 有 7 条规则(实际 " + std::to_string(known.size()) + ")");
        const char* names[] = {rule::kMissing, rule::kShifted, rule::kWrong,
                               rule::kPolarity, rule::kBridge, rule::kInsufficient,
                               rule::kShort};
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
// 短路判据的**前置条件**:元件本体不能是金属色
// ============================================================
//
// 短路判据量的是"两个端子之间的绝缘缝里有没有金属"。如果元件本体
// 自己就落在金属色带里(生成器的调色板按 RGB 写、注释按 BGR 写,
// 于是"电容黄"实际渲染成和铜箔几乎同色的青蓝),那缝里本来就全是
// 金属,分不出"是本体"还是"是短路"—— 硬判就是**每块好板都报短路**。
//
// 这条前置条件判据自己查不了(缺陷图的本体中心被那根短路铜条盖住,
// 两种情况长得一模一样),所以由**配方层**保证:
//   · 谁配 short -> tools/export_recipes.py 的 CHECKS 表
//   · 配了对不对 -> 就是下面这个断言
//
// 断言在**好板**上量(好板上本体中心就是本体,没有铜条盖着)。
// 配错了不会静默:好板会整片报短路,下面那张真值表当场红。
static void checkShortPrecondition(const cv::Mat& img, const InspectResult& ir,
                                   const std::string& who) {
    for (const auto& s : ir.sites) {
        bool hasShort = false;
        for (const auto& c : s.checks) {
            if (c == "short") { hasShort = true; break; }
        }
        if (!hasShort) continue;
        const int cx = s.roi.x + s.roi.width / 2;
        const int cy = s.roi.y + s.roi.height / 2;
        int gold = 0;
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                if (x < 0 || y < 0 || x >= img.cols || y >= img.rows) continue;
                if (ir.goldMask.at<uchar>(y, x) != 0) ++gold;
            }
        }
        check(gold == 0,
              who + " 的 " + s.label + " 配了短路检查,它的本体中心**不是**金属色"
              "(金层掩膜里 " + std::to_string(gold) + "/25 个像素是金属)—— "
              "这是短路判据能成立的前提:本体要是金属色,两脚之间的缝里"
              "有没有金属就分不出来了");
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
            int fileCol = -1, defectCol = -1, expCol = -1, siteCol = -1;
            for (size_t i = 0; i < rows[0].size(); ++i) {
                if (rows[0][i] == "file")   fileCol = static_cast<int>(i);
                if (rows[0][i] == "defect") defectCol = static_cast<int>(i);
                if (rows[0][i] == "expected") expCol = static_cast<int>(i);
                if (rows[0][i] == "site")   siteCol = static_cast<int>(i);
            }
            check(fileCol >= 0 && defectCol >= 0 && expCol >= 0,
                  "manifest.csv 有 file/defect/expected 三列");
            check(siteCol >= 0,
                  "manifest.csv 有 site 列(缺陷画在哪个元件上——"
                  "期望值从这里读,不在代码里写死)");

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
                // 缺陷画在哪个元件上 —— 从**真值表**读。
                // 写死 "R2" 就是第二份真值表:哪天生成器把缺陷挪到别的
                // 元件上,这条断言会一直绿着,直到有一天它自己红掉 ——
                // 而那时候没人知道它为什么红。
                const std::string wantSite =
                    (siteCol >= 0 && static_cast<int>(row.size()) > siteCol)
                        ? row[siteCol] : std::string();

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

                // --- 短路:点名到元件 + 读数在阈值两侧 ---
                //
                // ⚠ 2026-10-20 短路判据重做后加的。
                //   "整张板 FAIL"不等于"那个元件被找到了"——
                //   别的站点报一条同类型的缺陷就能把 verdict 撑成 FAIL。
                //   所以短路必须**点名到元件**,而且读数要能看出
                //   "好板是 0%、废板是接近 100%"。
                {
                    const std::string who = file;
                    std::string comp;
                    double read = -1.0;      // 出缺陷那个元件的读数
                    double worst = -1.0;     // 所有配了短路的元件里最大的读数
                    for (const auto& d : jr.defects) {
                        if (d.type == "short") comp = d.component;
                    }
                    for (const auto& m : jr.metrics) {
                        if (m.rule != "short") continue;
                        if (m.component == comp) read = m.value;
                        if (m.value > worst) worst = m.value;
                    }
                    if (wantDefect == "short") {
                        check(comp == wantSite,
                              who + " 的短路必须点名到 " + wantSite +
                              "(实际 \"" + comp + "\")");
                        check(read >= 50.0,
                              who + " 的端子间隙金属占比应超过阈值 50%(实际 " +
                              std::to_string(read) + ")");
                    } else if (wantResult == "PASS") {
                        check(comp.empty(),
                              who + " 是好板,不该报短路(实际点名到 \"" + comp + "\")");
                        // 好板:所有配了短路的元件,读数都必须是 **0**
                        //
                        // ⚠ 这里钉的是"**一点金属都没有**",不是"小于阈值"。
                        //   阈值是 50%,而好板真实的读数是 0% ——
                        //   中间那一大截余量看着安全,其实会盖住一类 bug:
                        //   采样窗口多掐进焊盘 1px(焊盘自带 1px 描边,也是金属),
                        //   好板读数就变成 4.8%:仍然低于阈值,判定不变,
                        //   但"缝里到底有没有金属"这个读数已经**不诚实**了。
                        //   读数一旦不诚实,标定它的那些结论就都不可信了。
                        check(worst == 0.0,
                              who + " 是好板,端子间隙里一点金属都没有(最大 " +
                              std::to_string(worst) + "%)");
                        // 好板才量得了前置条件(缺陷图的本体中心被铜条盖着)
                        checkShortPrecondition(img, ir, who);
                    }
                }
                // --- insufficient:读数必须诚实,缺陷必须点名 ---
                //
                // ⚠ 2026-10-21 加的。insufficient 判据重做成"逐个登记过的
                //   端子量金属面积"之后,只断言"缺陷类型命中"是不够的:
                //   **读数本身也可以是假的**。
                //   · 不是焊少废板的图:同一个元件的焊盘一样大,
                //     最小焊盘比同侪中位数低 0% -> 读数必须**恰好 0**。
                //     钉"恰好 0"而不是"小于阈值":阈值是 33%,中间那一大截
                //     余量看着安全,其实会盖住一类 bug —— 采样框多掐进 1px
                //     金属,读数变成 4%,判定不变,但"这个焊盘到底有多少铜"
                //     这个数已经不诚实了。读数不诚实,拿它标出来的阈值就不可信。
                //   · 焊少废板:读数必须超过阈值(阈值从 metric 自己读,
                //     **不抄一份**),而且缺陷必须点名到真值表说的那个元件。
                {
                    std::string comp;
                    double read = -1.0, thr = -1.0;
                    for (const auto& d : jr.defects)
                        if (d.type == "insufficient") comp = d.component;
                    for (const auto& m : jr.metrics) {
                        if (m.rule != "insufficient") continue;
                        if (m.component == comp) { read = m.value; thr = m.threshold; }
                    }
                    if (wantDefect == "insufficient") {
                        check(comp == wantSite,
                              file + " 的焊少必须点名到 " + wantSite +
                              "(实际 \"" + comp + "\")");
                        check(read > thr,
                              file + " 的焊少读数必须超过阈值(实际 " +
                              std::to_string(read) + " vs 阈值 " +
                              std::to_string(thr) + ")");
                    } else {
                        double worst = -1.0;
                        for (const auto& m : jr.metrics)
                            if (m.rule == "insufficient" && m.value > worst)
                                worst = m.value;
                        check(worst == 0.0,
                              file + " 不是焊少废板,焊少读数必须恰好 0%"
                              "(实际最大 " + std::to_string(worst) + "%)");
                    }
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
            int fileCol = -1, kindCol = -1, expCol = -1, siteCol = -1;
            for (size_t i = 0; i < rows[0].size(); ++i) {
                if (rows[0][i] == "file")     fileCol = static_cast<int>(i);
                if (rows[0][i] == "kind")     kindCol = static_cast<int>(i);
                if (rows[0][i] == "expected") expCol = static_cast<int>(i);
                if (rows[0][i] == "site")     siteCol = static_cast<int>(i);
            }
            check(siteCol >= 0,
                  "complex_manifest.csv 有 site 列(缺陷画在哪个元件上——"
                  "期望值从这里读,不在代码里写死)");

            // 已知漏检:原因见 common/pcb_core/pcb_core.cpp 里对应规则的注释
            //
            // ⚠ 这张表是**当前真实行为**的登记,不是期望行为的清单。
            //   哪天判据重做完了,把条目删掉,断言会立刻转红,
            //   逼你补上"必须命中"那条 —— 否则就是改好了却没人知道。
            //
            // 📌 2026-10-20:**这张表现在是空的**。
            //   里面最后一条是 power_short.png —— 短路判据原来找的是
            //   生成器画给人眼看的蓝紫**标记圈**,而 power 那张图画的是
            //   一条**金条**,于是整板一路 PASS(漏检)。
            //   短路判据重做成"量两个端子之间的绝缘缝里有没有金属"之后,
            //   这张图能抓到了,条目删掉(改好了却没人知道 = 漏检变成第二张好板)。
            //
            //   另一张 signal_polarity.png 早在极性判据重做时就划掉了。
            struct KnownMiss { const char* file; const char* why; };
            const std::vector<KnownMiss> misses = {};
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
                // 缺陷画在哪个元件上 —— 从**真值表**读,不在代码里写死 "L1"。
                const std::string wantSite =
                    (siteCol >= 0 && static_cast<int>(row.size()) > siteCol)
                        ? row[siteCol] : std::string();

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
                // --- 短路:点名到元件 ---
                //
                // ⚠ 短路必须点名到元件:"整张板 FAIL"不等于"那个元件被找到了"
                //   —— 别的站点报一条同类型的缺陷就能把 verdict 撑成 FAIL。
                //   短路是**按元件**的规则(它量的是某个元件的两脚之间),
                //   所以它报出来的也必须是某个元件。
                if (wantType == "short") {
                    std::string comp;
                    for (const auto& d : jr.defects) {
                        if (d.type == "short") comp = d.component;
                    }
                    // ⚠ 读数要**按元件取**:一块板上可能有好几个元件配了短路
                    //   (power 板有 7 个),读错那条会拿到别的元件的 0%。
                    double read = -1.0;
                    for (const auto& m : jr.metrics) {
                        if (m.rule == "short" && m.component == comp) read = m.value;
                    }
                    check(comp == wantSite,
                          who + " 的短路必须点名到 " + wantSite +
                          "(实际 \"" + comp + "\")");
                    check(read >= 50.0,
                          who + " 的端子间隙金属占比应超过阈值 50%(实际 " +
                          std::to_string(read) + ")");
                }
                if (!wantFail) {
                    // 好板才量得了短路的前置条件(缺陷图的本体中心被铜条盖着)
                    checkShortPrecondition(img, ir, who);
                    // 好板:配了短路的元件,端子间隙里一点金属都没有。
                    //
                    // ⚠ 这里也钉"**恰好 0**",不是"小于阈值"——
                    //   power 板有 7 个元件配了短路,读数只要有一点点不对
                    //   (比如采样窗口多掐进焊盘 1px),这个数就再也不诚实了。
                    //   读数不诚实,拿它标出来的阈值就不可信。
                    double worst = -1.0;
                    for (const auto& m : jr.metrics) {
                        if (m.rule != "short") continue;
                        if (m.value > worst) worst = m.value;
                    }
                    check(worst == 0.0,
                          who + " 是好板,端子间隙里一点金属都没有(最大 " +
                          std::to_string(worst) + "%)");
                }
                // --- insufficient:读数必须诚实,缺陷必须点名 ---
                //
                // 同 demo 那边:不是焊少废板的图,读数必须**恰好 0**;
                // 是焊少废板,读数必须超过阈值(阈值从 metric 自己读),
                // 而且缺陷必须点名到真值表说的那个元件。
                //
                // ⚠ 2026-10-21:三块 complex 板的焊少真值图是这一轮新加的
                //   (power/mcu/signal_insufficient.png)。在它们出现之前,
                //   insufficient 这条规则在 complex 板上**一次都没跑过**
                //   (全在"没查"里),所以既没有废板样本,也没有读数可钉。
                {
                    std::string comp;
                    double read = -1.0, thr = -1.0;
                    for (const auto& d : jr.defects)
                        if (d.type == "insufficient") comp = d.component;
                    for (const auto& m : jr.metrics) {
                        if (m.rule != "insufficient") continue;
                        if (m.component == comp) { read = m.value; thr = m.threshold; }
                    }
                    if (wantType == "insufficient") {
                        check(comp == wantSite,
                              who + " 的焊少必须点名到 " + wantSite +
                              "(实际 \"" + comp + "\")");
                        check(read > thr,
                              who + " 的焊少读数必须超过阈值(实际 " +
                              std::to_string(read) + " vs 阈值 " +
                              std::to_string(thr) + ")");
                    } else {
                        double worst = -1.0;
                        for (const auto& m : jr.metrics)
                            if (m.rule == "insufficient" && m.value > worst)
                                worst = m.value;
                        check(worst == 0.0,
                              who + " 不是焊少废板,焊少读数必须恰好 0%"
                              "(实际最大 " + std::to_string(worst) + "%)");
                    }
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

// ============================================================
// 用例 5:短路必须有端子位置 —— 没有就必须报"没查"
//
// ============================================================
//
// ⚠ 这一条是 2026-10-20 补的,补的原因是**变异验证发现的**:
//   判据里有"没登记端子 -> 报没查(skipped)"这条路径,
//   但**没有任何断言**钉它 —— 把那条 push_back 删掉,
//   整套测试照样全绿。
//
//   而这条路径恰恰是"静默失效"最典型的形态:
//     端子没登记 -> 短路判不了 -> 这条规则没跑 -> defects 里没有 short
//     -> 板子判成合格 —— **看起来和"查过了没问题"一模一样**。
//   真短路的废板就这么被放过去了,而且界面上看不出任何异常。
//
// 所以这里钉三件事:
//   ① 没登记端子 -> 必须有 skipped 条目(rule=short),说清为什么没查;
//   ② 没查的时候不许同时报出 short 缺陷(没查就不许给结论);
//   ③ 登记够了端子 -> 规则必须真的跑了(不许有 skipped,也必须有读数)。
//
// 顺带把"登记了三个端子"也钉住:数量不对同样判不了,
// 不能因为"有 pads 字段"就当成登记好了。
// ============================================================
static void test_short_needs_pads() {
    beginCase("短路:没登记端子就必须报\"没查\",不许装作查过了");

    const fs::path w6 = g_root / "W6_PCB_DefectProject";
    const cv::Mat img =
        cv::imread((w6 / "images" / "baseline.png").string(), cv::IMREAD_COLOR);
    check(!img.empty(), "读得到 baseline.png");
    if (img.empty()) return;

    // 同一块板、同一个元件,只改"登记了几个端子"。
    // R2 在 demo 板上的端子是 [[110,480,12],[190,480,12]](配方里的真值)。
    auto run = [&](const std::string& padsJson) {
        JudgeResult jr;
        BoardRecipe r;
        std::string reason;
        const std::string json =
            "{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":[{"
            "\"label\":\"R2\",\"kind\":\"R\",\"cx\":150,\"cy\":480,"
            "\"body_w\":100,\"body_h\":28,\"checks\":[\"short\"]" +
            padsJson + "}]}";
        const bool ok = parseRecipeJson(json, &r, &reason);
        check(ok, std::string("配方能加载") + (ok ? "" : ("(" + reason + ")")));
        if (!ok) return jr;
        const JudgeParams p;
        const InspectResult ir = inspect(img, r, pcbcolor::goldBand(p.maskVMul));
        return judge(img, ir.sites, p, ir.goldMask, ir.boardBase);
    };
    auto countSkipped = [](const JudgeResult& jr, const char* rule) {
        int n = 0;
        for (const auto& s : jr.skipped) if (s.rule == rule) ++n;
        return n;
    };
    auto countDefects = [](const JudgeResult& jr, const char* type) {
        int n = 0;
        for (const auto& d : jr.defects) if (d.type == type) ++n;
        return n;
    };

    // ① 没登记端子 -> 必须报"没查"
    {
        const JudgeResult jr = run("");
        const int n = countSkipped(jr, "short");
        check(n == 1,
              "没登记端子 -> 必须报一条 \"没查\"(实际 " + std::to_string(n) + " 条)");
        std::string why;
        for (const auto& s : jr.skipped) if (s.rule == "short") why = s.reason;
        check(!why.empty(),
              "\"没查\" 必须说清为什么(实际: \"" + why + "\")");
        // ② 没查就不许给结论
        const int d = countDefects(jr, "short");
        check(d == 0,
              "没查的时候不许报出 short 缺陷(实际报了 " + std::to_string(d) + " 条)");
    }
    // ③ 登记了三个端子 -> 数量不对,同样判不了
    {
        const JudgeResult jr =
            run(",\"pads\":[[110,480,12],[190,480,12],[150,480,12]]");
        const int n = countSkipped(jr, "short");
        check(n == 1,
              "登记了三个端子 -> 同样必须报 \"没查\"(实际 " + std::to_string(n) + " 条)");
    }
    // ④ 登记了两个 -> 规则必须真的跑了
    {
        const JudgeResult jr = run(",\"pads\":[[110,480,12],[190,480,12]]");
        check(countSkipped(jr, "short") == 0,
              "登记了两个端子 -> 规则必须真的跑(不该报没查)");
        bool hasMetric = false;
        for (const auto& m : jr.metrics) if (m.rule == "short") hasMetric = true;
        check(hasMetric, "登记了两个端子 -> 必须给出读数(量了就得报数)");
    }
}

// ============================================================
// 用例 6:insufficient 必须有端子 —— 没有就必须报"没查"
//
// ============================================================
//
// ⚠ 2026-10-21 补的,补的原因和用例 5(短路)是**同一个坑**:
//   判据里有"没登记端子 -> 报没查(skipped)"这条路径,
//   但**没有任何断言**钉它 —— 把那条 push_back 删掉,整套测试照样全绿。
//
//   而这条路径恰恰是本轮的主题("把『没查』变成『查过了』"):
//     端子没登记 -> 焊少判不了 -> 这条规则没跑 -> defects 里没有 insufficient
//     -> 板子判成合格 —— **看起来和"查过了没问题"一模一样**。
//   真的焊少的废板就这么被放过去了,而且界面上看不出任何异常。
//
// 所以这里钉四件事:
//   ① 没登记端子 -> 必须有 skipped 条目(rule=insufficient),说清为什么没查;
//   ② 没查的时候不许同时报出 insufficient 缺陷(没查就不许给结论);
//   ③ 只登记一个端子 -> 同样报"没查"(没有同侪可比);
//   ④ 登记了两个 -> 规则必须真的跑了(不许有 skipped),而且必须给出读数。
//
// 顺带钉住读数:两个一样大的焊盘,读数必须**恰好 0%**。
// ============================================================
static void test_insufficient_needs_pads() {
    beginCase("焊少:没登记端子就必须报\"没查\",不许装作查过了");

    const fs::path w6 = g_root / "W6_PCB_DefectProject";
    const cv::Mat img =
        cv::imread((w6 / "images" / "baseline.png").string(), cv::IMREAD_COLOR);
    check(!img.empty(), "读得到 baseline.png");
    if (img.empty()) return;

    // 同一块板、同一个元件,只改"登记了几个端子"。
    // R2 在 demo 板上的端子是 [[110,480,12],[190,480,12]](配方里的真值)。
    auto run = [&](const std::string& padsJson) {
        JudgeResult jr;
        BoardRecipe r;
        std::string reason;
        const std::string json =
            "{\"name\":\"t\",\"ref_w\":1000,\"ref_h\":700,\"sites\":[{"
            "\"label\":\"R2\",\"kind\":\"R\",\"cx\":150,\"cy\":480,"
            "\"body_w\":100,\"body_h\":28,\"checks\":[\"insufficient\"]" +
            padsJson + "}]}";
        const bool ok = parseRecipeJson(json, &r, &reason);
        check(ok, std::string("配方能加载") + (ok ? "" : ("(" + reason + ")")));
        if (!ok) return jr;
        const JudgeParams p;
        const InspectResult ir = inspect(img, r, pcbcolor::goldBand(p.maskVMul));
        return judge(img, ir.sites, p, ir.goldMask, ir.boardBase);
    };
    auto countSkipped = [](const JudgeResult& jr, const char* rule) {
        int n = 0;
        for (const auto& s : jr.skipped) if (s.rule == rule) ++n;
        return n;
    };
    auto countDefects = [](const JudgeResult& jr, const char* type) {
        int n = 0;
        for (const auto& d : jr.defects) if (d.type == type) ++n;
        return n;
    };
    auto skipReason = [](const JudgeResult& jr, const char* rule) {
        for (const auto& s : jr.skipped)
            if (s.rule == rule) return s.reason;
        return std::string();
    };

    // ① 没登记端子 -> 必须报"没查"
    {
        const JudgeResult jr = run("");
        const int n = countSkipped(jr, "insufficient");
        check(n == 1,
              "没登记端子 -> 必须报一条 \"没查\"(实际 " + std::to_string(n) + " 条)");
        const std::string why = skipReason(jr, "insufficient");
        check(!why.empty(),
              "\"没查\" 必须说清为什么(实际: \"" + why + "\")");
        // 原因里必须点名"缺的是端子",不是一句"没查"
        check(why.find("没有登记端子") != std::string::npos,
              "\"没查\" 的原因必须写明缺的是端子(实际: \"" + why + "\")");
        // ② 没查就不许给结论
        const int d = countDefects(jr, "insufficient");
        check(d == 0,
              "没查的时候不许报出 insufficient 缺陷(实际报了 " +
              std::to_string(d) + " 条)");
    }
    // ③ 只登记一个端子 -> 没有同侪可比,同样报"没查"
    {
        const JudgeResult jr = run(",\"pads\":[[110,480,12]]");
        const int n = countSkipped(jr, "insufficient");
        check(n == 1,
              "只登记一个端子 -> 同样必须报 \"没查\"(实际 " +
              std::to_string(n) + " 条)");
        const std::string why = skipReason(jr, "insufficient");
        check(why.find("1 个") != std::string::npos,
              "\"没查\" 的原因必须写明登记了几个(实际: \"" + why + "\")");
    }
    // ④ 登记了两个 -> 规则必须真的跑了
    {
        const JudgeResult jr = run(",\"pads\":[[110,480,12],[190,480,12]]");
        check(countSkipped(jr, "insufficient") == 0,
              "登记了两个端子 -> 规则必须真的跑(不该报没查)");
        bool hasMetric = false;
        double read = -1.0;
        for (const auto& m : jr.metrics)
            if (m.rule == "insufficient") { hasMetric = true; read = m.value; }
        check(hasMetric, "登记了两个端子 -> 必须给出读数(量了就得报数)");
        // 两个一样大的焊盘 -> 读数必须恰好 0%
        check(read == 0.0,
              "两个一样大的焊盘 -> 焊少读数必须恰好 0%(实际 " +
              std::to_string(read) + "%)");
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
    test_short_needs_pads();
    test_insufficient_needs_pads();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
