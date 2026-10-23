#pragma once
// ============================================================
// headless CLI —— 命令行这一层
// ============================================================
//
// 为什么单独拉一个文件:
//   main.cpp 是**入口**,它只做三件事:
//     1. 解析参数
//     2. 有 --run 就走无头,没有就开窗口
//     3. 把退出码定下来
//
//   真正值得测的是「参数怎么解析」和「跑完之后退出码是几」——
//   所以这两样放在这里,而不是塞进 main() 里。
//
// ⚠ 依赖面分层:
//   · parseCliArgs  —— **纯 QtCore**(头文件里 inline),能直接测
//   · runHeadlessCli —— 要真算子(OpenCV),只能靠**端到端冒烟**验
//
//   这条分层是刻意的:能纯的部分纯,不能纯的部分用真实二进制验。
//   拿假的算子去验"真算子接得上吗"是自欺欺人。
//
// ============================================================
// 退出码约定(CI 靠它判成败)
// ============================================================
//
//   0  跑完了,没有步骤报错,判定是 PASS(或者这条流水线本来就不做判定)
//   1  跑完了,没有步骤报错,但判定是 **FAIL**
//   2  用法错 / 文件读不了 / JSON 坏了 / 流水线根本跑不了(有环等等)
//   3  跑完了,但**有步骤报错**(某个算子跑了却在输出里带 error)
//
// ⚠ 四种情况必须分开,不能合成一个"非零":
//
//   2 = 我们**没跑起来** —— 工具坏了
//   3 = 跑起来了,但流水线里有步骤报错 —— 工具能用,流水线坏了
//   1 = 跑起来了,判定 FAIL —— 工具能用,流水线说没通过
//
//   合成一个码的话,CI 就分不清该修工具还是该修流水线,
//   而"分不清"在实践中就等于"没人修"。
//
// ⚠ 3 优先于 1:步骤报错时那个 FAIL 往往只是它的**后果**。
//
// ⚠ "算子跑了但输出里带 error"也算错:
//   引擎看的是"有没有崩",而用户关心的是"有没有算成"。
//   窗口那边把这种状态点标成橙色 —— 这边也得算成错误,
//   不然退出码是 0,CI 就当它过了。
//
// ============================================================

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

namespace w7 {

// ============================================================
// 命令行参数表(从 argv 造)
// ============================================================
//
// ⚠ **不要用 QCoreApplication::arguments()**:
//   它要 QApplication 实例才准 —— 而实例化之前我们还不知道
//   要不要开窗口(开不开窗口决定 QT_QPA_PLATFORM 怎么设,
//   而那个必须在 QApplication 构造**之前**定下来)。
//   于是顺序就成了死结:先要参数才知道模式,先要模式才能构造 app。
//
//   直接从 argv 造一份,死结就解开了 ——
//   而且这样解析器**完全不依赖 app 实例**,能直接进单元测试。
inline QStringList cliArgsFromArgv(int argc, char* argv[]) {
    QStringList a;
    a.reserve(argc);
    for (int i = 0; i < argc; ++i)
        a << QString::fromLocal8Bit(argv[i]);   // Windows 上 argv 是 ANSI 码页
    return a;
}

// ============================================================
// 命令行选项
// ============================================================
struct CliOptions {
    bool help = false;         // --help
    bool valid = false;        // 参数本身没问题
    bool runMode = false;      // 给了 --run -> 无头模式;否则开窗口

    QString runFile;           // --run <pipeline.json>
    QString outFile;           // --out <result.json>
    QString imagePath;         // --image <path>(覆盖 JSON 里带的)
    int     maxFrames = 0;     // --frames <N>(<= 0 = 不额外加限)

    QString error;             // 参数有问题时的说明
};

// ============================================================
// parseCliArgs —— 解析命令行(纯 QtCore)
// ============================================================
//
// ⚠ 这里**不**用 QCommandLineParser::process():
//   process() 遇到 --help 会直接 exit,没法测。
//   parse() 只做解析,副作用交给调用方 —— 于是这段能进单元测试。
inline CliOptions parseCliArgs(const QStringList& args) {
    CliOptions o;

    QCommandLineParser p;
    p.setApplicationDescription(
        QStringLiteral("W7 Mini VisionMaster Lite —— 无头跑一条流水线"));
    p.addHelpOption();
    p.addOption({{QStringLiteral("run"), QStringLiteral("r")},
                  QStringLiteral("pipeline.json"),
                  QStringLiteral("跑一条流水线(JSON 文件),不开窗口")});
    p.addOption({{QStringLiteral("out"), QStringLiteral("o")},
                  QStringLiteral("result.json"),
                  QStringLiteral("把结果写成 JSON 文件")});
    p.addOption({{QStringLiteral("image"), QStringLiteral("i")},
                  QStringLiteral("path"),
                  QStringLiteral("图像路径(覆盖 JSON 里带的那个)")});
    p.addOption({{QStringLiteral("frames"), QStringLiteral("f")},
                  QStringLiteral("N"),
                  QStringLiteral("最多跑几帧")});

    if (!p.parse(args)) {
        o.error = p.errorText();
        return o;
    }
    if (p.isSet(QStringLiteral("help"))) {
        o.help = true;
        o.valid = true;
        return o;
    }

    o.runFile  = p.value(QStringLiteral("run"));
    o.outFile  = p.value(QStringLiteral("out"));
    o.imagePath = p.value(QStringLiteral("image"));
    o.runMode  = !o.runFile.isEmpty();

    if (p.isSet(QStringLiteral("frames"))) {
        bool okNum = false;
        const int n = p.value(QStringLiteral("frames")).toInt(&okNum);
        if (!okNum || n < 1) {
            o.error = QStringLiteral("--frames 要一个 >= 1 的整数");
            return o;
        }
        o.maxFrames = n;
    }

    // --out 只有在无头模式下才有意义
    if (!o.runMode && !o.outFile.isEmpty()) {
        o.error = QStringLiteral("--out 只能和 --run 一起用");
        return o;
    }

    o.valid = true;
    return o;
}

// ============================================================
// 用法文本
// ============================================================
inline QString cliUsage() {
    return QStringLiteral(
        "用法:\n"
        "  w7_vm_lite.exe                       开窗口(正常模式)\n"
        "  w7_vm_lite.exe --run pipeline.json   无头跑一条流水线\n"
        "\n"
        "选项:\n"
        "  -r, --run <file>     要跑的流水线(JSON)\n"
        "  -o, --out <file>     把结果写成 JSON\n"
        "  -i, --image <path>   图像路径(覆盖 JSON 里带的)\n"
        "  -f, --frames <N>     最多跑几帧\n"
        "  -h, --help           这段话\n"
        "\n"
        "退出码: 0 = 跑完且判定 PASS;1 = 判定 FAIL;"
        "2 = 用法错或跑不了;3 = 有步骤报错\n");
}

// ============================================================
// runHeadlessCli —— 无头跑一条流水线(声明在 headless_cli.cpp)
// ============================================================
//
// 返回值就是进程退出码(见上面那份约定)。
int runHeadlessCli(const CliOptions& opt);

} // namespace w7
