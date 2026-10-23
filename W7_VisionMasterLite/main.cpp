// ============================================================
// W7 Mini VisionMaster Lite — Qt6 + C++ + OpenCV
// 学习版:复刻海康 VM 的核心思路(算子 + 流水线 + GUI)
// ============================================================
//
// 两种模式:
//
//   w7_vm_lite.exe                       开窗口(正常模式)
//   w7_vm_lite.exe --run pipeline.json   无头跑一条流水线
//
// ⚠ 为什么要有第二种模式:
//   W7 以前只能开窗跑 —— CI 跑的全是单元测试,
//   **从来没跑过一条完整流水线**。
//   "一条流水线从头到尾能不能跑通"这件事一直没有答案。
//
//   无头模式用的执行内核和窗口模式是**同一份**
//   (w7::runFrame / w7::topoOrder)——
//   不是"另一套执行逻辑",否则两边迟早漂,而两边都是绿的。
//
//   退出码: 0 = 跑完且判定 PASS;1 = 判定 FAIL;2 = 用法错或跑不了
//   (约定和理由见 headless_cli.h 开头那段)
// ============================================================

#include <QApplication>
#include <QFont>
#include <QTextStream>

#include "headless_cli.h"
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    // ⚠ 无头模式要在 QApplication 构造**之前**设好平台插件:
    //   QT_QPA_PLATFORM 是在 QApplication 构造时读的,之后再设没用。
    //   设成 offscreen = 不需要显示器(CI 里就是这么跑的)。
    //
    // ⚠ 参数从 argv 直接造,不用 QCoreApplication::arguments() ——
    //   那个要 app 实例才准,而实例化之前我们还不知道要不要开窗口
    //   (要不要开窗口决定 QT_QPA_PLATFORM 怎么设)。
    //   顺序要是反了,参数会**整份丢掉**。
    const QStringList args = w7::cliArgsFromArgv(argc, argv);
    const w7::CliOptions opt = w7::parseCliArgs(args);

    QTextStream err(stderr);
    if (!opt.valid) {
        err << QStringLiteral("参数有问题: %1\n\n").arg(opt.error);
        QTextStream out(stdout);
        out << w7::cliUsage();
        out.flush();
        err.flush();
        return 2;
    }
    if (opt.help) {
        QTextStream out(stdout);
        out << w7::cliUsage();
        out.flush();
        return 0;
    }

    if (opt.runMode) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication app(argc, argv);
        QFont font("Microsoft YaHei", 9);
        app.setFont(font);
        return w7::runHeadlessCli(opt);
    }

    QApplication app(argc, argv);

    // 默认字体(对标 VM 中文 UI)
    QFont font("Microsoft YaHei", 9);
    app.setFont(font);

    w7::MainWindow w;
    w.show();
    return app.exec();
}
