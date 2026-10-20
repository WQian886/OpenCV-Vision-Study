// ============================================================
// W6 的可测逻辑层 —— 实现
//
// 见 w6_cli.h 顶部的说明:这一层是纯逻辑,给输入出结果,不打印。
// 打印是 main.cpp 的事。
// ============================================================

#include "w6_cli.h"

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
// 路径 <-> 文本(UTF-8)
// ============================================================
std::string pathText(const fs::path& p) {
    // generic_u8string():UTF-8 + 正斜杠。
    // 用 generic_ 而不是 u8string_ 是因为路径要写进 CSV,
    // 正斜杠跨平台一致,反斜杠在 CSV 里还得再转义一遍。
    //
    // u8string() 在 C++20 返回 std::u8string,这里统一转回 std::string,
    // 免得哪天升了 C++ 标准就编不过。
    try {
#if __cplusplus >= 202002L
        const std::u8string s = p.generic_u8string();
        return std::string(s.begin(), s.end());
#else
        return p.generic_u8string();
#endif
    } catch (const std::exception&) {
        // 路径里有连 UTF-8 都表示不了的字符(非法代理对之类)。
        // 退一步用窄串,至少别崩。
        return p.generic_string();
    }
}

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
//   读出来就是 UTF-8 字节,正好和 pathText() 的输出同一套编码。
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
    const std::string fname = pathText(imgPath.filename());
    if (!fs::exists(dir)) return "";
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".csv") continue;
        const std::string name = pathText(e.path().filename());
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

std::string findRecipeDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        // ⚠ 指定的目录不对就**返回空**,别原样返回一个坏路径。
        //   原样返回的话,每块板各报一次"配方加载失败",刷屏,
        //   而且真正的原因(目录写错了)被埋在最后一条错误里。
        return looksLikeRecipeDir(fs::path(overrideDir)) ? overrideDir : "";
    }
    for (const char* d : kRecipeDirs) {
        if (looksLikeRecipeDir(fs::path(d))) return d;
    }
    return "";
}

// ============================================================
// W6d: 丝印 OCR(复用 W5 的 tesseract 调用)
//
// 用 CreateProcessW 而不是 system():tesseract 的路径里有空格,
// system() 会把它切成两半。
//
// ⚠ tesseract 没装 / 跑不起来时**明说**,不假装读到了空串。
// ============================================================
std::string readSilkscreen(const cv::Mat& img, std::string& err) {
#ifdef _WIN32
    // 1. 预处理:灰度 + **放大** + 自适应阈值
    //
    // 放大是必须的:板上的丝印是 14~18px 的字(FONT_S / FONT_M),
    // 直接扔给 tesseract 基本读不出来 —— 之前整板 OCR 出来是一堆乱码。
    // 放大 3 倍之后字高到 42~54px,才进得了 tesseract 的射程。
    const double kScale = 3.0;
    cv::Mat gray, big, thresh;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, big, cv::Size(), kScale, kScale, cv::INTER_CUBIC);
    cv::adaptiveThreshold(big, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY, 31, 10);

    // 预处理图和 OCR 结果放 out/,**不能放 images/** ——
    // images/ 是批量报告的输入目录,往里丢中间产物,下一次 report
    // 就会把自己的中间产物当成待检板扫一遍(而且它没有 manifest 登记,
    // 会被报成"跳过",看着像少检了一块板)。
    const std::string prePath  = "out/silk/w6_silk_preprocessed.png";
    const std::string outBase  = "out/silk/w6_silk_result";
    const std::string outTxt   = outBase + ".txt";
    fs::create_directories("out/silk");
    if (!cv::imwrite(prePath, thresh)) {
        err = "预处理图写不出去: " + prePath;
        return "";
    }

    // 2. 调 tesseract
    const std::wstring tesseract = L"D:\\Program Files\\Tesseract-OCR\\tesseract.exe";
    // 宽字符转窄字符:Windows 那边是 wchar_t,这里要的是 std::string。
    // 直接用迭代器构造会逐元素窄化(wchar_t -> char),在中文路径上会毁掉。
    auto wideToUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                          static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
    };
    const std::string tesseractPath = wideToUtf8(tesseract);
    if (!fs::exists(fs::path(tesseractPath))) {
        err = "没找到 tesseract(" + tesseractPath +
              ") —— 装 Tesseract-OCR 才能读丝印";
        return "";
    }
    auto w = [&wideToUtf8](const std::string& s) {
        // 窄字符 -> 宽字符。同样逐元素扩宽在非 ASCII 上会毁掉,
        // 路径里可能带中文,所以走 WinAPI。
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                          static_cast<int>(s.size()),
                                          nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                            out.data(), n);
        return out;
    };
    // --psm 11 = "sparse text"(找散落的文字),不是 6("uniform block")。
    // 板上的丝印是**散落的**(板号在一角、SN 标在二维码下面、
    // 每个元件上面还有位号),按整块文本排版去解释会把版面分析带偏。
    const std::wstring cmdLine = L"\"" + tesseract + L"\" \"" + w(prePath) +
                                 L"\" \"" + w(outBase) + L"\" -l eng --psm 11";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        err = "CreateProcessW 失败,错误码 " + std::to_string(GetLastError());
        return "";
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ret = 1;
    GetExitCodeProcess(pi.hProcess, &ret);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (ret != 0) {
        err = "tesseract 返回码 " + std::to_string(ret);
        return "";
    }

    // 3. 读结果
    std::ifstream f(outTxt);
    if (!f) {
        err = "tesseract 跑完了但没产出 " + outTxt;
        return "";
    }
    std::string all, line;
    while (std::getline(f, line)) {
        if (!all.empty()) all += " ";
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        all += line;
    }
    f.close();
    return all;
#else
    err = "丝印 OCR 目前只有 Windows 实现(用 CreateProcessW 调 tesseract)";
    return "";
#endif
}

std::string readQRCode(const cv::Mat& img) {
    cv::QRCodeDetector det;
    std::vector<cv::Point2f> pts;
    return det.detectAndDecode(img, pts);
}

// ============================================================
// 跑一块板
// ============================================================
bool runBoard(const fs::path& imgPath, const std::string& recipeDir,
              BoardResult& out, std::string& errOut) {
    // --- 板型 ---
    const std::string kind = boardKindOf(imgPath);
    if (kind.empty()) {
        errOut = "找不到这块板属于哪种板型(同目录里没有登记它的 manifest.csv)"
                 "—— 拿不到配方就没法定位元件,判定会给出错误结论";
        return false;
    }
    const fs::path recipePath = fs::path(recipeDir) / (kind + ".json");
    pcbcore::BoardRecipe recipe;
    std::string reason;
    if (!pcbcore::loadRecipeFile(pathText(recipePath), &recipe, &reason)) {
        errOut = "板型配方加载失败:" + reason;
        return false;
    }
    out.recipeName = recipe.name;
    out.boardTitle = recipe.title;

    // --- 读图 ---
    cv::Mat img = loadImage(imgPath);
    if (img.empty()) {
        errOut = "读不出图片: " + pathText(imgPath);
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
ReportResult runReport(const fs::path& dir, const std::string& recipeDir,
                       bool doSilkscreen) {
    ReportResult res;

    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".png") continue;

        // 相对 dir 的路径。用相对路径而不是文件名,是因为递归之后
        // filenames 不再唯一(complex/mcu_wrong.png 和顶层的重名也能撞)。
        const std::string rel = pathText(fs::relative(e.path(), dir));
        const std::string name = pathText(e.path().filename());

        // 过滤掉 inspect 生成的 marked 图(带叠加层,会污染检测)
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
        std::string qr, silk, silkErr;
        if (!img.empty()) {
            qr = readQRCode(img);
            if (doSilkscreen) silk = readSilkscreen(img, silkErr);
        }

        const std::vector<ReportRow> rows = rowsFromBoard(rel, br, silk, qr);
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

bool writeCSVReport(const std::string& outPath, const std::vector<ReportRow>& rows,
                    bool writeHeader) {
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
