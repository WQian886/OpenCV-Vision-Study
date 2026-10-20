#pragma once
// ============================================================
// qr_ocr —— QR 识别 + 文字 OCR 的共享核心(无 Qt 依赖)
//
// === 为什么要共用 ===
//
// 这是本项目**第五次**抄同一个副本。前四次栽在哪:
//   1. W5 的 main.cpp 写了一份 QR 解码 + 一份 tesseract 调用
//   2. W6 的 w6_cli.cpp 又抄了一份(W5 的版本更粗糙:
//      --psm 6 是"整块文字",板丝印是散落的,读出来是乱码;
//      宽字符转换用逐元素窄化,路径带中文就毁)
//   3. W7 的 qr_decode 算子再抄了一份 QR 解码
//   4. W7 的 operators/ 里还有一份颜色判据的副本
//      —— 那次已经抽成了 common/pcb_core/
//
// 抄出来的后果不是"多几行代码":
//   · W6 的 --psm 11 和 W5 的 --psm 6 是**两个不同的答案**,
//     谁也说不清哪个对 —— 因为它们是两份拷贝,不是一份的两个参数。
//   · W6 那份修了宽字符转换的坑,W5 那份还在逐元素窄化。
//     修一处,另一处照旧坏着,而且没人会怀疑它们本来应该一样。
//
// 所以这里的规则和 common/pcb_core/ 一样:
//   **同一份定义只能有一处。** 三边都调这里的函数,
//   改一处三边跟着动。
//
// === 这一层刻意不依赖 Qt ===
// W5 和 W6 都不引 Qt。引了就是给学习项目加依赖。
//
// === 失败必须说得出来 ===
//
// 本项目的硬规矩:**静默失效优先于崩溃排查**。
// 所以本模块的返回值里,"没读到"和"读到了但是空的"必须能分开:
//   · QrResult::found = false  ->  这张图上没找到 QR
//   · OcrResult::ok    = false  ->  **没读**(tesseract 没装 / 跑不起来)
//   · OcrResult::ok    = true, text 为空 -> 读了,图上确实没字
//
// 三者在报告里长得一模一样,就会变成"废板判合格"。
// ============================================================

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace qroc {

// ============================================================
// QR
// ============================================================
struct QrResult {
    bool found = false;                    // 图上有没有找到并解出 QR
    std::string text;                      // 解出来的内容
    std::vector<cv::Point2f> corners;      // 四个角点(检测到才有)
};

// 读 QR。图读不出来 / 没有 QR 都**不会崩**,found=false。
QrResult readQr(const cv::Mat& img);

// ============================================================
// OCR
// ============================================================
struct OcrOptions {
    // 放大倍数。板上的丝印是 14~18px 的字,直接扔给 tesseract 基本读不出来 ——
    // 放大 3 倍之后字高到 42~54px,才进得了 tesseract 的射程。
    double scale = 3.0;

    // tesseract 的页面分割模式。
    //   11 = "sparse text"(找散落的文字)—— **板丝印是这个**:
    //        板号在一角、SN 标在二维码下面、每个元件上面还有位号,
    //        按整块文本排版去解释会把版面分析带偏。
    //   6  = "uniform block"(整块文字)—— 段落式文档用这个。
    int psm = 11;

    std::string lang = "eng";

    // tesseract 可执行文件路径。空 = 自动找(先找显式路径,
    // 再找默认安装位置,再找 PATH)。
    std::string tesseractPath;

    // 中间产物目录(预处理图 + tesseract 的输出 txt 放这儿)。
    // 空 = 系统临时目录。
    //
    // ⚠ **不能放在待检图目录里** —— 那目录是批量报告的输入,
    //   往里丢中间产物,下一次扫描会把自己的中间产物当成待检板
    //   扫一遍(而且它没有 manifest 登记,会被报成"跳过",
    //   看着像少检了一块板)。
    std::string workDir;

    // 中间文件名前缀(同一目录下多次调用不会互相覆盖)。
    std::string stem = "ocr";
};

struct OcrResult {
    bool ok = false;                 // false = **没读**,error 里有原因
    std::string text;                // 读到的文字(ok=true 时有效)
    std::string error;               // 人话;ok=true 时为空
    cv::Mat preprocessed;            // 预处理后的二值图(教学展示用)
    std::string commandLine;         // 实际调 tesseract 的命令行(教学展示用)
    std::string usedTesseract;       // 实际用的 tesseract 路径
};

// 读文字。
//
// 失败时 ok=false 且 error 写**人话**(哪一步失败、为什么)。
// 成功但图上没字时 ok=true 且 text 为空 —— 这两件事必须分得开。
OcrResult readText(const cv::Mat& img, const OcrOptions& opt = OcrOptions());

// 找 tesseract。找到返回路径,找不到返回**空串**。
//
// ⚠ 找不到就返回空串,不要返回一个"看起来能用"的假路径 ——
//   那样调用方会拿着假路径去跑,报错信息会指向一个不存在的文件。
std::string findTesseract(const std::string& explicitPath = std::string());

// 默认安装位置(仅 Windows)。
std::string defaultTesseractPath();

} // namespace qroc
