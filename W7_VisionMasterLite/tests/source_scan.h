#pragma once
// ============================================================
// source_scan.h —— 测试用的源码扫描辅助(只给测试目标用)
// ============================================================
//
// ⚠ 为什么有这个文件:
//   2026-10-25 那一轮学到两件事,都需要这套工具:
//
//   1) 「源码守卫查的是文本」—— 而**注释也是文本**。
//      守卫 `src.contains("w7::planResultRender")` 被 mainwindow.cpp 的
//      **注释**喂饱过:把真正的调用换掉,守卫照样绿。
//      所以查源码之前**先剥掉注释**。
//      (见 notes/atomic/at-the-guard-can-be-fed-by-a-comment.md)
//
//   2) 「抄副本」连测试代码里也会发生。
//      stripComments 第一次写在 test_result_render_plan.cpp 里,
//      第二个套件要用它的时候,抄一份是最省事的做法 ——
//      而那正是本项目栽过**九遍**的那种病。
//      所以住在**共享的**头文件里,两边都 include 它。
//
// ⚠ 这不是生产代码:
//   它只被测试目标 include,不进 w7_vm_lite。
// ============================================================

#include <QFile>
#include <QString>
#include <QStringList>

namespace w7test {

// ------------------------------------------------------------
// 读一个文本文件;打不开就返回空串
// ------------------------------------------------------------
inline QString readSource(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// ------------------------------------------------------------
// 剥掉注释,只留代码
// ------------------------------------------------------------
//
// ⚠ 字符串里的 // 和 /* 不算注释 —— 剥错了同样会骗人(只是骗的方向反过来)。
// ============================================================
inline QString stripComments(const QString& src) {
    QString out;
    out.reserve(src.size());
    enum State { Code, LineComment, BlockComment, Str, Chr } st = Code;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        const QChar n = (i + 1 < src.size()) ? src.at(i + 1) : QChar();
        switch (st) {
        case Code:
            if (c == QLatin1Char('/') && n == QLatin1Char('/')) { st = LineComment; ++i; }
            else if (c == QLatin1Char('/') && n == QLatin1Char('*')) { st = BlockComment; ++i; }
            else if (c == QLatin1Char('"')) { st = Str; out += c; }
            else if (c == QLatin1Char('\'')) { st = Chr; out += c; }
            else out += c;
            break;
        case LineComment:
            if (c == QLatin1Char('\n')) { st = Code; out += c; }
            break;
        case BlockComment:
            if (c == QLatin1Char('*') && n == QLatin1Char('/')) { st = Code; ++i; }
            break;
        case Str:
            out += c;
            if (c == QLatin1Char('\\') && i + 1 < src.size()) out += src.at(++i);
            else if (c == QLatin1Char('"')) st = Code;
            break;
        case Chr:
            out += c;
            if (c == QLatin1Char('\\') && i + 1 < src.size()) out += src.at(++i);
            else if (c == QLatin1Char('\'')) st = Code;
            break;
        }
    }
    return out;
}

// ------------------------------------------------------------
// 真正被 include 的头文件
// ------------------------------------------------------------
//
// ⚠ 只认**指令行** —— 注释里的 "#include <QWidget>" 不算。
//   这是「剥注释」之外的另一种精确:判据是**语法**,不是**文本**。
//
// ⚠ 尖括号和引号**都要**:
//   #include <QPlainTextEdit>  —— 拖 widget
//   #include "mainwindow.h"     —— 也拖 widget(而且拖得更多)
//
//   只看尖括号的话,第二种写法会漏过去。
// ============================================================
inline QStringList includeDirectives(const QString& src) {
    QStringList out;
    for (const QString& raw : src.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.startsWith(QLatin1String("#include"))) continue;

        const int lt = line.indexOf(QLatin1Char('<'));
        const int gt = line.lastIndexOf(QLatin1Char('>'));
        if (lt >= 0 && gt > lt) {
            out.append(line.mid(lt + 1, gt - lt - 1));
            continue;
        }
        const int q1 = line.indexOf(QLatin1Char('"'));
        const int q2 = line.lastIndexOf(QLatin1Char('"'));
        if (q1 >= 0 && q2 > q1)
            out.append(line.mid(q1 + 1, q2 - q1 - 1));
    }
    return out;
}

} // namespace w7test
