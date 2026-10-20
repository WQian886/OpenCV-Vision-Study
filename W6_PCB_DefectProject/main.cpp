// ============================================================
// W6: PCB 缺陷检测综合项目 —— 程序入口
//
// ============================================================
//
// ## 这个文件现在只管三件事
//
//   1. 解析参数
//   2. 调 w6_cli 的纯逻辑
//   3. 把结果打印成人话
//
// 判定逻辑在 common/pcb_core/(W6/W7 共用一份),
// W6 自己的那层逻辑在 w6_cli.h/.cpp(可被测试链接)。
//
// ## 2026-10-20 的架构调整
//
// 原来这个文件里是:一张写死的 SITES[6] 真值表 + 七处
// `if (label == "R2")` 的规则分派。实测代价(拿三种别的板型直接跑):
//
//   - 元件数恒等于 6,与图上实际有什么无关
//   - 三块**好板**全被判废(误报 3/3)
//   - 两块废板的输出与好板**逐字相同**,等于没检
//
// 根因不是"坐标写死了",而是**规则也写死了** —— 换一块布局不同的板,
// 坐标不对,规则更是一条都跑不了。
//
// 而 W7 那边早就把判定改成了**配方驱动 + 逐条标定**的版本。也就是说:
// **能力在 W7、数据在 W6,而 W6 自己用不上。**
//
// 现在两边都调 common/pcb_core/ —— 同一份实现,不会各漂各的。
//
// ## 子模式（argv[1]）
//
//   baseline            跑完整管线,处理 images/baseline.png
//   inspect <path>      打开单张图,定位 + 判定,保存标注图
//   report [dir]        批量处理目录里所有 PNG(递归),每个元件写一行 CSV
//   gen                 调用 python gen_pcb_dataset.py 重新生成测试数据
//
// ## 可选参数
//
//   --recipes <dir>     板型配方目录(默认自动找)
//   --no-gui            inspect 模式不弹窗(无显示器 / 自动化用)
//
// ============================================================

#include "w6_cli.h"

#include <iostream>
#include <string>
#include <cstdlib>      // system()

#ifdef _WIN32
// windows.h 会把 min/max 定义成宏,不加 NOMINMAX 的话
// `std::max(...)` 会被展开成 `std::(max)(...)` -> C2589。
// 共享核心里也用了 std::max/std::min,所以这里必须挡掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ============================================================
// 参数
// ============================================================
struct Options {
    std::string recipesDir;
    bool noGui = false;
};

static Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--recipes" && i + 1 < argc) {
            o.recipesDir = argv[++i];
        } else if (a == "--no-gui") {
            o.noGui = true;
        }
    }
    return o;
}

// ============================================================
// baseline
// ============================================================
static void runBaseline(const Options& opt) {
    std::cout << "===== Baseline full pipeline =====\n";
    const std::string recipeDir = w6::findRecipeDir(opt.recipesDir);
    if (recipeDir.empty()) {
        std::cerr << "❌ 找不到板型配方目录(试了 recipes/ 与 ../W7_VisionMasterLite/recipes/)\n"
                     "   用 --recipes <dir> 指定\n";
        return;
    }
    w6::BoardResult br;
    std::string err;
    if (!w6::runBoard("images/baseline.png", recipeDir, br, err)) {
        std::cerr << "❌ " << err << "\n";
        return;
    }
    fs::create_directories("report");
    cv::Mat img = w6::loadImage("images/baseline.png");
    const w6::BoardCodes codes = w6::readBoardCodes(img, true);
    // 一个元件一行。行怎么摊由 w6_cli 说了算 ——
    // baseline 和 report 两条路径必须给出**一模一样**的行,
    // 所以这里不自己再摊一遍(那才是抄副本)。
    const std::vector<w6::ReportRow> rows =
        w6::rowsFromBoard("images/baseline.png", br, codes.silk, codes.qr);
    w6::writeCSVReport("report/baseline.csv", rows, true);

    std::cout << "  板型:      " << br.recipeName << " (" << br.boardTitle << ")\n";
    std::cout << "  元件数:    " << br.componentCount << "\n";
    std::cout << "  缺陷数:    " << br.defects.size() << "\n";
    std::cout << "  未判定:    " << (br.componentCount - br.withRules) << " 个元件没配规则\n";
    if (!br.skipped.empty()) {
        std::cout << "  ⚠ 跳过:    " << br.skipped.size() << " 条规则没跑(不是合格)\n";
        for (const auto& s : br.skipped) {
            std::cout << "    - " << s.component
                      << (s.rule.empty() ? "" : (" / " + s.rule))
                      << ": " << s.reason << "\n";
        }
    }
    std::cout << "  QR:        " << (codes.qr.empty() ? "(空)" : codes.qr) << "\n";
    // ⚠ 丝印空着的时候必须说清是"没读"还是"读了但是空"。
    //   "没读"的原因直接写在这里 —— 以前失败原因被算出来就丢掉了,
    //   界面上只看到一个空串,和"板上没丝印"分不开。
    if (!codes.silkRead) {
        std::cout << "  丝印:      ⚠ 没读到(" << codes.silkError << ")\n";
    } else {
        std::cout << "  丝印:      "
                  << (codes.silk.empty() ? "(空 —— 读到了,图上没字)" : codes.silk)
                  << "\n";
    }
    std::cout << "  " << (br.pass ? "✅ PASS" : "❌ FAIL") << "\n";
    std::cout << "  CSV 已写:  report/baseline.csv\n";
}

// ============================================================
// inspect
// ============================================================
static void runInspect(int argc, char** argv, const Options& opt) {
    const std::string imgPath = (argc > 2 && std::string(argv[2])[0] != '-')
                                    ? argv[2] : "images/baseline.png";
    std::cout << "===== Inspect: " << imgPath << " =====\n";

    const std::string recipeDir = w6::findRecipeDir(opt.recipesDir);
    if (recipeDir.empty()) {
        std::cerr << "❌ 找不到板型配方目录,用 --recipes <dir> 指定\n";
        return;
    }
    w6::BoardResult br;
    std::string err;
    if (!w6::runBoard(imgPath, recipeDir, br, err)) {
        std::cerr << "❌ " << err << "\n";
        return;
    }

    std::cout << "  板型:      " << br.recipeName << " (" << br.boardTitle << ")\n";
    std::cout << "  元件数:    " << br.componentCount << "\n";
    for (const auto& d : br.defects) {
        std::cout << "  ❌ " << d.type
                  << (d.component.empty() ? "" : (" @" + d.component))
                  << ": " << d.detail << "\n";
    }
    if (!br.skipped.empty()) {
        std::cout << "  ⚠ 跳过 " << br.skipped.size() << " 条规则(没查,不是合格):\n";
        for (const auto& s : br.skipped) {
            std::cout << "    - " << s.component
                      << (s.rule.empty() ? "" : (" / " + s.rule))
                      << ": " << s.reason << "\n";
        }
    }
    std::cout << "  " << (br.pass ? "✅ PASS" : "❌ FAIL") << "\n";

    const std::string outPath = "images/w6_inspect_marked.png";
    fs::create_directories("images");
    cv::imwrite(outPath, br.marked);
    std::cout << "  标注图已保存: " << outPath << "\n";

    if (!opt.noGui) {
        cv::namedWindow("W6 Inspect", cv::WINDOW_AUTOSIZE);
        cv::imshow("W6 Inspect", br.marked);
        std::cout << "按任意键关闭窗口...\n";
        cv::waitKey(0);
        cv::destroyAllWindows();
    }
}

// ============================================================
// report
// ============================================================
static void runReport(int argc, char** argv, const Options& opt) {
    const fs::path dir = (argc > 2 && std::string(argv[2])[0] != '-')
                             ? fs::path(argv[2]) : fs::path("images");
    std::cout << "===== Batch report: " << w6::pathText(dir) << " =====\n";

    const std::string recipeDir = w6::findRecipeDir(opt.recipesDir);
    if (recipeDir.empty()) {
        std::cerr << "❌ 找不到板型配方目录(试了 recipes/ 与 ../W7_VisionMasterLite/recipes/)\n"
                     "   用 --recipes <dir> 指定\n";
        return;
    }
    fs::create_directories("report");
    const std::string csvPath = "report/report.csv";
    fs::remove(csvPath);

    const w6::ReportResult rep = w6::runReport(dir, recipeDir, true);

    // 一次写完,表头由 writeCSVReport 写一次。
    //
    // ⚠ 别用"还没写过行"来判断要不要写表头:第一张图要是被跳过了,
    //   按行数判断就会写成"没表头",用 pandas 读第一行就成了列名。
    //   一次调用就没有这个坑。
    w6::writeCSVReport(csvPath, rep.rows, true);

    // 打印每块板的结论
    std::cout << "  判了 " << rep.boardCount << " 块板: "
              << rep.passCount << " PASS / " << rep.failCount << " FAIL\n";

    // 跳过的必须逐条列出来。静默跳过 = 少检了却没人知道。
    if (!rep.skipped.empty()) {
        std::cout << "\n  ⚠ 跳过 " << rep.skipped.size() << " 张(没有判定,不是合格):\n";
        for (const auto& s : rep.skipped) {
            std::cout << "    - " << s.file << " : " << s.reason << "\n";
        }
    }
    // 没读成的**读取**也要逐条列出来。
    //
    // ⚠ 这跟"跳过"不是一回事:板判了,只是丝印那一栏是空的。
    //   不报出来的话,看报告的人只会看到 silk 列空着,
    //   分不清是"板上没丝印"还是"tesseract 没装"。
    if (!rep.readFailures.empty()) {
        std::cout << "\n  ⚠ 有 " << rep.readFailures.size()
                  << " 项读取没成功(板判了,但那一栏是空的):\n";
        for (const auto& f : rep.readFailures) {
            std::cout << "    - " << f.file << " / " << f.what << " : " << f.reason << "\n";
        }
    }
    std::cout << "\n  共 " << rep.boardCount << " 张图: " << rep.passCount << " PASS / "
              << rep.failCount << " FAIL / " << rep.skipCount << " 跳过\n";
    std::cout << "  CSV 已写: " << csvPath << "\n";
}

// ============================================================
// gen
// ============================================================
static void runGen() {
    std::cout << "===== 重新生成测试数据 =====\n";
#ifdef _WIN32
    std::string cmd = "python gen_pcb_dataset.py";
    std::cout << "  调用: " << cmd << "\n";
    int rc = system(cmd.c_str());
    std::cout << "  返回码: " << rc << "\n";
#else
    std::cerr << "  本辅助函数仅 Windows 实现\n";
#endif
}

// ============================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const Options opt = parseOptions(argc, argv);
    std::string mode = (argc > 1) ? argv[1] : "baseline";

    if      (mode == "inspect")  runInspect(argc, argv, opt);
    else if (mode == "report")   runReport(argc, argv, opt);
    else if (mode == "gen")      runGen();
    else                         runBaseline(opt);   // 默认 = baseline

    return 0;
}
