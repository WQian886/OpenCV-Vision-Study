#pragma once
// ============================================================
// W6 的可测逻辑层
//
// ## 为什么要有这一层
//
// 原来这些逻辑全在 main.cpp 里 —— main.cpp 是**程序入口**,
// 没法被别的目标链接,于是 W6 一条自动化测试都没有。
//
// 这个仓库里已经有同样的先例:W7 的 `step_scope.h` 就是为了
// 让"单步执行跑哪些节点"这条逻辑能被测,才从 mainwindow.cpp 里
// 抽出来的(见那个头文件的说明)。W6 这层是同一个道理。
//
// 拆分的界线:
//   · 这一层(w6_cli):**纯逻辑** —— 查板型、找配方、跑一块板、
//     跑一批板、拼 CSV 行。给输入、出结果,不打印。
//   · main.cpp:**入口** —— 解析参数、把结果打印成人话。
//
// 拆开之后,本文件里的每一条判断都有测试,
// 而"少检了一块板却没人知道"这类静默失效**有了断言**。
// ============================================================

#include <opencv2/opencv.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "pcb_core.h"

namespace w6 {

namespace fs = std::filesystem;

// ============================================================
// 路径 <-> 文本
//
// ⚠ 为什么需要这一对:
//
//   C++17 的 `path::string()` 在 Windows 上按 **ANSI 代码页**转换,
//   路径里有非 ANSI 字符时**直接抛异常**
//   (实测:"根本没有这张图.png" -> "No mapping for the Unicode character
//   exists in the target multi-byte code page")。
//   同一个坑还有第二个:窄串构造路径时(`p / "中文"`),
//   MSVC 同样按 ANSI 解释那段窄串 —— 而源文件是 UTF-8,
//   于是**路径构造本身就抛**。
//
//   这一层统一走 UTF-8,理由不只是"别崩":
//   仓库里的 manifest.csv 是 UTF-8,文件名要和表里的内容
//   **按同一套编码**比,否则比不出来 ——
//   比不出来 = 查不到板型 = 这块板被**静默跳过**。
//   在报告里,被跳过的板和合格的板长得一模一样。
// ============================================================
std::string pathText(const fs::path& p);

// 读图。路径含非 ASCII 字符时也能读。
//
// ⚠ 不用 cv::imread:它内部按 ANSI 打开文件,
//   中文路径读不出来,而且**不报错**,只给你一个空 Mat。
//   这里自己按宽路径把字节读进来,再 imdecode。
cv::Mat loadImage(const fs::path& p);

// ============================================================
// 板型查询
//
// 在图片**所在目录**里找真值表(*manifest.csv),查它属于哪块板。
//
// ⚠ 不靠文件名猜板型:"mcu_baseline.png" 去掉后缀是 mcu_baseline,
//   按 "_" 切第一段才是 mcu —— 而真值表里直接写着 kind,
//   比任何切法都可靠。
//
// 找不到返回空串 —— 调用方**必须**把它当成"没法判"并报出来,
// 不能默认当成某块板。
// ============================================================
std::string boardKindOf(const fs::path& imgPath);

// ============================================================
// 板型配方目录
//
// 配方 JSON 只有一份(在 W7_VisionMasterLite/recipes/ 里),
// W6 不复制一份 —— 复制就是抄副本,改一边忘一边就漂了。
// 这里按顺序试几个位置,第一个有 demo.json 的就是它。
// ============================================================
std::string findRecipeDir(const std::string& overrideDir);

// ============================================================
// 一块板的判定结果
// ============================================================
struct BoardResult {
    std::string recipeName;
    std::string boardTitle;
    std::vector<pcbcore::InspectSite> sites;
    std::vector<pcbcore::Defect>  defects;
    std::vector<pcbcore::Metric>  metrics;
    std::vector<pcbcore::Skipped> skipped;
    cv::Mat marked;
    int componentCount = 0;
    int withRules = 0;
    bool pass = false;
};

// 跑一块板。失败时 errOut 写**人话**,说清为什么不能判。
//
// ⚠ 失败 ≠ 合格。调用方必须把"跳过了"和"PASS"分开报 ——
//   产线上少检一块板却报成合格,是最坏的一种结果。
bool runBoard(const fs::path& imgPath, const std::string& recipeDir,
              BoardResult& out, std::string& errOut);

// ============================================================
// 一批板
// ============================================================
struct Skip {
    std::string file;     // 相对扫描根的路径
    std::string reason;   // 人话
};

// CSV 里的一行 = 一个元件。
struct ReportRow {
    std::string boardFile;   // 相对扫描根的路径
    std::string boardType;   // 配方名(power/mcu/signal/demo)
    std::string component;   // 位号;全局缺陷(短路)留空
    std::string kind;        // 封装类别
    std::string defect;      // "ok" 或 "wrong|bridge"(一个元件可能多种缺陷)
    std::string description; // 缺陷说明
    std::string silk;        // 丝印 OCR
    std::string qr;          // QR 串号
    std::string result;      // 整块板的结论(PASS/FAIL)
};

struct ReportResult {
    std::vector<ReportRow> rows;
    std::vector<Skip>      skipped;
    int passCount = 0;
    int failCount = 0;
    int skipCount = 0;
    int boardCount = 0;     // 真正判过的板数
};

// ============================================================
// 把一块板的判定结果摊成 CSV 行(一个元件一行)
//
// ⚠ 为什么这个函数是**共用**的:
//   baseline 模式(单板)和 report 模式(批量)都要出同样的行,
//   之前这两处各写了一份归拢逻辑 —— 改一处忘另一处,
//   两份报告的列就会悄悄对不上(而 CSV 读起来还是合法的)。
//   这就是本项目栽过四遍的"抄副本",不给自己再栽一次。
// ============================================================
std::vector<ReportRow> rowsFromBoard(const std::string& relPath,
                                     const BoardResult& br,
                                     const std::string& silk,
                                     const std::string& qr);

// 扫目录(递归)跑一批板。
//
// readSilkscreen 为 false 时跳过 OCR —— 丝印 OCR 要起 tesseract 进程,
// 慢,而且测试用不着。
ReportResult runReport(const fs::path& dir, const std::string& recipeDir,
                       bool readSilkscreen);

// ============================================================
// CSV
// ============================================================

// 字段转义:含逗号/引号/换行时用双引号包起来,引号本身双写。
//
// ⚠ 不转义的后果是**静默**的列错位:一列变两列,后面用 pandas 读
//   才发现不对,而不转义时程序不报错。
std::string csvEscape(const std::string& s);

std::string csvHeader();

// 写报告。writeHeader 只在第一份报告时为 true。
bool writeCSVReport(const std::string& outPath, const std::vector<ReportRow>& rows,
                    bool writeHeader);

// ============================================================
// W6d: QR 串号 / 丝印 OCR
// ============================================================
std::string readQRCode(const cv::Mat& img);

// 读丝印。失败时 err 写**人话**。
//
// ⚠ "读出来是空的"和"根本没读"是两件事 —— 报告里 silk 列空着,
//   看的人分不清是板上没丝印还是 OCR 没跑。
std::string readSilkscreen(const cv::Mat& img, std::string& err);

} // namespace w6
