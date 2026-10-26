// ============================================================
// W6 的可测逻辑层 —— 实现
//
// 见 w6_cli.h 顶部的说明:这一层是纯逻辑,给输入出结果,不打印。
// 打印是 main.cpp 的事。
// ============================================================

#include "w6_cli.h"

#include "qr_ocr.h"
#include "utf8_path.h"

#include <fstream>
#include <sstream>
#include <map>
#include <iterator>

#ifdef _WIN32
// windows.h 会把 min/max 定义成宏,不加 NOMINMAX 的话
// `std::max(...)` 会被展开成 `std::(max)(...)` -> C2589。
// 共享核心里也用了 std::max/std::min,所以这里必须挡掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace w6 {

// ============================================================
// 读图
// ============================================================
cv::Mat loadImage(const fs::path& p) {
    // 自己按宽路径把字节读进来,再 imdecode。
    //
    // ⚠ 为什么不用 cv::imread:它内部按 ANSI 打开文件,
    //   中文路径读不出来 —— 而且**不报错**,只给你一个空 Mat,
    //   上层看到空图就报 "imread failed",看起来像文件坏了,
    //   其实是路径编码的事。
    std::ifstream f(p, std::ios::binary);
    if (!f) return cv::Mat();
    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (buf.empty()) return cv::Mat();
    const cv::Mat raw(1, static_cast<int>(buf.size()), CV_8UC1, buf.data());
    return cv::imdecode(raw, cv::IMREAD_COLOR);
}

// ============================================================
// 极简 CSV 读(真值表里没有引号和逗号转义)
//
// ⚠ 按**原始字节**读,不做编码转换 —— 仓库里的 manifest.csv 是 UTF-8,
//   读出来就是 UTF-8 字节,正好和 utf8path::text() 的输出同一套编码。
//   (要是哪天改了编码,两边的比较会一起失效 ——
//   所以改 manifest 编码时必须同时改这里。)
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

std::string boardKindOf(const fs::path& imgPath) {
    const fs::path dir = imgPath.parent_path();
    // 文件名按 UTF-8 取,和 manifest.csv 里的内容同一套编码。
    const std::string fname = utf8path::text(imgPath.filename());
    if (!fs::exists(dir)) return "";
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".csv") continue;
        const std::string name = utf8path::text(e.path().filename());
        if (name.find("manifest") == std::string::npos) continue;

        const auto rows = readCsv(e.path());
        if (rows.empty()) continue;
        // 表头:找出 file 列和 kind 列的位置
        int fileCol = -1, kindCol = -1;
        for (size_t i = 0; i < rows[0].size(); ++i) {
            if (rows[0][i] == "file")  fileCol = static_cast<int>(i);
            if (rows[0][i] == "kind")  kindCol = static_cast<int>(i);
        }
        if (fileCol < 0) continue;
        for (size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (static_cast<int>(row.size()) <= fileCol) continue;
            if (row[fileCol] != fname) continue;
            if (kindCol < 0) {
                // 这张表没有 kind 列 = demo 板
                return "demo";
            }
            // ⚠ 表里有 kind 列,但这行**没有**kind 单元格,或者单元格里是空的
            //   —— 这是"查不到",不是"默认 demo"。
            //   猜错了的后果:拿 demo 的配方去判一块 mcu 板,
            //   元件框全对不上,判出来的东西是编的。
            if (static_cast<int>(row.size()) <= kindCol) break;
            const std::string kind = row[kindCol];
            if (kind.empty()) break;
            return kind;
        }
    }
    return "";
}

// ============================================================
// 板型配方目录
// ============================================================
static const char* kRecipeDirs[] = {
    "recipes",                                  // 编译时拷到 exe 旁边
    "../W7_VisionMasterLite/recipes",           // 仓库里跑
    "W7_VisionMasterLite/recipes",
};

// 目录里至少有一份 .json 才算配方目录
static bool looksLikeRecipeDir(const fs::path& d) {
    if (!fs::exists(d) || !fs::is_directory(d)) return false;
    for (const auto& e : fs::directory_iterator(d)) {
        if (e.is_regular_file() && e.path().extension() == ".json") return true;
    }
    return false;
}

fs::path findRecipeDir(const fs::path& overrideDir) {
    if (!overrideDir.empty()) {
        // ⚠ 指定的目录不对就**返回空**,别原样返回一个坏路径。
        //   原样返回的话,每块板各报一次"配方加载失败",刷屏,
        //   而且真正的原因(目录写错了)被埋在最后一条错误里。
        return looksLikeRecipeDir(overrideDir) ? overrideDir : fs::path();
    }
    for (const char* d : kRecipeDirs) {
        const fs::path p(d);
        if (looksLikeRecipeDir(p)) return p;
    }
    return fs::path();
}

// ============================================================
// W6d: 丝印 OCR / QR 串号
//
// ⚠ 2026-10-20:原来这里有一份 readSilkscreen() 和一份 readQRCode(),
//   **删掉了** —— 搬到 common/qr_ocr/,W5 / W6 / W7 共用一份。
//
//   这里是本项目**第五次**抄同一个副本。前三次分别长在:
//     · W5 的 main.cpp(QR 解码 + tesseract 调用)
//     · 本文件(抄 W5 的,修了宽字符转换和 --psm 11)
//     · W7 的 qr_decode 算子(又抄了一份 QR 解码)
//
//   抄出来的代价不是"多几行代码":
//     · W6 的 --psm 11 和 W5 的 --psm 6 是**两个不同的答案**,
//       谁也说不清哪个对 —— 它们是两份拷贝,不是一份的两个参数。
//     · 本文件修了宽字符转换的坑(路径带中文会毁),
//       W5 那份还在逐元素窄化。修一处,另一处照旧坏着,
//       而且没人会怀疑它们本来应该一样。
//
//   现在两边都是 qroc::readText() / qroc::readQr(),
//   psm 是一个**参数**,改一处三边跟着动。
// ============================================================

// 一块板的码怎么读,只在这里定义一次。
//
// baseline 模式(单板)和 report 模式(批量)都要读,
// 之前是各写一份 —— 又是抄副本,而且两份的参数已经开始漂
// (一个 psm 11、一个 psm 6,谁也说不清哪个对)。
BoardCodes readBoardCodes(const cv::Mat& img, bool doSilkscreen,
                          const fs::path& tesseractPath) {
    BoardCodes out;
    if (img.empty()) {
        out.silkError = "图像是空的,没读";
        return out;
    }

    const qroc::QrResult qr = qroc::readQr(img);
    out.qr = qr.text;
    out.qrFound = qr.found;

    if (!doSilkscreen) {
        // 开关关着 —— 这是"**没读**",不是"读了但是空"。
        out.silkRead = false;
        out.silkError = "没跑 OCR(开关关着)";
        return out;
    }

    qroc::OcrOptions opt;
    // --psm 11 = "sparse text"(找散落的文字),不是 6("uniform block")。
    // 板上的丝印是**散落的**(板号在一角、SN 标在二维码下面、
    // 每个元件上面还有位号),按整块文本排版去解释会把版面分析带偏。
    opt.psm = 11;
    // 中间产物放 out/,**不能放 images/** ——
    // images/ 是批量报告的输入目录,往里丢中间产物,下一次 report
    // 就会把自己的中间产物当成待检板扫一遍(而且它没有 manifest 登记,
    // 会被报成"跳过",看着像少检了一块板)。
    // ⚠ 写成 fs::path(...),不是裸窄串 ——
    //   成员赋值不走函数调用,编译器拦不住窄串(见 utf8_path.h 的「局限」),
    //   所以这一处得**自己**写清楚:这是路径,不是文本。
    opt.workDir = fs::path("out/silk");
    opt.stem = "w6_silk";
    // 显式指定时**不会**悄悄换一个 tesseract 去跑 ——
    // 拿另一个程序跑出来的结果,调用方却以为是自己指定的那个。
    opt.tesseractPath = tesseractPath;

    const qroc::OcrResult r = qroc::readText(img, opt);
    out.silkRead = r.ok;
    out.silk = r.text;
    out.silkError = r.error;
    return out;
}

// ============================================================
// 跑一块板
// ============================================================
bool runBoard(const fs::path& imgPath, const fs::path& recipeDir,
              BoardResult& out, std::string& errOut) {
    // --- 板型 ---
    const std::string kind = boardKindOf(imgPath);
    if (kind.empty()) {
        errOut = "找不到这块板属于哪种板型(同目录里没有登记它的 manifest.csv)"
                 "—— 拿不到配方就没法定位元件,判定会给出错误结论";
        return false;
    }
    // kind 来自 manifest.csv,那是 **UTF-8** 文本 ⇒ 拼文件名要走 fromUtf8。
    // (直接 `recipeDir / (kind + ".json")` 的话,MSVC 会按 ACP 解释
    //  那段窄串 —— 本机 GBK 把 24 字节 UTF-8 读成 12 个汉字,
    //  runner 的 ACP=1252 读成 24 个 Latin-1 字符:
    //  **两边都不是原来那条路径**,而且谁都不报错。)
    const fs::path recipePath = recipeDir / utf8path::fromUtf8(kind + ".json");
    pcbcore::BoardRecipe recipe;
    std::string reason;
    if (!pcbcore::loadRecipeFile(recipePath, &recipe, &reason)) {
        errOut = "板型配方加载失败:" + reason;
        return false;
    }
    out.recipeName = recipe.name;
    out.boardTitle = recipe.title;

    // --- 读图 ---
    cv::Mat img = loadImage(imgPath);
    if (img.empty()) {
        errOut = "读不出图片: " + utf8path::text(imgPath);
        return false;
    }

    // --- 定位 + 判定 ---
    const pcbcore::JudgeParams params;
    const pcbcore::InspectResult ir =
        pcbcore::inspect(img, recipe, pcbcore::pcbcolor::goldBand(params.maskVMul));
    const pcbcore::JudgeResult jr =
        pcbcore::judge(img, ir.sites, params, ir.goldMask, ir.boardBase);
    if (jr.refused) {
        errOut = jr.refuseReason;
        return false;
    }

    out.sites          = ir.sites;
    out.defects        = jr.defects;
    out.metrics        = jr.metrics;
    out.skipped        = jr.skipped;
    out.marked         = jr.marked;
    out.componentCount = static_cast<int>(ir.sites.size());
    out.withRules      = jr.withRules;
    out.pass           = jr.defects.empty();

    // 定位阶段被丢掉的站点(算不出有效 ROI)也算"没查",要说出来
    for (const auto& s : ir.skippedSites) {
        out.skipped.push_back({s, "", "这块图上算不出有效 ROI,站点被跳过"});
    }
    return true;
}

// ============================================================
// 把一块板摊成 CSV 行
// ============================================================
std::vector<ReportRow> rowsFromBoard(const std::string& relPath,
                                     const BoardResult& br,
                                     const std::string& silk,
                                     const std::string& qr) {
    std::vector<ReportRow> rows;
    const std::string verdict = br.pass ? "PASS" : "FAIL";

    // 把缺陷按元件归拢
    std::map<std::string, std::vector<const pcbcore::Defect*>> byComponent;
    for (const auto& d : br.defects) byComponent[d.component].push_back(&d);

    for (const auto& s : br.sites) {
        ReportRow row;
        row.boardFile = relPath;
        row.boardType = br.recipeName;
        row.component = s.label;
        row.kind      = s.kind;
        row.silk      = silk;
        row.qr        = qr;
        row.result    = verdict;
        auto it = byComponent.find(s.label);
        if (it != byComponent.end() && !it->second.empty()) {
            // 一个元件可能同时有多种缺陷(比如 wrong + bridge),
            // 全都写出来,别只写第一个 —— 只写第一个会让人以为只有那一种。
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i) { row.defect += "|"; row.description += " | "; }
                row.defect      += it->second[i]->type;
                row.description += it->second[i]->detail;
            }
        } else {
            row.defect = "ok";
            row.description = "-";
        }
        rows.push_back(row);
    }

    // 全局缺陷(短路等,不归属某个元件)
    for (const auto& d : br.defects) {
        if (!d.component.empty()) continue;
        ReportRow row;
        row.boardFile   = relPath;
        row.boardType   = br.recipeName;
        row.component   = "";
        row.kind        = "";
        row.defect      = d.type;
        row.description = d.detail;
        row.silk        = silk;
        row.qr          = qr;
        row.result      = verdict;
        rows.push_back(row);
    }
    return rows;
}

// ============================================================
// 跑一批板
// ============================================================
ReportResult runReport(const fs::path& dir, const fs::path& recipeDir,
                       bool doSilkscreen, const fs::path& tesseractPath) {
    ReportResult res;

    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".png") continue;

        // 相对 dir 的路径。用相对路径而不是文件名,是因为递归之后
        // filenames 不再唯一(complex/mcu_wrong.png 和顶层的重名也能撞)。
        const std::string rel = utf8path::text(fs::relative(e.path(), dir));
        const std::string name = utf8path::text(e.path().filename());

        // 过滤掉 inspect 生成的 marked 图(带叠加层,会污染检测)
        //
        // ⚠ 这条规则现在是**兜底**,不是主力:
        //   inspect 的标注图从 2026-10-20 起写进 out/ 了,不再落 images/。
        //   但老目录里可能还留着以前写在这儿的 w6_inspect_*.png ——
        //   那些图带着叠加层,当板子扫会得出莫名其妙的结论。
        //   留着这条,老目录扫出来的结果还是对的。
        if (name.rfind("w6_inspect_", 0) == 0) {
            res.skipped.push_back({rel, "inspect 生成的叠加标注图,不是待检样本"});
            ++res.skipCount;
            continue;
        }

        BoardResult br;
        std::string err;
        if (!runBoard(e.path(), recipeDir, br, err)) {
            res.skipped.push_back({rel, err});
            ++res.skipCount;
            continue;
        }

        cv::Mat img = loadImage(e.path());
        const BoardCodes codes = readBoardCodes(img, doSilkscreen, tesseractPath);

        // ⚠ 没读成的**读取**必须报出来。
        //
        //   以前这里是 `silk = readSilkscreen(img, silkErr);` ——
        //   失败原因算出来了,却直接丢掉。报告里 silk 列空着,
        //   和"板上没丝印"长得一模一样。
        //
        //   开关关着不算失败(那是有意不读),所以只报真失败。
        if (doSilkscreen && !codes.silkRead) {
            res.readFailures.push_back({rel, "silkscreen", codes.silkError});
        }

        const std::vector<ReportRow> rows =
            rowsFromBoard(rel, br, codes.silk, codes.qr);
        res.rows.insert(res.rows.end(), rows.begin(), rows.end());

        br.pass ? ++res.passCount : ++res.failCount;
        ++res.boardCount;
    }
    return res;
}

// ============================================================
// CSV
// ============================================================
std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';   // 双写 = 转义
        out += c;
    }
    out += '"';
    return out;
}

std::string csvHeader() {
    return "board_file,board_type,component,kind,defect,description,silk,qr,result\n";
}

bool writeCSVReport(const fs::path& outPath, const std::vector<ReportRow>& rows,
                    bool writeHeader) {
    // ofstream 收 fs::path 时走宽字符 API —— 中文路径也写得出去。
    std::ofstream f(outPath, writeHeader ? std::ios::out
                                          : (std::ios::out | std::ios::app));
    if (!f.is_open()) return false;
    if (writeHeader) f << csvHeader();
    for (const auto& r : rows) {
        f << csvEscape(r.boardFile) << "," << csvEscape(r.boardType) << ","
          << csvEscape(r.component) << "," << csvEscape(r.kind) << ","
          << csvEscape(r.defect) << "," << csvEscape(r.description) << ","
          << csvEscape(r.silk) << "," << csvEscape(r.qr) << ","
          << r.result << "\n";
    }
    return true;
}

} // namespace w6
