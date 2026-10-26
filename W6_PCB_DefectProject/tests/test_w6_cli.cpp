// ============================================================
// W6 命令行层的回归测试
//
// ============================================================
//
// ## 为什么这份测试必须存在
//
// 2026-10-20,W6 的判定逻辑搬到了 common/pcb_core/,W6 自己也拆出了
// w6_cli.h/.cpp 让逻辑能被测。搬的过程中**新写/新挪了三块东西**,
// 它们当时一行测试都没有:
//
//   1. w6_cli.cpp 里的板型查询(runReport 靠它决定用哪份配方)
//   2. CSV 行的摊法(一个元件一行、多个缺陷怎么写)
//   3. writeCSVReport —— 上一轮刚修的 board_file 列 bug 就出在这里
//
// 特别要命的是第 1 条:**查不到板型**的后果不是"报错退出",
// 而是"拿不到配方 -> 判定不出东西 -> 报告里少一块板"。
// 而报告里少一块板,在界面上和"这块板合格"**长得一模一样**。
// 这就是本项目反复强调的"静默失效优先于崩溃排查"——
// 所以"必须跳过并且必须报出来"本身就得是一条断言。
//
// ## 和 core 测试的分工
//
//   common/pcb_core/tests : 判定**对不对**(真值表驱动)
//   这份                    : W6 **怎么用**判定(查板型/摊行/写 CSV)
//
// 两边都绿才说明"判定是对的,而且 W6 真的用上了它"。
//
// ## 期望值从哪来
//
// **不抄一份**。板子清单读 images/manifest.csv 和
// images/complex/complex_manifest.csv(本来就是真值表)。
// 加一张新图、改一条期望,测试自动跟着变。
//
// ============================================================

#include "w6_cli.h"

#include "utf8_path.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
// 极简断言(与仓库里其它套件同一套风格)
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
// 路径与文件的 UTF-8 辅助
//
// ⚠ 以前这里有个 utf8path::fromUtf8()(fs::u8path 的包装),
//   现在**删掉**了 —— 它是"UTF-8 窄串 -> 路径"的**第二份定义**,
//   而全仓库只有一处该有这个定义:utf8path::fromUtf8()。
//   两份定义迟早漂开(而且漂开的时候没人会想到去比)。
//   (跟本项目栽过五遍的"抄副本"是同一族。)
//
//   顺带:fs::u8path 在 C++20 已废弃
//   (u8string 变成 char8_t 之后它没法再返回 std::string)。
// ============================================================

// 复制文件(按宽路径,中文名也能写)
static bool copyFileTo(const fs::path& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

// 安全地写一张 PNG(路径含非 ASCII 字符时也能写)。
//
// ⚠ cv::imwrite 内部按 ANSI 打开文件,中文路径写不出来 ——
//   而且它不报错,只返回 false,看着像"编码失败"。
//   这里先 imencode 成内存字节,再按宽路径写出去。
static bool writePng(const fs::path& p, const cv::Mat& img) {
    std::vector<uchar> buf;
    if (!cv::imencode(".png", img, buf)) return false;
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return f.good();
}

// 找仓库根
//
// ctest 的工作目录是构建目录,写死相对路径会找不到文件。
//
// ⚠ 判据是 **common/ 存在**,不是"工程目录名存在"。
//   2026-10-20 上 CI 第一次跑就撞上了:CI 的构建目录是
//   build/ci/<工程名>/,build/ci/ 下面正好摆着一份
//   W6_PCB_DefectProject 和 W7_VisionMasterLite —— 循环在那一层就停了,
//   把**构建目录当成了仓库根**,于是报"找不到图"。
//   common/ 只有仓库根有,构建树里不可能有同名子目录。
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
static fs::path g_w6;
static fs::path g_recipes;

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

// 找出表头里某列的下标,找不到返回 -1
static int colOf(const std::vector<std::string>& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return static_cast<int>(i);
    }
    return -1;
}

// 递归数目录下的 .png 个数
static int countPng(const fs::path& dir) {
    int n = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".png") ++n;
    }
    return n;
}

// 递归列目录下的 .png 相对路径
static std::set<std::string> listPngRel(const fs::path& dir) {
    std::set<std::string> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".png") {
            out.insert(utf8path::text(fs::relative(e.path(), dir)));
        }
    }
    return out;
}

// ============================================================
// 用例 1:板型查询
//
// 这一条是整条链的入口:**查不到板型 -> 拿不到配方 ->
// 定位不出元件 -> 判定不出东西 -> 报告里少一块板**。
// 而且少掉的那块板在报告里和"合格"长得一模一样。
//
// 所以重点不是"能查到",而是"查不到时**不许瞎猜**"。
// ============================================================
static void test_board_kind() {
    beginCase("板型查询:查得到要准,查不到不许瞎猜");

    // --- demo 板:manifest.csv 里没有 kind 列 -> 约定是 demo ---
    {
        const std::string kind = w6::boardKindOf(g_w6 / "images" / "baseline.png");
        check(kind == "demo",
              "demo 板查到 \"demo\"(实际 \"" + kind + "\")");
    }

    // --- complex 板:kind 列写着什么就是什么 ---
    {
        const auto rows = readCsv(g_w6 / "images" / "complex" / "complex_manifest.csv");
        check(!rows.empty(), "读得到 complex_manifest.csv");
        if (!rows.empty()) {
            const int fileCol = colOf(rows[0], "file");
            const int kindCol = colOf(rows[0], "kind");
            check(fileCol >= 0 && kindCol >= 0, "complex 表有 file/kind 两列");
            for (size_t r = 1; r < rows.size(); ++r) {
                if (static_cast<int>(rows[r].size()) <= kindCol) continue;
                const std::string file = rows[r][fileCol];
                const std::string want = rows[r][kindCol];
                const std::string got =
                    w6::boardKindOf(g_w6 / "images" / "complex" / file);
                check(got == want,
                      file + " 板型应为 " + want + "(实际 \"" + got + "\")");
            }
        }
    }

    // --- 查不到:目录里没有 manifest ---
    {
        const std::string kind =
            w6::boardKindOf(g_w6 / "images" / utf8path::fromUtf8("根本没有这张图.png"));
        check(kind.empty(),
              "图不在任何 manifest 里 -> 返回空串而不是默认板型(实际 \"" + kind + "\")");
    }

    // --- 查不到:目录本身不存在 ---
    {
        const std::string kind = w6::boardKindOf(g_w6 / utf8path::fromUtf8("没有这个目录") / "x.png");
        check(kind.empty(), "目录不存在 -> 返回空串(不是崩)");
    }

    // --- 查不到:同名但不是 png ---
    {
        // manifest.csv 自己在 images/ 里,它不是 png,但 boardKindOf
        // 对任何文件都返回"板型" —— 拿 manifest.csv 去问,
        // 应该查不到(demo 表里没有 manifest.csv 这一行)。
        const std::string kind = w6::boardKindOf(g_w6 / "images" / "manifest.csv");
        check(kind.empty(),
              "问 manifest.csv 自己 -> 查不到(实际 \"" + kind + "\")");
    }
}

// ============================================================
// 用例 2:配方目录
// ============================================================
static void test_recipe_dir() {
    beginCase("配方目录:能找到,且不会因为一个坏目录就整个废掉");

    // --- 显式指定 ---
    {
        const fs::path d = w6::findRecipeDir(g_recipes);
        check(d == g_recipes,
              "显式指定时原样返回(实际 \"" + utf8path::text(d) + "\")");
    }

    // --- 显式指定了一个不存在的目录 ---
    //
    // ⚠ 关键:这里必须返回空串(让人看见"配方目录不对"),
    //   而不是原样返回一个坏路径 —— 后者会让每块板各报一次
    //   "配方加载失败",刷屏,而且掩盖了真正的原因。
    {
        const fs::path d = w6::findRecipeDir(g_w6 / utf8path::fromUtf8("根本没有这个目录"));
        check(d.empty(),
              "指定的目录里没有配方 -> 返回空串(实际 \"" + utf8path::text(d) + "\")");
    }

    // --- 自动找 ---
    {
        const fs::path d = w6::findRecipeDir(fs::path());
        check(!d.empty(), "自动找得到配方目录");
        if (!d.empty()) {
            check(fs::exists(d / "demo.json"),
                  "自动找到的目录里确实有 demo.json(实际 " + utf8path::text(d) + ")");
        }
    }
}

// ============================================================
// 用例 2.5:路径的编码往返
//
// ⚠ 这一组是被**真 CI runner** 逼出来的。
//
//   2026-10-26 第一次在 GitHub runner(windows-2022,ACP=1252)上真跑,
//   W6 的 w6_cli 套件挂在"配方目录"这一条:
//     "No mapping for the Unicode character exists in the target multi-byte code page"
//   本机(ACP=936 GBK)一路绿 —— 差别不在代码,在**这台机器的 ACP**。
//
//   ⚠ 而且原来那条测试只钉了**负例**(指定一个坏目录 -> 返回空)。
//     "坏目录"怎么编码都对 —— 于是**编码搞错了这条测试照样过**。
//     这就是"验证不等于验证":测试看着在验,其实没验到点上。
//     负例通过 + 正例缺失 = 这条断言当时什么都没保证。
//
//   所以这里补三条**正例**:
//     (a) text() 出来的是**真 UTF-8 字节**
//         (按 ACP 窄化是 16 字节 GBK;ACP=1252 直接抛异常)
//     (b) fromUtf8(text(p)) == p —— 往返不丢
//     (c) 一个**真的存在**的中文名目录,findRecipeDir 找得到
//         (只有正例才能证明"路径没在传递过程中被改写")
// ============================================================
static void test_path_encoding_roundtrip() {
    beginCase("路径编码:文本是 UTF-8,往返不丢,中文目录找得到");

    const fs::path zh = utf8path::fromUtf8("根本没有这个目录");

    // --- (a) text() 是 UTF-8 ---
    {
        const std::string t = utf8path::text(zh);
        // "根本没有这个目录" = 8 个汉字,UTF-8 各 3 字节 = 24 字节。
        // 按 ACP 窄化(GBK)只有 16 字节;ACP=1252 压根转不了(抛异常)。
        check(t.size() == 24,
              "text() 出来是 UTF-8 字节(期望 24,实际 " +
              std::to_string(t.size()) + ")");
        check(t.find('?') == std::string::npos,
              "text() 里没有被替换成 '?' 的字符(那是 ACP 转不动时的默认行为)");
    }

    // --- (b) 往返不丢 ---
    {
        const fs::path back = utf8path::fromUtf8(utf8path::text(zh));
        check(back == zh,
              "路径 -> UTF-8 文本 -> 路径,还是同一条路径");
    }

    // --- (c) 中文名目录找得到 ---
    {
        const fs::path dir =
            fs::current_path() / "w6_cli_test_tmp" / utf8path::fromUtf8("中文配方目录");
        fs::create_directories(dir);
        {
            // findRecipeDir 要求目录里确实有 .json 才认它
            std::ofstream j(dir / "demo.json");
            j << "{}";
        }
        const fs::path found = w6::findRecipeDir(dir);
        check(found == dir,
              "中文名目录被原样找到(实际 \"" + utf8path::text(found) + "\")");
        check(fs::exists(found / "demo.json"),
              "找到的就是那一个,不是碰巧同名的别的目录");
    }
}

// ============================================================
// 用例 3:跑一块板
//
// 真值表驱动:每张图的期望结论都从 manifest 读,不写死。
// ============================================================
static void test_run_board() {
    beginCase("跑一块板:结论要对,失败时要说人话");

    // --- demo 板:逐张对照 manifest.csv ---
    {
        const auto rows = readCsv(g_w6 / "images" / "manifest.csv");
        check(!rows.empty(), "读得到 images/manifest.csv");
        if (!rows.empty()) {
            const int fileCol   = colOf(rows[0], "file");
            const int defectCol = colOf(rows[0], "defect");
            const int expCol    = colOf(rows[0], "expected");
            check(fileCol >= 0 && defectCol >= 0 && expCol >= 0,
                  "manifest 有 file/defect/expected 三列");

            for (size_t r = 1; r < rows.size(); ++r) {
                const auto& row = rows[r];
                if (static_cast<int>(row.size()) <= expCol) continue;
                const std::string file = row[fileCol];
                const std::string want = row[expCol];
                const std::string wantDefect = row[defectCol];

                w6::BoardResult br;
                std::string err;
                const bool ok = w6::runBoard(g_w6 / "images" / file,
                                            g_recipes, br, err);
                check(ok, file + " 能判(失败原因:" + err + ")");
                if (!ok) continue;
                check(br.pass == (want == "PASS"),
                      file + " 期望 " + want + "(实际 " +
                      (br.pass ? "PASS" : "FAIL") + ")");
                if (want == "FAIL" && wantDefect != "none") {
                    bool hit = false;
                    std::string types;
                    for (const auto& d : br.defects) {
                        types += d.type + " ";
                        if (d.type == wantDefect) hit = true;
                    }
                    check(hit, file + " 必须命中 " + wantDefect +
                               "(实际: " + types + ")");
                }
                check(br.componentCount > 0, file + " 定位到了元件");
            }
        }
    }

    // --- 失败路径 1:图不在 manifest 里 ---
    //
    // ⚠ 这一条是整个测试里最重要的:
    //   查不到板型 -> 必须**失败并说明为什么**,
    //   绝不能"当成某块板"或者"安静地什么都不做"。
    {
        const fs::path lonely = fs::current_path() / "w6_cli_test_tmp" / "lonely";
        fs::create_directories(lonely);
        cv::Mat blank(120, 160, CV_8UC3, cv::Scalar(40, 90, 40));
        check(writePng(lonely / "unregistered.png", blank), "写测试 PNG 成功");

        w6::BoardResult br;
        std::string err;
        const bool ok = w6::runBoard(lonely / "unregistered.png",
                                     g_recipes, br, err);
        check(!ok, "没登记的图必须判不了(不能当成某块板蒙混过关)");
        check(!err.empty(), "判不了时必须给原因(不能一声不吭)");
        check(err.find("板型") != std::string::npos,
              "原因里要点明是板型的问题(实际 \"" + err + "\")");
        check(!br.pass, "判不了时不能是 PASS");
    }

    // --- 失败路径 2:图根本不存在 ---
    {
        w6::BoardResult br;
        std::string err;
        const bool ok = w6::runBoard(g_w6 / "images" / utf8path::fromUtf8("根本没有这张图.png"),
                                     g_recipes, br, err);
        check(!ok, "图不存在必须判不了");
        check(!err.empty(), "图不存在时必须给原因");
    }

    // --- 失败路径 3:配方目录不对 ---
    {
        w6::BoardResult br;
        std::string err;
        const bool ok = w6::runBoard(g_w6 / "images" / "baseline.png",
                                     (g_w6 / utf8path::fromUtf8("根本没有这个目录")),
                                     br, err);
        check(!ok, "配方目录不对时必须判不了");
        check(!err.empty(), "配方目录不对时必须给原因");
    }
}

// ============================================================
// 用例 3.5:非 ASCII 文件名
//
// ⚠ 这一组是被**真实 bug** 逼出来的,不是预防性测试:
//
//   原来文件名是按 ANSI 代码页取的(path::string()),
//   而 manifest.csv 是 UTF-8 —— 两边编码不同,
//   中文名**永远比不出来** -> "查不到板型" -> 这块板被**静默跳过**。
//   在报告里,被跳过的板和合格的板长得一模一样。
//
//   同一个坑还有第二个:窄串构造路径(path / "中文")
//   在 MSVC 上按 ANSI 解释,UTF-8 源文件直接抛异常 —— 崩。
//
// 所以这两条都要钉:
//   (a) 查得到板型
//   (b) 批量报告里这块板**被判**,而不是被跳过
// ============================================================
static void test_non_ascii_filename() {
    beginCase("非 ASCII 文件名:查得到板型,不会被静默跳过");

    const fs::path dir = fs::current_path() / "w6_cli_test_tmp" / "utf8name";
    fs::create_directories(dir);

    // 拿一张**真的**板子当素材 —— 空白图判不出东西(会被当成拒绝判定),
    // 那样测的就不是"文件名编码",而是"图对不对"了。
    const fs::path src = g_w6 / "images" / "complex" / "power_baseline.png";
    const fs::path png = dir / utf8path::fromUtf8("测试板.png");
    check(copyFileTo(src, png), "复制出中文名的板子");
    check(fs::exists(png), "中文名的板子真在磁盘上");

    // 真值表也用 UTF-8 写(和仓库里其它 manifest 一样)
    {
        std::ofstream mf(dir / "manifest.csv");
        mf << "file,kind\n";
        mf << "测试板.png,power\n";   // 源文件是 UTF-8,写出去就是 UTF-8 字节
    }

    // --- (a) 查得到板型 ---
    {
        const std::string kind = w6::boardKindOf(png);
        check(kind == "power",
              "中文文件名也查得到板型(实际 \"" + kind + "\")");
    }

    // --- (b) 批量报告里被"判",不是被"跳过" ---
    {
        const w6::ReportResult rep = w6::runReport(dir, g_recipes, false);
        check(rep.boardCount == 1,
              "中文名板子被判了 1 块(实际 " + std::to_string(rep.boardCount) + ")");
        check(rep.skipCount == 0,
              "中文名板子没有被跳过(实际跳过 " + std::to_string(rep.skipCount) + ")");
        if (!rep.rows.empty()) {
            check(rep.rows[0].boardFile == "测试板.png",
                  "CSV 里的 board_file 是中文名(实际 \"" + rep.rows[0].boardFile + "\")");
        }
    }
}

// ============================================================
// 用例 4:CSV 转义
//
// 不转义的后果是**静默**的列错位:一列变两列,程序不报错,
// 用 pandas 读才发现不对。
// ============================================================
static void test_csv_escape() {
    beginCase("CSV 转义:含逗号/引号/换行的字段必须包起来");

    check(w6::csvEscape("abc") == "abc", "普通字符串原样返回");
    check(w6::csvEscape("") == "", "空串原样返回");
    check(w6::csvEscape("a b") == "a b", "只有空格不用包");
    check(w6::csvEscape("a,b") == "\"a,b\"", "含逗号 -> 加引号");
    check(w6::csvEscape("a\"b") == "\"a\"\"b\"", "含引号 -> 加引号且引号双写");
    check(w6::csvEscape("a\nb") == "\"a\nb\"", "含换行 -> 加引号");
    check(w6::csvEscape("a\r\nb") == "\"a\r\nb\"", "含 CRLF -> 加引号");
    check(w6::csvEscape("a,b\"c\nd") == "\"a,b\"\"c\nd\"",
          "逗号+引号+换行混在一起也要正确转义");

    // 转义之后不能再出现"未转义的引号",否则 CSV 解析器会把后面的列吞掉。
    {
        const std::string s = "he said \"hi\", ok\n";
        const std::string e = w6::csvEscape(s);
        int quotes = 0;
        for (char c : e) if (c == '"') ++quotes;
        check(quotes % 2 == 0, "转义后引号成对(实际 " + std::to_string(quotes) + " 个)");
    }
}

// ============================================================
// 用例 5:摊行
//
// CSV 里一个元件一行。同一个元件可能同时有多种缺陷
// (比如错色 + 焊桥),**两种都要写出来** ——
// 只写一种,看报告的人会以为只有那一种缺陷。
// ============================================================

// 手工造一个站点。
//
// ⚠ 用**具名字段赋值**,不用位置聚合初始化 `{...}`:
//   InspectSite 有十几个字段,位置初始化要求**顺序和个数全对**。
//   结构体中间插一个字段,所有位置初始化整体错位 ——
//   错到 bool 塞进 bool 还能编过,行为却悄悄变了。
//   (2026-10-20 加 polaritySide 时真踩了:插入位置不对,
//    MSVC 直接报"无法把 bool 转成 std::string",当场暴露;
//    但那只是运气好 —— 换成两个同类型字段就会静默错位。)
static pcbcore::InspectSite makeSite(const std::string& label,
                                     const std::string& kind) {
    pcbcore::InspectSite s;
    s.label = label;
    s.kind = kind;
    s.checksExplicit = true;   // 配方说了算(哪怕 checks 是空的)
    return s;
}

static void test_rows_from_board() {
    beginCase("摊行:一个元件一行,多个缺陷全写出来");

    // --- 手工造一块板,精确控制缺陷 ---
    {
        w6::BoardResult br;
        br.recipeName = "demo";
        br.pass = false;
        br.sites.push_back(makeSite("R1", "RES"));
        br.sites.push_back(makeSite("C1", "CAP_SMALL"));
        br.sites.push_back(makeSite("U1", "QFP64"));
        br.defects.push_back({"R1", "wrong", "本体错色"});
        br.defects.push_back({"R1", "bridge", "焊盘间金条连片"});
        br.defects.push_back({"", "short", "全图短路"});   // 全局缺陷

        const auto rows = w6::rowsFromBoard("b.png", br, "SILK", "QR123");
        check(rows.size() == 4,
              "3 个元件 + 1 个全局缺陷 = 4 行(实际 " +
              std::to_string(rows.size()) + ")");

        // 找 R1 那一行
        const w6::ReportRow* r1 = nullptr;
        for (const auto& r : rows) if (r.component == "R1") r1 = &r;
        check(r1 != nullptr, "有 R1 那一行");
        if (r1) {
            check(r1->defect == "wrong|bridge",
                  "R1 的两种缺陷都要写出来(实际 \"" + r1->defect + "\")");
            check(r1->description.find("本体错色") != std::string::npos &&
                  r1->description.find("金条连片") != std::string::npos,
                  "R1 的两种说明都要写出来(实际 \"" + r1->description + "\")");
            check(r1->result == "FAIL", "R1 行的 result 是 FAIL");
            check(r1->boardFile == "b.png", "R1 行的 board_file 对");
            check(r1->boardType == "demo", "R1 行的 board_type 对");
            check(r1->silk == "SILK" && r1->qr == "QR123", "R1 行的 silk/qr 对");
        }

        // 没缺陷的元件
        const w6::ReportRow* c1 = nullptr;
        for (const auto& r : rows) if (r.component == "C1") c1 = &r;
        check(c1 != nullptr, "有 C1 那一行");
        if (c1) {
            check(c1->defect == "ok", "没缺陷的元件写 ok(实际 \"" + c1->defect + "\")");
            check(c1->description == "-", "没缺陷的元件说明写 -");
        }

        // 全局缺陷:单独一行,component 留空
        const w6::ReportRow* gs = nullptr;
        for (const auto& r : rows) if (r.component.empty()) gs = &r;
        check(gs != nullptr, "全局缺陷单独成行");
        if (gs) {
            check(gs->defect == "short", "全局缺陷行写的是 short");
            check(gs->result == "FAIL", "全局缺陷行的 result 是 FAIL");
        }
    }

    // --- 合格板:全部写 ok ---
    {
        w6::BoardResult br;
        br.recipeName = "demo";
        br.pass = true;
        br.sites.push_back(makeSite("R1", "RES"));
        const auto rows = w6::rowsFromBoard("ok.png", br, "", "");
        check(rows.size() == 1, "合格板 1 个元件 = 1 行");
        if (!rows.empty()) {
            check(rows[0].defect == "ok", "合格板写 ok");
            check(rows[0].result == "PASS", "合格板 result 是 PASS");
        }
    }

    // --- 真数据:一个缺陷都不能丢 ---
    //
    // 用真图跑一遍,拿 runBoard 给出的缺陷集合和摊行后的结果对账。
    // 这条比上面手工造的更强:它跟着真值表走,换了板型也照样成立。
    {
        const auto rows = readCsv(g_w6 / "images" / "complex" / "complex_manifest.csv");
        if (rows.empty()) {
            check(false, "读得到 complex_manifest.csv");
            return;
        }
        const int fileCol = colOf(rows[0], "file");
        const int kindCol = colOf(rows[0], "kind");
        for (size_t r = 1; r < rows.size(); ++r) {
            if (static_cast<int>(rows[r].size()) <= kindCol) continue;
            const std::string file = rows[r][fileCol];
            const std::string kind = rows[r][kindCol];

            w6::BoardResult br;
            std::string err;
            if (!w6::runBoard(g_w6 / "images" / "complex" / file,
                              g_recipes, br, err)) {
                check(false, kind + "/" + file + " 能判:" + err);
                continue;
            }
            const auto out = w6::rowsFromBoard(file, br, "", "");

            // 期望:br.defects 里的每一条,都要在摊出来的行里出现
            std::multiset<std::string> want, got;
            for (const auto& d : br.defects) want.insert(d.type);
            for (const auto& row : out) {
                if (row.defect.empty() || row.defect == "ok") continue;
                std::string cur;
                std::istringstream ss(row.defect);
                while (std::getline(ss, cur, '|')) got.insert(cur);
            }
            check(want == got,
                  kind + "/" + file + " 摊行后缺陷一个不少(期望 " +
                  std::to_string(want.size()) + " 条,实际 " +
                  std::to_string(got.size()) + " 条)");
        }
    }
}

// ============================================================
// 用例 6:批量报告
//
// 这一组盯的是"少检了一块板却没人知道"——
// 本项目栽过一次的坑:用非递归目录遍历,complex/ 里 9 块板被静默跳过。
// ============================================================
static void test_run_report() {
    beginCase("批量报告:递归、相对路径、跳过必须报出来");

    const fs::path images = g_w6 / "images";
    const w6::ReportResult rep = w6::runReport(images, g_recipes, false);

    // --- 数量对账:判过的 + 跳过的 = 目录里 PNG 的总数 ---
    //
    // ⚠ 这一条是本组的总纲:**一张图都不许悄悄消失**。
    //   少一张,报告上就少一块板,而看报告的人看不出来。
    {
        const int total = countPng(images);
        check(static_cast<int>(rep.rows.size()) > 0, "报告里有行");
        check(rep.boardCount + rep.skipCount == total,
              "判过的(" + std::to_string(rep.boardCount) + ") + 跳过的(" +
              std::to_string(rep.skipCount) + ") = PNG 总数(" +
              std::to_string(total) + ")");
    }

    // --- 递归:complex/ 里的板必须也被判了 ---
    //
    // 原来用非递归遍历,complex/ 里的板被静默跳过 —— 报告看着完整,
    // 实际少检了一整片(当时是 9 块,现在是 13 块)。这条断言就是钉这个的。
    {
        const auto complexPng = listPngRel(images / "complex");
        int judged = 0;
        for (const auto& rel : complexPng) {
            for (const auto& row : rep.rows) {
                if (row.boardFile == "complex/" + rel) { ++judged; break; }
            }
        }
        check(judged == static_cast<int>(complexPng.size()),
              "complex/ 里 " + std::to_string(complexPng.size()) +
              " 块板都被判了(实际 " + std::to_string(judged) + ")");
    }

    // --- 板子数与真值表一致 ---
    {
        const auto demo = readCsv(images / "manifest.csv");
        const auto cpx  = readCsv(images / "complex" / "complex_manifest.csv");
        const int want = static_cast<int>((demo.empty() ? 1 : demo.size() - 1) +
                                          (cpx.empty()  ? 1 : cpx.size()  - 1));
        check(rep.boardCount == want,
              "判了 " + std::to_string(want) + " 块板(实际 " +
              std::to_string(rep.boardCount) + ")");
    }

    // --- 相对路径:不能是绝对路径 ---
    //
    // 绝对路径写进 CSV,换台机器就全废了;而且报告里出现
    // "E:\qianwie\..." 这种路径,一看就知道是开发机上的临时产物。
    {
        int absCount = 0;
        for (const auto& row : rep.rows) {
            if (row.boardFile.empty()) continue;
            if (row.boardFile.size() > 1 && row.boardFile[1] == ':') ++absCount;
            if (row.boardFile[0] == '/' || row.boardFile[0] == '\\') ++absCount;
        }
        check(absCount == 0,
              "board_file 全是相对路径(绝对路径有 " +
              std::to_string(absCount) + " 个)");
    }

    // --- 相对路径的形状:top-level 是 "x.png",子目录是 "complex/x.png" ---
    {
        bool sawTop = false, sawNested = false;
        for (const auto& row : rep.rows) {
            if (row.boardFile.find('/') == std::string::npos) sawTop = true;
            else if (row.boardFile.rfind("complex/", 0) == 0) sawNested = true;
        }
        check(sawTop, "有顶层图(board_file 不带目录)");
        check(sawNested, "有子目录图(board_file 带 complex/ 前缀)");
    }

    // --- 跳过的必须逐条有原因 ---
    //
    // ⚠ 这里**自己造**一张 inspect 叠加图,而不是指望仓库 images/ 里正好有一张。
    //
    // 以前这条断言是"images/ 里躺着 w6_inspect_*.png(inspect 的旧产物),
    // 所以 report 会跳过它" —— 它断言的是**仓库里恰好有的脏东西**。
    // 2026-10-20 把 inspect 的产物从 images/ 搬到 out/ 之后,这条断言就红了:
    // 它红的不是判据变了,是**它依赖的那个偶然没了**。
    //
    // 依赖"仓库里正好有脏文件"的断言本身就是脆的:哪天有人清理了 images/,
    // 测试就红,而代码一行没改。行为要测,就用测试自己造的输入去测。
    {
        const fs::path overlay =
            fs::current_path() / "w6_cli_test_tmp" / "overlay";
        fs::create_directories(overlay);
        cv::Mat blank(120, 160, CV_8UC3, cv::Scalar(40, 90, 40));
        writePng(overlay / "w6_inspect_probe.png", blank);

        const w6::ReportResult o =
            w6::runReport(overlay, g_recipes, false);
        check(o.skipCount == 1,
              "inspect 的叠加图被跳过,没被当成板子判(实际跳过 " +
              std::to_string(o.skipCount) + " 张)");
        check(o.rows.empty(), "叠加图不产出任何行");
        check(!o.skipped.empty() &&
                  o.skipped[0].file.find("w6_inspect_probe") != std::string::npos,
              "跳过项写明是哪张图");
        check(!o.skipped.empty() && !o.skipped[0].reason.empty(),
              "跳过项必须给原因(不能一声不吭)");
    }

    // --- 一个都不许漏:每张 PNG 要么被判、要么被报为跳过 ---
    {
        std::set<std::string> accounted;
        for (const auto& row : rep.rows) accounted.insert(row.boardFile);
        for (const auto& s : rep.skipped) accounted.insert(s.file);
        int missing = 0;
        std::string missingName;
        for (const auto& rel : listPngRel(images)) {
            if (accounted.count(rel)) continue;
            ++missing;
            if (missingName.empty()) missingName = rel;
        }
        check(missing == 0,
              "每张 PNG 都有交代(" + std::to_string(missing) +
              " 张下落不明,例如 " + missingName + ")");
    }

    // --- 一个 manifest 都没有的目录:全部跳过,一块板都不许"判了" ---
    {
        const fs::path orphan = fs::current_path() / "w6_cli_test_tmp" / "orphan";
        fs::create_directories(orphan);
        cv::Mat blank(120, 160, CV_8UC3, cv::Scalar(40, 90, 40));
        writePng(orphan / "a.png", blank);
        writePng(orphan / "b.png", blank);

        const w6::ReportResult o = w6::runReport(orphan, g_recipes, false);
        check(o.boardCount == 0, "没有真值表的目录 -> 一块板都不判");
        check(o.skipCount == 2, "两张图都记成跳过(实际 " +
              std::to_string(o.skipCount) + ")");
        check(o.rows.empty(), "跳过的板不产出任何行");
        for (const auto& s : o.skipped) {
            check(!s.reason.empty(), "跳过原因非空");
        }
    }
}

// ============================================================
// 用例 7:写 CSV
// ============================================================
static void test_write_csv() {
    beginCase("写 CSV:表头只写一次,列数对得上");

    const fs::path dir = fs::current_path() / "w6_cli_test_tmp";
    fs::create_directories(dir);
    const fs::path csv = dir / "out.csv";
    fs::remove(csv);

    w6::ReportRow a;
    a.boardFile = "b.png"; a.boardType = "demo"; a.component = "R1";
    a.kind = "RES"; a.defect = "ok"; a.description = "-";
    a.silk = "S1"; a.qr = "Q1"; a.result = "PASS";

    w6::ReportRow b = a;
    b.component = "C1"; b.defect = "wrong"; b.description = "本体错色";
    b.result = "FAIL";

    check(w6::writeCSVReport(csv, {a, b}, true), "写 CSV 成功");

    const auto lines = readCsv(csv);
    check(lines.size() == 3, "表头 + 2 行 = 3 行(实际 " +
          std::to_string(lines.size()) + ")");
    if (lines.size() == 3) {
        const int headerCols = colOf(lines[0], "board_file");
        check(headerCols == 0, "第一列是 board_file");
        check(colOf(lines[0], "board_type") >= 0, "有 board_type 列");
        check(colOf(lines[0], "component") >= 0, "有 component 列");
        check(colOf(lines[0], "result") >= 0, "有 result 列");
        check(lines[0].size() == 9, "表头 9 列(实际 " +
              std::to_string(lines[0].size()) + ")");
        check(lines[1].size() == 9, "数据行 9 列(实际 " +
              std::to_string(lines[1].size()) + ")");
        // 上一轮修过的 bug:board_file 列写的是"报告输出路径"
        // (report/baseline.csv,R1,...)而不是板子文件名。
        check(lines[1][0] == "b.png",
              "board_file 列是板子文件名(实际 \"" + lines[1][0] + "\")");
        check(lines[1][2] == "R1", "component 列对(实际 \"" + lines[1][2] + "\")");
        check(lines[1][8] == "PASS", "result 列对(实际 \"" + lines[1][8] + "\")");
    }

    // --- 追加:不重复写表头 ---
    {
        w6::ReportRow c = a;
        c.component = "U1";
        check(w6::writeCSVReport(csv, {c}, false), "追加 CSV 成功");
        const auto lines2 = readCsv(csv);
        check(lines2.size() == 4, "追加后是 4 行(实际 " +
              std::to_string(lines2.size()) + ")");
        if (lines2.size() == 4) {
            check(lines2[0][0] == "board_file", "表头没有重复出现");
            check(lines2[3][2] == "U1", "追加的是新行");
        }
    }

    // --- 表头行不能漏 ---
    //
    // 之前 report 模式是"按行数决定要不要写表头",
    // 第一张图要是被跳过,表头就没了 —— pandas 读第一行就成了列名。
    {
        const fs::path csv2 = dir / "empty.csv";
        fs::remove(csv2);
        check(w6::writeCSVReport(csv2, {}, true), "写空报告成功");
        const auto lines3 = readCsv(csv2);
        check(lines3.size() == 1, "一张行都没有时也要写表头(实际 " +
              std::to_string(lines3.size()) + " 行)");
        if (!lines3.empty()) {
            check(lines3[0][0] == "board_file", "空报告的表头对");
        }
    }
}

// ============================================================
// 用例 8:读码 —— "没读"和"读了但是空"必须分得开
//
// ============================================================
// 背景:2026-10-20 把 QR/OCR 搬到 common/qr_ocr/ 之后,
// 顺手逮到 W6 的一个真缺陷:
//
//     std::string qr, silk, silkErr;
//     if (doSilkscreen) silk = readSilkscreen(img, silkErr);
//     // ← silkErr 算出来了,之后再也没被用过
//
// 失败原因算出来就丢掉,报告里 silk 列空着 ——
// 和"板上没丝印"长得一模一样。看报告的人分不清是哪个。
//
// 这一组用例钉的是那个区分:
//   silkRead = false              -> **没读**(开关关着 / 读失败)
//   silkRead = true 且 silk 为空    -> 读了,图上确实没字
//
// 而"没读"的原因必须能被报出来 ——
// 批量报告里多了一个 readFailures 通道(和 skipped 分开,
// 因为"板没判"和"板判了但丝印没读到"是两件事)。
// ============================================================
static void test_read_codes() {
    beginCase("读码:没读 / 读了但是空,必须分得开;失败必须报出来");

    const fs::path qrImg = g_root / "W5_OCR_QRCode" / "images" / "qrcode_test.png";
    const cv::Mat img = w6::loadImage(qrImg);
    check(!img.empty(), "读得到 " + utf8path::text(qrImg));
    if (img.empty()) return;

    // --- QR 内容对 ---
    {
        const w6::BoardCodes c = w6::readBoardCodes(img, false);
        check(c.qrFound, "QR 找到了");
        // 期望值来自 W5 自己的生成器 gen_qr.py
        check(c.qr == "PCB-2026-001-SN:A12345X",
              "QR 内容对(实际 \"" + c.qr + "\")");
    }

    // --- 开关关着 = "没读",不是"读了但是空" ---
    {
        const w6::BoardCodes c = w6::readBoardCodes(img, false);
        check(!c.silkRead, "开关关着 -> silkRead=false");
        check(!c.silkError.empty(), "开关关着 -> 说清是开关关着,不是空白");
    }

    // --- 指定一个不存在的 tesseract -> 必须失败并说清楚 ---
    {
        // ⚠ 这里以前是 `const std::string bogus = "D:/绝对不存在的路径/..."`,
        //   然后直接喂给 readBoardCodes()。
        //   API 改成 fs::path 之后,**隐式**转换还在:
        //   fs::path(const std::string&) 按 **ACP** 解释那段窄串,
        //   于是路径当场被拆成 GBK 字节 —— 错误消息里写的是
        //   「D:/缁濆涓嶅瓨鍦ㄧ殑璺緞/tesseract.exe」。
        //   本机 ACP=936 把 24 字节 UTF-8 读成 12 个汉字,
        //   runner 的 ACP=1252 读成 24 个 Latin-1 字符 ——
        //   **两台机器都错**,只是本机错得"像是对的"。
        //   现在窄串一律先走 utf8path::fromUtf8()(唯一的一处定义),
        //   而 w6_cli.h 里的守卫会**让这种写法编译不过**。
        const fs::path bogus = utf8path::fromUtf8("D:/绝对不存在的路径/tesseract.exe");
        const w6::BoardCodes c = w6::readBoardCodes(img, true, bogus);
        check(!c.silkRead, "tesseract 不存在 -> silkRead=false");
        check(c.silk.empty(), "tesseract 不存在 -> 没有返回任何文字");
        check(c.silkError.find(utf8path::text(bogus)) != std::string::npos,
              "失败原因里写明了路径(实际 \"" + c.silkError + "\")");
    }

    // --- 空图 ---
    {
        const cv::Mat empty;
        const w6::BoardCodes c = w6::readBoardCodes(empty, true);
        check(!c.silkRead, "空图 -> silkRead=false");
        check(!c.silkError.empty(), "空图 -> 说清了为什么");
    }
}

// ============================================================
// 用例 9:批量报告里,没读成的读取必须**报出来**
//
// ⚠ 这条是本组用例里最要紧的:以前失败原因算出来就丢掉,
//   报告里 silk 列空着,和"板上没丝印"分不开。
//
// 而且它**不能混进 skipped** ——
//   skipped = 这块板**没判**,结论不成立;
//   读失败 = 板判了,只是丝印那一栏是空的。
// 混在一起会让看报告的人以为"少检了一块板",去查板型/配方 ——
// 而真正的问题是 tesseract 没装。
// ============================================================
static void test_report_reports_read_failures() {
    beginCase("批量报告:没读成的读取要单列,不能混进 skipped");

    const fs::path dir = g_w6 / "images";
    // 明确按 UTF-8 转 —— 这段中文是源文件里的 UTF-8 字节。
    // (别用 fs::path(窄串):那是按 ACP 解释,本机读成 12 个汉字、
    //  runner 读成 24 个 Latin-1 字符 —— 又一个"每台机器都错"的东西。)
    const fs::path bogus = utf8path::fromUtf8("D:/绝对不存在的路径/tesseract.exe");

    const w6::ReportResult rep =
        w6::runReport(dir, g_recipes, true, bogus);

    check(rep.boardCount > 0, "跑到了板(实际 " + std::to_string(rep.boardCount) + " 块)");
    check(!rep.readFailures.empty(),
          "OCR 全部失败 -> readFailures 不为空(实际 " +
          std::to_string(rep.readFailures.size()) + " 条)");
    if (!rep.readFailures.empty()) {
        const auto& f = rep.readFailures.front();
        check(!f.file.empty(), "失败项写了是哪张图");
        check(f.what == "silkscreen", "失败项写了读的是什么(实际 \"" + f.what + "\")");
        check(f.reason.find(utf8path::text(bogus)) != std::string::npos,
              "失败原因里写明了路径(实际 \"" + f.reason + "\")");
    }

    // 关键:**不能混进 skipped**。
    // skipped 是"板没判",读失败是"板判了但丝印没读到"。
    check(rep.skipCount == static_cast<int>(rep.skipped.size()),
          "skipped 的条数和 skipCount 对得上");
    for (const auto& s : rep.skipped) {
        check(s.reason.find("tesseract") == std::string::npos,
              "skipped 里不含 OCR 失败(\"" + s.reason + "\")");
    }

    // --- 开关关着时,不算失败 ---
    {
        const w6::ReportResult off = w6::runReport(dir, g_recipes, false);
        check(off.readFailures.empty(),
              "开关关着 -> 不算读取失败(实际 " +
              std::to_string(off.readFailures.size()) + " 条)");
    }
}

int main() {
    g_root = findRepoRoot();
    g_w6   = g_root / "W6_PCB_DefectProject";
    g_recipes = g_root / "W7_VisionMasterLite" / "recipes";

    // 测试产物目录:每次跑之前清干净,免得上一轮的残留混进来。
    const fs::path tmp = fs::current_path() / "w6_cli_test_tmp";
    fs::remove_all(tmp);

    std::cout << "=== W6 CLI 回归测试 ===" << std::endl;
    std::cout << "仓库根: " << utf8path::text(g_root) << std::endl;
    std::cout << "产物目录: " << utf8path::text(tmp) << std::endl;

    if (!fs::exists(g_w6) || !fs::exists(g_recipes)) {
        check(false, "找得到 W6 数据目录和配方目录");
        std::cout << "\nSUMMARY: pass=" << g_pass << " fail=" << g_fail
                  << "   RESULT=FAIL" << std::endl;
        return 1;
    }

    // 顶层 try/catch:测试里抛出的异常必须变成一条**看得懂的**失败,
    // 而不是直接崩掉(崩掉的话 ctest 只给一个退出码,看不出是哪一步坏的)。
    try {
        test_board_kind();
        test_recipe_dir();
        test_path_encoding_roundtrip();
        test_run_board();
        test_non_ascii_filename();
        test_csv_escape();
        test_rows_from_board();
        test_run_report();
        test_write_csv();
        test_read_codes();
        test_report_reports_read_failures();
    } catch (const std::exception& e) {
        check(false, std::string("测试抛出异常:") + e.what());
    } catch (...) {
        check(false, "测试抛出非 std::exception 的异常");
    }

    fs::remove_all(tmp);
    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
