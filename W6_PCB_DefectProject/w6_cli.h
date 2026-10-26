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
// 路径 <-> UTF-8 文本的**唯一一处定义** + 窄串守卫
// (守卫宏 UTF8PATH_NO_NARROW_ARG 在这里)。
#include "utf8_path.h"

namespace w6 {

namespace fs = std::filesystem;

// ============================================================
// 路径 <-> 文本
//
// 这一层的路径**一律是 std::filesystem::path**,不再有"这个 std::string
// 是一条路径"的情况。
//
// 理由(2026-10-26,第一次在真 CI runner 上跑出来的):
//
//   (1) C++17 的 `path::string()` 在 Windows 上按 **ANSI 代码页**窄化,
//       而且是**严格**的:字符在 ACP 里表示不出来就**直接抛异常**
//       (实测:"根本没有这个目录" -> "No mapping for the Unicode character
//       exists in the target multi-byte code page")。
//       本机 ACP=936(GBK)**有**中文 -> 转得出 -> 绿;
//       GitHub runner 的 ACP=1252 **没有**中文 -> **抛**。
//       **同一份代码,一台绿一台红**,差别不在代码,在**这台机器的 ACP**。
//
//   (2) 窄串构造路径时(`p / "中文"`),MSVC 同样按 ANSI 解释那段窄串。
//       而源文件是 UTF-8 —— 于是路径从一开始就是**另一条路径**。
//       ⚠ 这一条**不抛**:936 上 24 字节 UTF-8 变成 12 个汉字,
//         1252 上变成 24 个 Latin-1 字符 —— **两台机器都错**,只是错得不一样。
//         (别把它当成"本机绿、runner 红"的那种坑 —— 那是 (1)。)
//
//   窄串承载路径还得先挑编码,而"挑哪个"取决于它从哪来
//   (命令行 argv 是 ACP,UTF-8 文件读出来的是 UTF-8)——
//   两种窄串同一个类型,迟早挑错一次。
//   路径用 fs::path 就没这个问题:它内部是宽字符,不过任何代码页。
//
//   要把路径变成**文本**(写进 CSV、跟 manifest.csv 比文件名),
//   走 utf8path::text() —— 全仓库唯一的一处定义,
//   约定见 common/utf8_path/utf8_path.h。
//
//   这一层统一走 UTF-8,理由不只是"别崩":
//   仓库里的 manifest.csv 是 UTF-8,文件名要和表里的内容
//   **按同一套编码**比,否则比不出来 ——
//   比不出来 = 查不到板型 = 这块板被**静默跳过**。
//   在报告里,被跳过的板和合格的板长得一模一样。
// ============================================================

// 读图。路径含非 ASCII 字符时也能读。
//
// ⚠ 不用 cv::imread:它内部按 ANSI 打开文件,
//   中文路径读不出来,而且**不报错**,只给你一个空 Mat。
//   这里自己按宽路径把字节读进来,再 imdecode。
cv::Mat loadImage(const fs::path& p);

// 窄串守卫:传 std::string / "字面串" 进来**编译不过**
// (理由见 common/utf8_path/utf8_path.h —— fs::path 的窄串构造走 ACP,
//  那条路**每台机器都走错**,而且一声不吭。)
template <class S, std::enable_if_t<!utf8path::IsPathArg<S>::value, int> = 0>
cv::Mat loadImage(const S& p) {
    static_assert(utf8path::IsPathArg<S>::value, UTF8PATH_ONLY_PATH_MSG);
    return cv::Mat();
}

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
template <class S, std::enable_if_t<!utf8path::IsPathArg<S>::value, int> = 0>
std::string boardKindOf(const S& imgPath) {
    static_assert(utf8path::IsPathArg<S>::value, UTF8PATH_ONLY_PATH_MSG);
    return std::string();
}

// ============================================================
// 板型配方目录
//
// 配方 JSON 只有一份(在 W7_VisionMasterLite/recipes/ 里),
// W6 不复制一份 —— 复制就是抄副本,改一边忘一边就漂了。
// 这里按顺序试几个位置,第一个有 demo.json 的就是它。
//
// ⚠ 返回 / 收的都是 **fs::path**:目录路径不是"文本",
//   拿窄串承载就得先挑编码(见 common/utf8_path/utf8_path.h)。
//   找不到返回**空路径**。
// ============================================================
fs::path findRecipeDir(const fs::path& overrideDir);
template <class S, std::enable_if_t<!utf8path::IsPathArg<S>::value, int> = 0>
fs::path findRecipeDir(const S& overrideDir) {
    static_assert(utf8path::IsPathArg<S>::value, UTF8PATH_ONLY_PATH_MSG);
    return fs::path();
}

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
bool runBoard(const fs::path& imgPath, const fs::path& recipeDir,
              BoardResult& out, std::string& errOut);
// 守卫:两个路径参数都必须是 fs::path。
template <class S1, class S2,
          std::enable_if_t<!utf8path::IsPathArg<S1>::value ||
                           !utf8path::IsPathArg<S2>::value, int> = 0>
bool runBoard(const S1& imgPath, const S2& recipeDir,
              BoardResult& out, std::string& errOut) {
    static_assert(utf8path::IsPathArg<S1>::value &&
                  utf8path::IsPathArg<S2>::value, UTF8PATH_ONLY_PATH_MSG);
    return false;
}

// ============================================================
// 一批板
// ============================================================
struct Skip {
    std::string file;     // 相对扫描根的路径
    std::string reason;   // 人话
};

// 一次没读成的读取。
//
// what 写**读的是什么**(silkscreen / qr),reason 写人话。
struct ReadFailure {
    std::string file;
    std::string what;
    std::string reason;
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
    // 没读成的**读取**(比如丝印 OCR 没跑起来)。
    //
    // ⚠ 这跟 skipped 是**两件不同的事**,不能混:
    //   skipped = 这块板**没判**,结论不成立;
    //   读失败 = 板判了,但丝印那一栏是空的。
    //
    //   混在一起的后果:看报告的人以为"少检了一块板",
    //   去查板型/配方 —— 而真正的问题是 tesseract 没装。
    //
    //   更糟的是以前这里干脆**什么都不报**:
    //   OCR 的失败原因算出来了,却直接丢掉(silkErr 收了就没再用)。
    //   报告里 silk 列空着,和"板上没丝印"长得一模一样。
    std::vector<ReadFailure> readFailures;
    int passCount = 0;
    int failCount = 0;
    int skipCount = 0;
    int boardCount = 0;     // 真正判过的板数
};

// ============================================================
// 一块板的 QR 串号 + 丝印文字
//
// "读出来是空的"和"根本没读"必须分得开:
//   silkRead = false -> **没读**(开关关着,或者读失败)
//   silkRead = true, silk 为空 -> 读了,图上确实没字
//
// ⚠ 这两个在 CSV 里长得一模一样(都是空字符串),
//   所以失败原因必须**另外报出去**,不能指望从 silk 列看出来。
// ============================================================
struct BoardCodes {
    std::string qr;         // QR 内容
    bool qrFound = false;   // 图上到底有没有 QR
    std::string silk;       // 丝印文字
    bool silkRead = false;  // 有没有读
    std::string silkError;  // 没读成的原因(人话);读成了就是空
};

// 读一块板的码。
//
// doSilkscreen = false 时**不读**丝印(要起 tesseract 进程,慢,测试用不着),
// 此时 silkRead = false —— 不是"读了但是空"。
//
// tesseractPath 空 = 自动找。传一个不存在的路径可以确定性地
// 触发"没读成"那条路径(给测试用)。
BoardCodes readBoardCodes(const cv::Mat& img, bool doSilkscreen,
                          const fs::path& tesseractPath = fs::path());
// 守卫:第三个参数(路径)必须是 fs::path。
// 前两个参数(img / doSilkscreen)照抄 —— 守卫只管**路径参数**。
template <class S, std::enable_if_t<!utf8path::IsPathArg<S>::value, int> = 0>
BoardCodes readBoardCodes(const cv::Mat& img, bool doSilkscreen,
                          const S& tesseractPath) {
    static_assert(utf8path::IsPathArg<S>::value, UTF8PATH_ONLY_PATH_MSG);
    return BoardCodes();
}

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
//
// ⚠ tesseractPath 只给**测试**用:传一个不存在的路径,
//   就能确定性地触发"OCR 没跑成"这条路径,不用真的卸掉 tesseract。
//   空 = 自动找(默认安装位置 / PATH)。
ReportResult runReport(const fs::path& dir, const fs::path& recipeDir,
                       bool readSilkscreen, const fs::path& tesseractPath = fs::path());
// 守卫:第 1、2、4 个参数(路径)必须是 fs::path。
template <class S1, class S2, class S3,
          std::enable_if_t<!utf8path::IsPathArg<S1>::value ||
                           !utf8path::IsPathArg<S2>::value ||
                           !utf8path::IsPathArg<S3>::value, int> = 0>
ReportResult runReport(const S1& dir, const S2& recipeDir,
                       bool readSilkscreen, const S3& tesseractPath) {
    static_assert(utf8path::IsPathArg<S1>::value &&
                  utf8path::IsPathArg<S2>::value &&
                  utf8path::IsPathArg<S3>::value, UTF8PATH_ONLY_PATH_MSG);
    return ReportResult();
}

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
bool writeCSVReport(const fs::path& outPath, const std::vector<ReportRow>& rows,
                    bool writeHeader);
template <class S, std::enable_if_t<!utf8path::IsPathArg<S>::value, int> = 0>
bool writeCSVReport(const S& outPath, const std::vector<ReportRow>& rows,
                    bool writeHeader) {
    static_assert(utf8path::IsPathArg<S>::value, UTF8PATH_ONLY_PATH_MSG);
    return false;
}

// ============================================================
// W6d: QR 串号 / 丝印 OCR
//
// ⚠ 2026-10-20:这两个函数**从本文件里删掉了** ——
//   QR 解码和 tesseract 调用搬到了 common/qr_ocr/,W5 / W6 / W7 共用一份。
//
//   搬之前本文件里有一份拷贝,W5 的 main.cpp 里有一份,
//   W7 的 qr_decode 算子里还有一份 —— 本项目栽过四遍的坑,这是第五次。
//
//   现在的约定:
//     qroc::readQr(img)            -> QrResult{found, text, corners}
//     qroc::readText(img, options) -> OcrResult{ok, text, error, ...}
//
//   "读出来是空的"和"根本没读"必须分得开:
//     ok=false                    -> **没读**,error 里写清楚为什么
//     ok=true 且 text 为空         -> 读了,图上确实没字
//
//   报告里 silk 列空着的时候,看的人必须分得出是"板上没丝印"
//   还是"OCR 根本没跑"。分不清就等于把没查说成查过了。
// ============================================================

} // namespace w6
