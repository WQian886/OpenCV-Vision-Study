// ============================================================
// common/qr_ocr 的无头回归测试
// ============================================================
//
// ## 为什么这份测试必须存在
//
// 这份代码是 2026-10-20 从 W5 / W6 / W7 **三处**抄副本合并出来的。
// 合并之前,那三份**一行测试都没有**:
//
//   · W5 的 main.cpp:QR 解码 + tesseract 调用,只有 printf
//   · W6 的 w6_cli.cpp:抄了 W5 的,并修了宽字符转换和 --psm,
//     但同样没被测过(要起外部进程,慢而且依赖环境)
//   · W7 的 qr_decode 算子:抄了 QR 解码
//
// 于是有两件事没人知道:
//   1. W6 的 --psm 11 和 W5 的 --psm 6 到底哪个对?
//      (它们是两份拷贝,不是一份的两个参数 —— 没有"哪个对"这个问题)
//   2. tesseract 没装 / 跑不起来的时候,程序是**明说**了还是
//      **悄悄返回空串**?空串在报告里和"板上没丝印"长得一模一样。
//
// 第 2 条正是本项目的核心关切:**静默失效优先于崩溃排查**。
// 所以这份测试钉的是"失败必须说出来",不只是"成功时读对"。
//
// ## 依赖说明
//
// OCR 那几条**需要装 tesseract**。没装的话它们会被**跳过**,
// 而且跳过的条数会**明写在 SUMMARY 里** ——
// 不静默变绿、不假装测过了。
//
// ## 期望值从哪来
//
// · QR 的内容来自 W5 自己的生成器 gen_qr.py
//   (qr.add_data("PCB-2026-001-SN:A12345X")),
//   不是在这儿抄一份 —— 抄的那份会跟生成器漂开。
// · OCR 的内容来自 gen_ocr_test.py 画的三行字
//   ("R123 100R" / "C456 10uF" / "PCB-2026-001")。
//
// ============================================================

#include "qr_ocr.h"

#include "utf8_path.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

using namespace qroc;

// ============================================================
// 极简断言(与 common/pcb_core、各 W7 套件同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;
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

// 跳过也要**报出来**。
//
// 本项目的硬规矩:跳过/未判定必须报出来。
// 一条测试因为环境缺东西而没跑,如果不写出来,
// 看报告的人会以为"这条已经验过了"—— 那比没有还坏。
static void skip(const std::string& what, const std::string& why) {
    ++g_skip;
    std::cout << "  [SKIP] " << what << " —— " << why << std::endl;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ============================================================
// 找仓库根
//
// 同一个测试文件会被链进多个目标,各目标的 ctest 工作目录不同。
//
// ⚠ 判据是 **common/ 存在**,不是"工程目录名存在"。
//   2026-10-20 上 CI 第一次跑就撞上了:CI 的构建目录是
//   build/ci/<工程名>/,build/ci/ 下面正好摆着一份
//   W5_OCR_QRCode 和 W6_PCB_DefectProject —— 循环在那一层就停了,
//   把**构建目录当成了仓库根**,于是报"找不到图"。
//   common/ 只有仓库根有,构建树里不可能有同名子目录。
// ============================================================
static fs::path findRepoRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "common" / "qr_ocr" / "qr_ocr.h") &&
            fs::exists(p / "W5_OCR_QRCode") &&
            fs::exists(p / "W6_PCB_DefectProject")) {
            return p;
        }
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return fs::current_path();
}

static fs::path g_root;

// ============================================================
// 用例 1:QR
// ============================================================
static void test_qr() {
    beginCase("QR:认得出内容,认不出时也不崩");

    // --- 图上有 QR ---
    {
        const fs::path img = g_root / "W5_OCR_QRCode" / "images" / "qrcode_test.png";
        const cv::Mat m = cv::imread(utf8path::text(img), cv::IMREAD_COLOR);
        check(!m.empty(), "读得到 " + utf8path::text(img));
        if (m.empty()) return;

        const QrResult r = readQr(m);
        check(r.found, "找到了 QR");
        // 期望值来自 W5 自己的生成器 gen_qr.py,不是在这儿抄一份
        check(r.text == "PCB-2026-001-SN:A12345X",
              "QR 内容对(实际 \"" + r.text + "\")");
        check(r.corners.size() == 4,
              "给了 4 个角点(实际 " + std::to_string(r.corners.size()) + " 个)");
    }

    // --- 空白图上没 QR ---
    //
    // 这一条钉的是"**没找到**"和"**崩溃了**"是两回事。
    // 检测器在乱七八糟的图上完全可能抛异常,那时候调用方会崩,
    // 而"这张图上没 QR"是正常业务情况。
    {
        const cv::Mat blank(200, 300, CV_8UC3, cv::Scalar(255, 255, 255));
        const QrResult r = readQr(blank);
        check(!r.found, "空白图上没有 QR");
        check(r.text.empty(), "空白图上 QR 内容为空");
        check(r.corners.empty(), "空白图上没有角点");
    }

    // --- 空 Mat ---
    {
        const cv::Mat empty;
        const QrResult r = readQr(empty);
        check(!r.found, "空图不崩,found=false");
        check(r.text.empty(), "空图上内容为空");
    }
}

// ============================================================
// 用例 2:找 tesseract
// ============================================================
static void test_find_tesseract() {
    beginCase("tesseract:找得到 / 找不到,以及**不偷换**");

    // --- 显式指定一个不存在的路径 ---
    //
    // ⚠ 这条钉的是本模块最重要的一条契约:
    //   调用方明确说了"用这个程序",我们**不能**偷偷换一个去跑。
    //   偷换 = 拿另一个程序跑出来的结果,调用方却以为是自己指定的那个。
    //   静默换人比找不到更难查。
    {
        const fs::path bogus = utf8path::fromUtf8("D:/绝对不存在的路径/tesseract.exe");
        check(findTesseract(bogus).empty(),
              "显式指定的不存在路径 -> 返回空串,而不是悄悄换成默认的那个");
    }

    // --- 显式指定一个真实存在的路径 ---
    {
        const fs::path real = findTesseract();
        if (real.empty()) {
            skip("显式指定真实路径时原样返回",
                 "本机没装 tesseract,没有可指定的真实路径");
        } else {
            check(findTesseract(real) == real,
                  "显式指定真实路径时原样返回(\"" + utf8path::text(real) + "\")");
        }
    }

    // --- 自动找 ---
    {
        const fs::path found = findTesseract();
        if (found.empty()) {
            skip("自动找 tesseract", "本机没装 tesseract(这本身是允许的)");
            std::cout << "         装的路径是 " << utf8path::text(defaultTesseractPath()) << std::endl;
        } else {
            check(fs::exists(found), "自动找到的路径真的存在(\"" + utf8path::text(found) + "\")");
        }
    }
}

// ============================================================
// 用例 3:OCR
// ============================================================
static void test_ocr() {
    beginCase("OCR:读得出文字,读不出时说清楚为什么");

    const fs::path img = g_root / "W5_OCR_QRCode" / "images" / "ocr_test.png";

    // --- 失败路径(不依赖 tesseract 装没装,永远要跑)---
    {
        // 空图
        const cv::Mat empty;
        const OcrResult r = readText(empty);
        check(!r.ok, "空图 -> ok=false");
        check(!r.error.empty(), "空图 -> error 写清楚了为什么");
    }

    // --- 显式指定一个不存在的 tesseract ---
    {
        const fs::path bogus = utf8path::fromUtf8("D:/绝对不存在的路径/tesseract.exe");
        OcrOptions opt;
        opt.tesseractPath = bogus;
        opt.workDir = fs::temp_directory_path() / "qroc_test_bogus";

        const cv::Mat m = cv::imread(utf8path::text(img), cv::IMREAD_COLOR);
        if (!m.empty()) {
            const OcrResult r = readText(m, opt);
            check(!r.ok, "指定的 tesseract 不存在 -> ok=false(不是悄悄换一个)");
            check(r.text.empty(), "指定的 tesseract 不存在 -> 没有返回任何文字");
            check(contains(r.error, utf8path::text(bogus)),
                  "错误消息里写明了**用的是哪个路径**(实际 \"" + r.error + "\")");
            check(contains(r.error, "不会悄悄换"),
                  "错误消息明说不会悄悄换一个(实际 \"" + r.error + "\")");
        }
    }

    // --- 正常路径(需要 tesseract)---
    {
        if (findTesseract().empty()) {
            skip("读出文字 / 中间产物落盘 / 报错为空",
                 "本机没装 tesseract,跑不了真实 OCR");
            return;
        }

        const cv::Mat m = cv::imread(utf8path::text(img), cv::IMREAD_COLOR);
        check(!m.empty(), "读得到 " + utf8path::text(img));
        if (m.empty()) return;

        const fs::path work = fs::temp_directory_path() / "qroc_test_work";
        std::error_code ec;
        fs::remove_all(work, ec);

        OcrOptions opt;
        opt.workDir = work;
        opt.stem = "silk_test";
        opt.psm = 11;

        const OcrResult r = readText(m, opt);
        check(r.ok, "OCR 成功(实际失败原因:\"" + r.error + "\")");
        check(r.error.empty(), "成功时 error 为空(实际 \"" + r.error + "\")");

        // 期望的三行字来自 gen_ocr_test.py
        check(contains(r.text, "R123"),   "读到 R123(实际 \"" + r.text + "\")");
        check(contains(r.text, "C456"),   "读到 C456(实际 \"" + r.text + "\")");
        check(contains(r.text, "PCB-2026-001"),
              "读到 PCB-2026-001(实际 \"" + r.text + "\")");

        // 中间产物落在**指定的**目录里
        check(fs::exists(work / "silk_test.png"),
              "预处理图落在 workDir(" + utf8path::text(work / "silk_test.png") + ")");
        check(fs::exists(work / "silk_test.txt"),
              "tesseract 的输出落在 workDir(" + utf8path::text(work / "silk_test.txt") + ")");

        // 教学展示用的字段
        check(!r.preprocessed.empty(), "返回了预处理图");
        check(contains(r.commandLine, "tesseract"),
              "返回了实际调用的命令行(实际 \"" + r.commandLine + "\")");
        check(contains(r.commandLine, "--psm 11"),
              "命令行里带着 --psm 11(实际 \"" + r.commandLine + "\")");
        check(!r.usedTesseract.empty(), "记录了实际用的 tesseract 路径");

        // --- psm 是**参数**,不是两份拷贝 ---
        //
        // W5 用 --psm 6,W6 用 --psm 11,以前是两份不同的拷贝。
        // 现在是同一份实现的两个参数 —— 所以两个都必须能读出东西。
        {
            OcrOptions o6 = opt;
            o6.stem = "silk_test_psm6";
            o6.psm = 6;
            const OcrResult r6 = readText(m, o6);
            check(r6.ok, "psm=6 也跑得通");
            check(contains(r6.text, "PCB-2026-001"),
                  "psm=6 也能读到 PCB-2026-001(实际 \"" + r6.text + "\")");
        }
    }
}

// ============================================================
// 用例 4:中间产物不许落在待检图目录里
// ============================================================
static void test_workdir_isolation() {
    beginCase("中间产物不许落进待检图目录");

    // 这一条钉的是 W6 自己踩过的坑:
    // 预处理图和 OCR 结果曾经放在 images/ 里 ——
    // 而 images/ 是批量报告的**输入目录**。
    // 下一次扫描会把自己的中间产物当成待检板扫一遍
    // (而且它没有 manifest 登记,会被报成"跳过",
    //   看着像少检了一块板)。
    if (findTesseract().empty()) {
        skip("默认 workDir 落在系统临时目录", "本机没装 tesseract");
        return;
    }

    const fs::path img = g_root / "W5_OCR_QRCode" / "images" / "ocr_test.png";
    const cv::Mat m = cv::imread(utf8path::text(img), cv::IMREAD_COLOR);
    if (m.empty()) return;

    // 故意把 stem 取得很特别,万一落在 CWD 里就能认出来
    //
    // ⚠ 先清掉**上一轮跑剩的**同名文件。
    //   不清的话:某一次跑(比如变异验证时 workDir 故意写错)会在 CWD 里
    //   留下这两个文件,之后**每一轮**都红 ——
    //   红的原因还和当次改动毫无关系。
    //   这就是"测试自己制造出来的失败",比 flaky 还难查:
    //   flaky 至少每次随机,这个是**一次坏掉、永久变红**。
    const std::string stemName = "qroc_should_never_land_here";
    std::error_code ec;
    fs::remove(utf8path::fromUtf8(stemName + ".png"), ec);
    fs::remove(utf8path::fromUtf8(stemName + ".txt"), ec);

    OcrOptions opt;
    opt.stem = stemName;
    // workDir 留空 -> 默认系统临时目录
    const OcrResult r = readText(m, opt);
    check(r.ok, "默认 workDir 下 OCR 成功");

    check(!fs::exists(stemName + ".png"),
          "中间产物没有落在当前目录");
    check(!fs::exists(stemName + ".txt"),
          "中间产物没有落在当前目录");

    const fs::path tmp = fs::temp_directory_path() / "qroc_ocr";
    check(fs::exists(tmp / (stemName + ".png")),
          "中间产物落在系统临时目录(" + utf8path::text(tmp) + ")");
}

int main() {
    g_root = findRepoRoot();

    std::cout << "=== common/qr_ocr 回归测试 ===" << std::endl;
    std::cout << "仓库根: " << utf8path::text(g_root) << std::endl;

    test_qr();
    test_find_tesseract();
    test_ocr();
    test_workdir_isolation();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << " skip=" << g_skip
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    if (g_skip > 0) {
        std::cout << "注意:有 " << g_skip
                  << " 条因为环境缺 tesseract 被跳过 —— "
                     "这些**没验过**,不是验过了" << std::endl;
    }
    return g_fail == 0 ? 0 : 1;
}
