// ============================================================
// W7 Mini VisionMaster Lite — Qt6 + C++ + OpenCV
// 学习版:复刻海康 VM 的核心思路(算子 + 流水线 + GUI)
// ============================================================

#include <QApplication>
#include <QFont>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 默认字体(对标 VM 中文 UI)
    QFont font("Microsoft YaHei", 9);
    app.setFont(font);

    w7::MainWindow w;
    w.show();
    return app.exec();
}
