// ============================================================
// test_operator_knobs.cpp — 旋钮审计
// ============================================================
//
// ------------------------------------------------------------
// 这条套件在查什么
// ------------------------------------------------------------
// 「旋钮」= 算子的参数面板上那一个个可调的输入。
//
// 界面上摆一个旋钮,用户以为它有用;旋钮背后对应的那段代码,
// 决定了这个旋钮**到底调不调得动**。两者之间可能差着三种东西:
//
//   1. 死旋钮:defaultParams() 登记了它,面板上也画出来了,
//      但 run() 从来没读过它 —— 用户调了,什么都没发生。
//      界面上摆一个"调了不生效"的旋钮 = 骗人。
//
//   2. 没登记的旋钮:run() 读了它,面板上也画了,但
//      defaultParams() 没登记 —— 于是写进流水线的参数 JSON 里
//      压根没有这个 key,它的默认值只活在 run() 的兜底代码里。
//      用户在面板上看到的初值,和算子实际用的兜底值,可能不是一个数。
//
//   3. 抄了多份的默认值:同一个默认值写在 defaultParams()、
//      run() 的兜底、面板 buildUI() 三处。改一处忘一处,
//      就出现"面板上写着 A、实际跑的是 B"。
//      **这不是假想敌** —— 本套件落地前实测已经有三处漂了:
//        blob_count.min_area      声明 20   兜底 50
//        count_check.expected_min 声明 1    兜底 0
//        count_check.expected_max 声明 100  兜底 999
//
// ------------------------------------------------------------
// 这条套件**怎么**查
// ------------------------------------------------------------
// 两种手段,各有各的边界,都写在下面,别把它们当成更强的证据:
//
// 【源码级 lint】(用例 1~4)
//   扫 operators/*.cpp 的源码,做静态检查。
//   ⚠ 它认得死旋钮是因为「登记了却没被读」,认不得
//     「用一种奇怪写法读了但没生效」—— 那种要靠用例 5 的行为测试。
//   ⚠ 它的另一条边界:它只认「run() 里 params.value("x") 这种读法」。
//     换成 readHsvScalar(params.value("x"), ...) 这种包一层的写法,
//     它就看不见了。
//
// 【行为测试】(用例 5~6)
//   真的把算子跑一遍:拿「空参数」和「声明的默认参数」各跑一次,
//   两次的判定结果必须一样。
//   为什么这个比 lint 强:lint 只看"读没读",行为测试看的是
//   **算出来的数一不一样**。兜底值和声明值不一样,数就会不一样。
//   ⚠ 边界:只有「输出里能看出差别」的算子才被这条抓住
//     (比如 count_check 把两个阈值原样吐在输出里)。
//     输出里看不出差别的算子(比如 template_match 的 score 与阈值无关),
//     这条只能钉住"两次运行一致",钉不住具体阈值。
//
// ------------------------------------------------------------
// 为什么要有「豁免清单」
// ------------------------------------------------------------
// 有些算子依赖外部 SDK(ONNX Runtime / 海康 MVS),
// 没装的时候那部分代码**根本不参与编译**。
// 源码里读得到旋钮,当前这个构建里读不到 ——
// 这不是"写错了",是"这个功能现在没装上"。
//
// 这种旋钮必须在**显式清单**里登记,并写明原因:
//   · 清单漏了 → 审计报红(有人新加了个停用分支,忘了登记)
//   · 清单过期了(SDK 装上了,旋钮其实已经生效)→ 审计报红(该删这条)
// 清单自己也会烂掉,所以必须有人盯着 —— 这就是用例 4 干的事。
//
// ============================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "operator_base.h"
#include "operator_factory.h"
#include "dl_infer.h"
#include "image_source_camera.h"
#include "param_read.h"   // KnobKind —— 用例 9 要按旋钮类型分别取"坏值"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <windows.h>   // SetConsoleOutputCP

#include <algorithm>
#include <cmath>

using namespace w7;

// ============================================================
// 极简断言(与其余套件同一套风格)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static QString g_case;

static void beginCase(const QString& name) {
    g_case = name;
    qInfo().noquote() << "\n=== " << name << " ===";
}
static void check(bool cond, const QString& what) {
    if (cond) { ++g_pass; qInfo().noquote() << "  [PASS]" << what; }
    else { ++g_fail; qWarning().noquote() << "  [FAIL]" << what
           << "  (case:" << g_case << ")"; }
}

// ============================================================
// 源码解析小工具
// ============================================================

// 去掉注释,**保留字符串字面量**。
//
// 为什么必须去注释:本文件里记录历史的那几段说明,
// 恰好写着被审计的那些字样(比如"以前这里登记的是 min_area = 80")。
// 不去注释的话,审计第一天就对自己的文档误报 ——
// 而一个对自身文档误报的守卫,下一个人只会把它删掉。
//
// 为什么要区分字符串:源码里有 "D://xxx" 这种路径,
// 按行 naively 地切 // 会把后面真代码一起切掉。
static QString stripComments(const QString& src) {
    QString out;
    out.reserve(src.size());
    bool inStr = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) {           // 转义,跳过下一个字符
                out += c;
                if (i + 1 < src.size()) out += src[++i];
                continue;
            }
            if (c == QLatin1Char('"')) inStr = false;
            out += c;
            continue;
        }
        if (c == QLatin1Char('"')) { inStr = true; out += c; continue; }
        if (c == QLatin1Char('/') && i + 1 < src.size() &&
            src[i + 1] == QLatin1Char('/')) {
            while (i < src.size() && src[i] != QLatin1Char('\n')) ++i;
            if (i < src.size()) out += QLatin1Char('\n');
            continue;
        }
        // 块注释 /* ... */:源码里有几处多行说明,不去掉会挡住函数体
        //
        // ⚠⚠ 这里踩过一个**只有 C++ 才会踩**的坑:
        //   for 循环每次迭代结束会自动 ++i。所以跳过注释之后,
        //   i 必须停在注释的**最后一个字符**上,让自增把它带走 ——
        //   写成 i += 2(跳过 "*/" 两个字符)的话,自增会再吃掉
        //   紧随其后的那个字符。
        //
        //   实测后果(2026-10-21):image_source_camera 的签名是
        //       run(const QJsonObject& params, const QJsonObject& /*inputs*/) {
        //   那个右括号被吃掉,变成
        //       run(const QJsonObject& params, const QJsonObject&  {
        //   于是**括号配对数错**,整个 run() 函数体找不到 ——
        //   相机的三个旋钮全被误判成"死旋钮"。
        //
        //   同一个逻辑的 Python 复刻是对的:Python 的 while 循环
        //   不会自动自增,所以两边的行为不一样 —— 复刻不等于等价。
        if (c == QLatin1Char('/') && i + 1 < src.size() &&
            src[i + 1] == QLatin1Char('*')) {
            i += 2;                                  // 跳过 "/*"
            while (i + 1 < src.size() &&
                   !(src[i] == QLatin1Char('*') && src[i + 1] == QLatin1Char('/'))) ++i;
            i += 1;                                  // 停在 "*/" 的最后一个字符上
            continue;                                // 由 for 的 ++i 带走
        }
        out += c;
    }
    return out;
}

// 把**字符串/字符字面量里**的花括号换成空格,别的内容一个字都不动。
//
// 为什么只动花括号:找函数体要配对括号,而字符串里可能有花括号;
// 但字符串里的其它内容必须留着 —— key 名("expected_min")、
// 算子名("count_check")都在字符串里,抹了就等于审计什么都没扫到。
//
// ⚠ 第一版把整个字符串内容抹成空格,结果 parseOpName() 和 parseReads()
//   全都认不出东西:扫到的算子数是 0,三个 lint 全部变成恒真的空断言,
//   而测试还绿着。这就是"注释会骗人,断言不会"的反面:
//   **断言也会骗人**,如果它断言的东西本身是空的。
static QString blankBracesInStrings(const QString& src) {
    QString out = src;
    bool inStr = false, inChr = false;
    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('"')) { inStr = false; continue; }
            if (c == QLatin1Char('{') || c == QLatin1Char('}'))
                out[i] = QLatin1Char(' ');
            continue;
        }
        if (inChr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('\'')) { inChr = false; continue; }
            if (c == QLatin1Char('{') || c == QLatin1Char('}'))
                out[i] = QLatin1Char(' ');
            continue;
        }
        if (c == QLatin1Char('"')) { inStr = true; continue; }
        if (c == QLatin1Char('\'')) { inChr = true; continue; }
    }
    return out;
}

static int matchBracket(const QString& s, int open, QChar openC, QChar closeC) {
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s[i] == openC) ++depth;
        else if (s[i] == closeC) { --depth; if (depth == 0) return i; }
    }
    return -1;
}

// 按**顶层逗号**切参数 —— 括号/方括号/花括号里的逗号不算。
//
// 为什么不能直接 split(",") :
//   readIntKnobs(params, kKeys, defaultParams(), &out, &err) 没问题,
//   但键名里带逗号的那种(readTextKnob(params, d, "a,b", ...))就会被切成两半。
//   而且 defaultParams() 里的括号也必须整体跳过。
static QStringList splitTopLevel(const QString& s) {
    QStringList out;
    int depth = 0;
    bool inStr = false, inChr = false;
    int start = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('"')) inStr = false;
            continue;
        }
        if (inChr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('\'')) inChr = false;
            continue;
        }
        if (c == QLatin1Char('"')) { inStr = true; continue; }
        if (c == QLatin1Char('\'')) { inChr = true; continue; }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) { ++depth; continue; }
        if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) { --depth; continue; }
        if (c == QLatin1Char(',') && depth == 0) {
            out.append(s.mid(start, i - start).trimmed());
            start = i + 1;
        }
    }
    const QString tail = s.mid(start).trimmed();
    if (!tail.isEmpty() || !out.isEmpty()) out.append(tail);
    return out;
}

// 找出所有 `name(` 的**函数定义**并返回函数体。
//
// ⚠ 只认"参数表后面紧跟 {" 的那些 ——
//   `op->defaultParams()` 这种调用后面跟的是 "("，不能当定义。
//   (第一版就栽在这:把调用也当成了定义,于是"没读到"的算子被误判成死旋钮。)
//
// ⚠ 第二版又栽了一次:参数表和 "{" 之间还夹着**限定符** ——
//   `QJsonObject CountCheck::defaultParams() const {`
//   第一版只跳空白,遇到 "const" 就以为不是定义,**结果一个算子都没扫到**,
//   三个 lint 全变成恒真的空断言(测试还绿着)。
//   所以这里要跳过:空白 + 标识符(const / override / noexcept / final)+ 空白。
static QStringList functionBodies(const QString& src, const QString& name) {
    QStringList out;
    const QRegularExpression re(QStringLiteral("\\b%1\\s*\\(").arg(name));
    auto it = re.globalMatch(src);
    while (it.hasNext()) {
        const auto m = it.next();
        const int open = m.capturedEnd() - 1;
        const int close = matchBracket(src, open, QLatin1Char('('), QLatin1Char(')'));
        if (close < 0) continue;
        int i = close + 1;
        // 跳过空白与限定符,直到看见 '{' 或别的
        for (;;) {
            while (i < src.size() && src[i].isSpace()) ++i;
            // 一个标识符?(const / override / noexcept / final)
            if (i < src.size() && (src[i].isLetter() || src[i] == QLatin1Char('_'))) {
                int j = i;
                while (j < src.size() &&
                       (src[j].isLetterOrNumber() || src[j] == QLatin1Char('_'))) ++j;
                i = j;
                continue;
            }
            break;
        }
        if (i >= src.size() || src[i] != QLatin1Char('{')) continue;  // 调用,不是定义
        const int end = matchBracket(src, i, QLatin1Char('{'), QLatin1Char('}'));
        if (end < 0) continue;
        out.append(src.mid(i, end - i + 1));
    }
    return out;
}

// 判断某个宏在**当前构建**里是否被定义。
//
// 只有两个宏会决定算子源码的分支结构:
//   HAS_ONNXRUNTIME —— 装没装 ONNX Runtime
//   HAS_MVS_SDK      —— 装没装海康 MVS SDK
//
// 这两个的答案不靠猜:直接问算子自己(runtimeAvailable())。
// 问错的话,审计会把"编译进来的那支"认反,
// 于是豁免清单的判断整个反过来 —— 那比没有审计更坏。
static bool macroDefined(const QString& name, bool hasOnnx, bool hasMvs) {
    if (name == QLatin1String("HAS_ONNXRUNTIME")) return hasOnnx;
    if (name == QLatin1String("HAS_MVS_SDK"))      return hasMvs;
    return false;
}

// 返回「当前构建真正会编进去」的行的行号集合(1-based)。
//
// 为什么需要它:dl_infer 里那两支 run() 只有一支参与编译。
// 不判断分支的话,审计只能看到"源码里读得到",看不到
// "这个构建里读不到" —— 而后者才是死旋钮。
static QSet<int> liveLines(const QStringList& lines, bool hasOnnx, bool hasMvs) {
    QSet<int> live;
    // 栈:每一层 = (外层是否活着, 是否已经走过某个分支)
    QVector<QPair<bool, bool>> stack;
    bool outer = true;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (t.startsWith(QLatin1String("#ifdef"))) {
            const QString m = t.mid(6).trimmed();
            const bool cur = outer && macroDefined(m, hasOnnx, hasMvs);
            stack.append(qMakePair(outer, cur));
            outer = cur;
        } else if (t.startsWith(QLatin1String("#ifndef"))) {
            const QString m = t.mid(7).trimmed();
            const bool cur = outer && !macroDefined(m, hasOnnx, hasMvs);
            stack.append(qMakePair(outer, cur));
            outer = cur;
        } else if (t.startsWith(QLatin1String("#else"))) {
            if (!stack.isEmpty()) {
                const bool parent = stack.last().first;
                const bool anyTaken = stack.last().second;
                outer = parent && !anyTaken;
                stack.last().second = true;
            }
        } else if (t.startsWith(QLatin1String("#endif"))) {
            if (!stack.isEmpty()) {
                outer = stack.last().first;
                stack.removeLast();
            }
        }
        if (outer) live.insert(i + 1);
    }
    return live;
}

// 把「当前构建不参与编译的行」清空(保留行数),得到 live 源码。
static QString maskDeadLines(const QString& src, const QSet<int>& live) {
    const QStringList lines = src.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    for (int i = 0; i < lines.size(); ++i)
        out.append(live.contains(i + 1) ? lines[i] : QString());
    return out.join(QLatin1Char('\n'));
}

// ============================================================
// 兜底读法:params.value("KEY").toXxx(EXPR)
// ============================================================
//
// EXPR 就是"这个 key 缺了的时候,算子拿哪个值顶上" ——
// 它是默认值的**第二份**,第一份在 defaultParams() 里。
//
// ⚠ 为什么必须跨行找:
//   pcb_inspect.cpp 写的是
//       params.value("mode").toString(
//           QString::fromUtf8(PcbInspectDefaults::kMode));
//   表达式在下一行。正则写成单行就看不见这条兜底 ——
//   而"看不见"等于"没检查"。
//
// ⚠ 为什么不能简单地"整个 run() 里不许有数字":
//   run() 里全是算法常量(面积换算、颜色阈值、像素偏移),
//   那些数字不是默认值,是算法本身。禁掉它们等于把算法也禁了。
//   所以这里只盯**兜底表达式**那一小段 ——
//   那一段里的数字,按定义就是"某个旋钮的默认值"。
// ============================================================
struct FallbackRead {
    QString key;      // 读的是哪个旋钮
    QString expr;     // 兜底值表达式(toXxx 的那个参数)
};

static QVector<FallbackRead> parseFallbackReads(const QString& body) {
    QVector<FallbackRead> out;
    const QRegularExpression re(
        QStringLiteral("params\\s*\\.\\s*value\\s*\\(\\s*\"([^\"]+)\"\\s*\\)"));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        int i = m.capturedEnd();
        while (i < body.size() && body[i].isSpace()) ++i;   // 换行也算空白
        if (i >= body.size() || body[i] != QLatin1Char('.')) continue;
        ++i;
        const int idStart = i;
        while (i < body.size() &&
               (body[i].isLetterOrNumber() || body[i] == QLatin1Char('_'))) ++i;
        const QString method = body.mid(idStart, i - idStart);
        if (!method.startsWith(QLatin1String("to"))) continue;  // 不是 toXxx(...)
        while (i < body.size() && body[i].isSpace()) ++i;
        if (i >= body.size() || body[i] != QLatin1Char('(')) continue;
        const int close = matchBracket(body, i, QLatin1Char('('), QLatin1Char(')'));
        if (close < 0) continue;
        FallbackRead fr;
        fr.key = m.captured(1);
        fr.expr = body.mid(i + 1, close - i - 1).trimmed();
        out.append(fr);
    }
    return out;
}

// ============================================================
// 面板初值:setValue(EXPR)
// ============================================================
//
// buildUI() 里每个 setValue(...) 的参数 —— 那是"界面上初值那份"。
// ============================================================
static QStringList parseSetValueArgs(const QString& body) {
    QStringList out;
    const QRegularExpression re(QStringLiteral("\\bsetValue\\s*\\("));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        const int open = m.capturedEnd() - 1;
        const int close = matchBracket(body, open, QLatin1Char('('), QLatin1Char(')'));
        if (close < 0) continue;
        out.append(body.mid(open + 1, close - open - 1).trimmed());
    }
    return out;
}

// ============================================================
// 表达式里有没有"裸字面量"
// ============================================================
//
// 判据:先把下标和标识符都抹掉,剩下的东西里还有数字/引号 → 有裸字面量。
//
// 为什么先抹下标:lo[0] 里的 0 是**下标**,不是值。
//   当成值来报,审计第一天就会对 color_segment 误报 ——
//   而一个对自家代码误报的守卫,下一个人只会把它关掉。
//
// 为什么先抹标识符:kMinArea、pcbcolor::kMaskVMul 这些是**具名常量**,
//   它们正是我们要的。裸字面量指的是 0.9 / 85 / "*.png" 这种
//   手抄的、没有名字的值。
static QString exprWithoutSubscripts(const QString& expr) {
    QString t = expr;
    t.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")));
    return t;
}

static QString exprWithoutIdentifiers(const QString& expr) {
    QString t = exprWithoutSubscripts(expr);
    t.remove(QRegularExpression(QStringLiteral("[A-Za-z_][A-Za-z_0-9]*")));
    return t;
}

// 裸数字字面量?
static bool exprHasBareNumber(const QString& expr) {
    return exprWithoutIdentifiers(expr).contains(QRegularExpression(QStringLiteral("[0-9]")));
}

// 裸字符串字面量?(只认双引号。字符字面量不算 —— 它常是分隔符,不是默认值)
static bool exprHasBareString(const QString& expr) {
    return exprWithoutIdentifiers(expr).contains(QLatin1Char('"'));
}

// ============================================================
// 算子自己的 Defaults 结构体名
// ============================================================
//
//   pcb_inspect.cpp      -> PcbInspectDefaults
//   image_source_camera  -> ImageSourceCameraDefaults
//
// 规则:文件名按 "_" 拆开,每段首字母大写,再接 "Defaults"。
// 这条规则只用来**核对引用** —— 真正的定义在 operator_defaults.h,
// 3b 那条会验它真的在那儿。
// ============================================================
static QString ownDefaultsStructName(const QString& fileBaseName) {
    QString out;
    const QStringList parts = fileBaseName.split(QLatin1Char('_'));
    for (const QString& p : parts) {
        if (p.isEmpty()) continue;
        out += p.at(0).toUpper() + p.mid(1);
    }
    return out + QStringLiteral("Defaults");
}

// ============================================================
// operator_defaults.h 里的常量(带类型)
// ============================================================
struct DefaultsMember {
    QString structName;
    QString memberName;
    bool isString = false;     // const char* 的算字符串型
};

static QVector<DefaultsMember> parseDefaultsMembers(const QString& header) {
    QVector<DefaultsMember> out;
    const QRegularExpression structRe(QStringLiteral("struct\\s+(\\w+Defaults)\\s*\\{"));
    auto it = structRe.globalMatch(header);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1);
        const int open = m.capturedEnd() - 1;
        const int close = matchBracket(header, open, QLatin1Char('{'), QLatin1Char('}'));
        if (close < 0) continue;
        const QString body = header.mid(open, close - open + 1);
        const QRegularExpression memberRe(
            QStringLiteral("static\\s+constexpr\\s+"
                           "(const\\s+char\\s*\\*|int|double|float|bool)\\s+(k\\w+)\\s*="));
        auto mit = memberRe.globalMatch(body);
        while (mit.hasNext()) {
            const auto mm = mit.next();
            DefaultsMember dm;
            dm.structName = name;
            dm.memberName = mm.captured(2);
            dm.isString = mm.captured(1).contains(QLatin1String("char"));
            out.append(dm);
        }
    }
    return out;
}

// ============================================================
// 面板 setParams() 里的兜底读法
// ============================================================
//
// 和 run() 的那套只差一处:键名可以是**变量**(kKeys[i]),
// 不是字面量 —— 所以这里不关心"读的是哪个键",
// 只关心"兜底表达式里有没有裸字面量"。
//
// ⚠ 特别要抓 **toInt() / toDouble() 不带参数**:
//   看着像"没写兜底",其实是 Qt 的默认参数 0 ——
//   它是**隐式**的,源码里看不见那个 0,lint 也看不见。
//   而 0 几乎从来不是那个旋钮的声明值。
// ============================================================
struct ValueRead {
    QString keyExpr;    // 读的表达式(可能是字面量,也可能是 kKeys[i])
    QString expr;       // 兜底表达式(toXxx 的参数;空 = 没写兜底)
};

// ============================================================
// 一个算子源码的解析结果
// ============================================================
struct OperatorSource {
    QString file;                 // "circle_fit.cpp"
    QString opName;               // "circle_fit"
    QStringList declared;         // defaultParams() 登记的 key
    QVector<QPair<QString, QString>> defaultExprs;  // key -> 右值表达式
    QSet<QString> readsLive;      // 当前构建的 run() 读到的 key
    QSet<QString> readsAll;       // 所有分支的 run() 读到的 key
    QVector<FallbackRead> fallbacks;   // run() 里的兜底读法(见下)
    QVector<FallbackRead> helperFallbacks;  // 新读法(readXxxKnob)里的兜底
    QString buildUi;              // buildUI() 的函数体(没有面板就空)
    QStringList setValueArgs;     // buildUI() 里每个 setValue(...) 的参数
    QStringList helperIssues;     // 审计**认不出来**的地方(见 parseHelperReads)
    QStringList helperFallbackIssues;  // 新读法的兜底**抄了第二份**(不是认不出)
    QString setParamsBody;        // setParams() 的函数体(没有面板就空)
    QVector<ValueRead> setParamsReads;   // setParams() 里的兜底读法
};

// defaultParams() 里登记的 key 与右值表达式
static void parseDefaults(const QString& body, OperatorSource& op) {
    const QRegularExpression re(
        QStringLiteral("p\\[\"([^\"]+)\"\\]\\s*=\\s*([^;]+);"));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        op.declared.append(m.captured(1));
        op.defaultExprs.append(qMakePair(m.captured(1), m.captured(2).trimmed()));
    }
}

// run() 里读到的 params key
static QSet<QString> parseReads(const QString& body) {
    QSet<QString> keys;
    const QStringList pats = {
        QStringLiteral("params\\s*\\.\\s*value\\s*\\(\\s*\"([^\"]+)\""),
        QStringLiteral("params\\s*\\[\\s*\"([^\"]+)\"\\s*\\]"),
        QStringLiteral("params\\s*\\.\\s*contains\\s*\\(\\s*\"([^\"]+)\"")
    };
    for (const QString& p : pats) {
        auto it = QRegularExpression(p).globalMatch(body);
        while (it.hasNext())
            keys.insert(it.next().captured(1));
    }
    return keys;
}

// ============================================================
// 新读法:readIntKnobs(params, <键名清单>, <兜底>, ...)
// ============================================================
//
// 2026-10-21 引入(见 operators/param_read.h):测量类算子的坐标旋钮
// 改成走这个 helper,好处是"类型不对就报错"和"兜底值只有一份"。
//
// 代价是**审计认不出这种读法了** —— 键名在常量里,不再是
// params.value("x1") 这种字面量形态。
//
// 实测后果:迁移完一编译,审计报了 **28 条"死旋钮"**
// (8+6+6+4+4,正好是五个测量算子的坐标旋钮个数)。
// 而这些旋钮**明明被读了** —— 审计在撒谎,而且是"看起来很肯定"地撒谎。
//
// 教训:**审计的识别能力跟不上代码,它就会开始撒谎。**
// 升级代码的同时必须升级审计,否则最危险的不是漏报,是误报 ——
// 误报会把人引到完全错误的方向(去查旋钮,其实旋钮好着呢)。
//
// 所以这里补上:
//   1. 认出源文件里的"键名清单"常量(键名 → 清单)
//   2. 认出 readIntKnobs/readDoubleKnobs 这种读法,把它读的键记下来
//   3. 认不出来的时候**明说"我认不出"**,而不是判成"死旋钮"
// ============================================================

// ============================================================
// 面板 setParams() 里的兜底读法
// ============================================================
//
// 实现见上面的 struct ValueRead。
static QVector<ValueRead> parseValueReads(const QString& body) {
    QVector<ValueRead> out;
    const QRegularExpression re(
        QStringLiteral("params\\s*\\.\\s*value\\s*\\(|params\\s*\\["));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        const bool bracketForm = m.captured(0).endsWith(QLatin1Char('['));
        int close = -1;
        if (bracketForm)
            close = matchBracket(body, m.capturedEnd() - 1, QLatin1Char('['), QLatin1Char(']'));
        else
            close = matchBracket(body, m.capturedEnd() - 1, QLatin1Char('('), QLatin1Char(')'));
        if (close < 0) continue;

        ValueRead vr;
        vr.keyExpr = body.mid(m.capturedStart(), close - m.capturedStart() + 1).trimmed();

        int i = close + 1;
        while (i < body.size() && body[i].isSpace()) ++i;
        if (i >= body.size() || body[i] != QLatin1Char('.')) continue;
        ++i;
        const int idStart = i;
        while (i < body.size() &&
               (body[i].isLetterOrNumber() || body[i] == QLatin1Char('_'))) ++i;
        const QString method = body.mid(idStart, i - idStart);
        if (!method.startsWith(QLatin1String("to"))) continue;
        if (method == QLatin1String("toString")) continue;   // 文本旋钮另说
        while (i < body.size() && body[i].isSpace()) ++i;
        if (i >= body.size() || body[i] != QLatin1Char('(')) continue;
        const int argClose = matchBracket(body, i, QLatin1Char('('), QLatin1Char(')'));
        if (argClose < 0) continue;
        vr.expr = body.mid(i + 1, argClose - i - 1).trimmed();
        out.append(vr);
    }
    return out;
}

// 源文件里的键名清单常量:
//   const QStringList kCoordKeys = {"x1", "y1", "x2", "y2"};
static QMap<QString, QStringList> parseKeyLists(const QString& src) {
    QMap<QString, QStringList> out;
    const QRegularExpression re(
        QStringLiteral("const\\s+QStringList\\s+(\\w+)\\s*=\\s*\\{([^}]*)\\}"));
    auto it = re.globalMatch(src);
    while (it.hasNext()) {
        const auto m = it.next();
        QStringList keys;
        const QRegularExpression kRe(QStringLiteral("\"([^\"]+)\""));
        auto kit = kRe.globalMatch(m.captured(2));
        while (kit.hasNext())
            keys.append(kit.next().captured(1));
        if (!keys.isEmpty())
            out.insert(m.captured(1), keys);
    }
    return out;
}

// ============================================================
// 认出 helper 读法。返回:读到的键 + 审计自己认不出来的地方。
//
// 2026-10-21 第四轮扩到**两种**形式:
//
//   一组:readIntKnobs(params, kKeys, defaultParams(), &out, &err)
//   单个:readIntKnob(params, defaultParams(), "min_area", &out, &err)
//
// 为什么"单个"也要认:键名直接写在调用处,一眼看得见;
// "一组"那种把键名放进常量清单的,适合键多、类型一样的场合
// (五个测量算子的坐标、pcb_defect_judge 的 16 个判据阈值)。
// 两种形式审计都得认 —— 认不出就会判成"死旋钮",那是撒谎。
//
// ⚠ 解析方式:正则只用来**定位调用**,参数用 matchBracket/splitTopLevel 切。
//   因为参数里有括号(defaultParams())、有引号(键名),
//   用正则切参数切不准 —— 切错了就是"认不出",而"认不出"会撒谎。
//
// fallbackIssues 和 issues 分开,是两回事:
//   issues          —— 审计**认不出**(键名清单找不到、键名不是字面量)
//   fallbackIssues  —— 兜底**抄了第二份**(不是 defaultParams())
// 混在一起的话,报出来的理由会指错方向。
// ============================================================
static QSet<QString> parseHelperReads(const QString& body,
                                      const QMap<QString, QStringList>& keyLists,
                                      QStringList* issues,
                                      QStringList* fallbackIssues,
                                      QVector<FallbackRead>* fallbacks) {
    QSet<QString> keys;
    const QRegularExpression re(QStringLiteral("\\b(read\\w+Knobs?)\\s*\\("));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString fn = m.captured(1);
        const bool plural = fn.endsWith(QLatin1Char('s'));
        const int open = m.capturedEnd() - 1;          // '(' 的位置
        const int close = matchBracket(body, open, QLatin1Char('('), QLatin1Char(')'));
        if (close < 0) {
            issues->append(QString("认不出 %1 的参数(括号不配对)").arg(fn));
            continue;
        }
        const QStringList args = splitTopLevel(body.mid(open + 1, close - open - 1));
        if (args.size() < 3 || !args.first().startsWith(QLatin1String("params"))) {
            issues->append(QString("认不出 %1 的调用形式").arg(fn));
            continue;
        }

        QString defaults;
        QStringList readKeys;
        if (plural) {
            const QString listName = args.at(1);
            defaults = args.at(2);
            if (!keyLists.contains(listName)) {
                // 认不出键名清单 → **明说认不出**,不装作"这个旋钮没被读"
                issues->append(QString("审计认不出键名清单 %1(在 %2 里)")
                                   .arg(listName, fn));
                continue;
            }
            readKeys = keyLists.value(listName);
        } else {
            defaults = args.at(1);
            const QString keyLit = args.at(2);
            if (keyLit.length() < 2 || !keyLit.startsWith(QLatin1Char('"')) ||
                !keyLit.endsWith(QLatin1Char('"'))) {
                issues->append(QString("认不出 %1 的键名(不是字面量:%2)").arg(fn, keyLit));
                continue;
            }
            readKeys << keyLit.mid(1, keyLit.length() - 2);
        }

        // 键名先记上 —— 记不上就会被判成"死旋钮"。
        // 那是**另一种**错误(会把人引到完全错的方向),
        // 不能拿它盖住真正的问题。
        for (const QString& k : readKeys) keys.insert(k);

        // 兜底必须是 defaultParams() —— 那是声明值的唯一一份。
        // 传别的东西进来,就等于在 helper 外面又抄了一份默认值。
        if (!defaults.contains(QLatin1String("defaultParams"))) {
            fallbackIssues->append(QString("%1 的兜底不是 defaultParams()(是 %2)—— "
                                            "声明值只能有一份").arg(fn, defaults));
            continue;
        }
        if (fallbacks) {
            for (const QString& k : readKeys)
                fallbacks->append({k, defaults});
        }
    }
    return keys;
}

// 读一个文件(定义在下面"找源码目录"那一节)
static QString readAll(const QString& path, bool* ok);

// 算子名:从**同名头文件**的 name() 覆写里取。
//
// ⚠ 为什么不是从 .cpp 里取:
//   `QString name() const override { return "count_check"; }`
//   这行在 **.h** 里(header 里内联定义),.cpp 里根本没有。
//   第一版在 .cpp 里找,一个都没找到 —— 于是扫到的算子数是 0,
//   三个 lint 全部变成恒真的空断言,而测试还绿着。
//
// 读不到头文件时退回**文件名**(W7 的规矩是一个算子一个同名 .cpp)。
static QString parseOpName(const QString& cppPath) {
    QString hPath = cppPath;
    if (hPath.endsWith(QLatin1String(".cpp"))) hPath.chop(4);
    hPath += QLatin1String(".h");

    bool ok = false;
    const QString text = readAll(hPath, &ok);
    if (ok) {
        const QRegularExpression re(
            QStringLiteral("name\\s*\\(\\s*\\)\\s*const\\s*override\\s*\\{\\s*"
                           "return\\s*\"([^\"]+)\""));
        const auto m = re.match(stripComments(text));
        if (m.hasMatch()) return m.captured(1);
    }
    return QFileInfo(cppPath).completeBaseName();
}

// ============================================================
// 编译期停用算子的豁免清单
// ============================================================
// 每条都要写明:哪个算子、哪些旋钮、**为什么**它们在当前构建里读不到。
//
// 这份清单会互相校验:
//   · 漏登记 → 用例 1 报红
//   · 过期了(SDK 装上了,旋钮其实生效了)→ 用例 4 报红
// 所以清单不会自己烂掉。
// ============================================================
struct Exemption {
    const char* opName;
    QStringList keys;
    const char* reason;
};

static const Exemption kExemptions[] = {
    {"dl_infer",
     {"input_w", "input_h", "conf_threshold", "nms_iou", "class_names", "output_layout"},
     "本机没装 ONNX Runtime(环境变量 ONNXRUNTIME_DIR 未设置):"
     "真实推理路径整段在 #ifdef HAS_ONNXRUNTIME 里,没编进来;"
     "当前构建走的是空实现,它只读 model_path(用来区分"
     "\"路径写错了\"和\"SDK 没装\"两种病因)。"
     "装上 SDK 后这条豁免就过期了,审计会提醒删掉它。"},
};

// 某个算子的外部运行时在当前构建里是否可用
static bool runtimeAvailableFor(const QString& opName) {
    if (opName == QLatin1String("dl_infer"))
        return DlInfer::runtimeAvailable();
    if (opName == QLatin1String("image_source_camera"))
        return ImageSourceCamera::runtimeAvailable();
    return true;   // 其它算子没有外部运行时
}

static const Exemption* findExemption(const QString& opName, const QString& key) {
    for (const auto& e : kExemptions) {
        if (opName != QLatin1String(e.opName)) continue;
        if (e.keys.contains(key)) return &e;
    }
    return nullptr;
}

static const Exemption* findExemptionForOp(const QString& opName) {
    for (const auto& e : kExemptions)
        if (opName == QLatin1String(e.opName)) return &e;
    return nullptr;
}

// ============================================================
// 找源码目录
// ============================================================
#ifndef W7_SOURCE_DIR
#error "W7_SOURCE_DIR is not defined. The build system must tell the test where the sources are."
#endif

static QString operatorsDir() {
    return QString(W7_SOURCE_DIR) + "/operators";
}

static QString readAll(const QString& path, bool* ok) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { *ok = false; return {}; }
    *ok = true;
    return QString::fromUtf8(f.readAll());
}

// 解析一个算子源文件
static bool parseOperator(const QString& path, bool hasOnnx, bool hasMvs,
                          OperatorSource& op) {
    bool ok = false;
    const QString raw = readAll(path, &ok);
    if (!ok) return false;

    const QString noComment = stripComments(raw);
    // 供配对括号用的那一版:只把字符串里的花括号换成空格,
    // 字符串里的其它内容(key 名、算子名)全部保留。
    const QString scanSrc = blankBracesInStrings(noComment);

    // 当前构建真正会编进去的源码
    const QStringList lines = scanSrc.split(QLatin1Char('\n'));
    const QSet<int> live = liveLines(lines, hasOnnx, hasMvs);
    const QString liveSrc = maskDeadLines(scanSrc, live);

    // 所有分支的源码(分支判断只影响"哪支参与编译",这里两份都要看)
    const QStringList allLines = scanSrc.split(QLatin1Char('\n'));
    QSet<int> allLive;
    for (int i = 0; i < allLines.size(); ++i) allLive.insert(i + 1);
    const QString allSrc = maskDeadLines(scanSrc, allLive);

    const QStringList defs = functionBodies(liveSrc, QStringLiteral("defaultParams"));
    if (defs.isEmpty()) return false;      // 不是算子源文件

    op.file = QFileInfo(path).fileName();
    // ⚠ 算子名在**头文件**里(.h),不在 .cpp 里 —— 见 parseOpName 的说明。
    op.opName = parseOpName(path);
    if (op.opName.isEmpty()) return false;
    parseDefaults(defs.first(), op);

    for (const auto& body : functionBodies(liveSrc, QStringLiteral("run")))
        op.readsLive.unite(parseReads(body));
    for (const auto& body : functionBodies(allSrc, QStringLiteral("run")))
        op.readsAll.unite(parseReads(body));

    // 新读法(readIntKnob / readIntKnobs 之类):键名在常量里或字面量里,
    // 上面那套认不出来。认不出就会把 28 个坐标旋钮全判成"死旋钮" —— 那是撒谎。
    const QMap<QString, QStringList> keyLists = parseKeyLists(scanSrc);
    for (const auto& body : functionBodies(liveSrc, QStringLiteral("run")))
        op.readsLive.unite(parseHelperReads(body, keyLists, &op.helperIssues,
                                           &op.helperFallbackIssues, &op.helperFallbacks));
    for (const auto& body : functionBodies(allSrc, QStringLiteral("run")))
        op.readsAll.unite(parseHelperReads(body, keyLists, &op.helperIssues,
                                           &op.helperFallbackIssues, &op.helperFallbacks));
    op.helperIssues.removeDuplicates();
    op.helperFallbackIssues.removeDuplicates();

    // 兜底读法:**所有分支**都收。
    //
    // 为什么不是只收当前构建那一支:dl_infer 的 6 个旋钮在当前构建里
    // 读不到(整段在 #ifdef HAS_ONNXRUNTIME 里),但那一支的兜底
    // 是**装上 SDK 之后立刻生效**的代码 —— 那时候它要是抄了第二份,
    // 审计现在就该报出来,而不是等 SDK 装上才被人发现。
    for (const auto& body : functionBodies(allSrc, QStringLiteral("run")))
        op.fallbacks += parseFallbackReads(body);

    // 面板:buildUI() 的函数体 + 里面每个 setValue(...) 的参数
    const QStringList uiBodies = functionBodies(liveSrc, QStringLiteral("buildUI"));
    if (!uiBodies.isEmpty()) {
        op.buildUi = uiBodies.first();
        op.setValueArgs = parseSetValueArgs(op.buildUi);
    }
    // 面板把存盘里的值读回控件时用的那一档兜底(setParams)
    const QStringList spBodies = functionBodies(liveSrc, QStringLiteral("setParams"));
    if (!spBodies.isEmpty()) {
        op.setParamsBody = spBodies.first();
        op.setParamsReads = parseValueReads(op.setParamsBody);
    }
    return true;
}

// ============================================================
// 用例 0:审计本身没有空转
// ============================================================
//
// ⚠ 这条是**给自己**的守卫。
//
// 第一版审计有个致命问题:functionBodies() 认不出
// "QJsonObject Xxx::defaultParams() const {" 这种写法 ——
// 限定符 const 卡在参数表和 "{" 之间,第一版只跳空白,
// 于是**一个算子都没扫到**:三个 lint 全部变成恒真的空断言,
// 测试照样全绿,而审计**根本没在工作**。
//
// 这就是本项目记过的第 12 条教训:守卫写成"打不开就 return",
// 于是它变成一条恒真的空断言 —— 比没有守卫更坏,因为
// 它让人以为守卫在生效。
//
// 所以这里明确断言:扫到的算子数、扫到的旋钮数,都得是像样的数。
// 解析器一坏,这几条立刻变红 —— 而不是安静地什么都不查。
// ============================================================
static void test_audit_is_not_vacuous(const QVector<OperatorSource>& ops) {
    beginCase("0. 审计不是空转:确实扫到了算子和旋钮");

    check(ops.size() >= 15,
          QString("扫到至少 15 个算子源文件(实际 %1)—— "
                  "扫不到就是审计没在工作").arg(ops.size()));

    int total = 0;
    for (const auto& op : ops) total += op.declared.size();
    check(total >= 70,
          QString("一共扫到至少 70 个旋钮(实际 %1)—— "
                  "解析器坏了的话这个数会掉下去").arg(total));

    QStringList names;
    for (const auto& op : ops) names << op.opName;
    const char* must[] = {"pcb_inspect", "dl_infer", "circle_fit", "count_check",
                          "template_match", "color_segment", "image_source_camera",
                          "angle", "pcb_defect_judge"};
    for (const char* m : must) {
        const QString n = QString::fromUtf8(m);
        check(names.contains(n), QString("扫到了算子 %1").arg(n));
    }

    // 算子名必须和源文件名一致(一个算子一个同名 .cpp)。
    // 不一致说明取名取错了 —— 那时审计查的是"另一个名字"的旋钮,
    // 结果会**看起来正常**(有条数、有结论),但查的不是那回事。
    for (const auto& op : ops) {
        const QString base = QFileInfo(op.file).completeBaseName();
        check(op.opName == base,
              QString("%1 的算子名和文件名一致(实际算子名 %2)")
                  .arg(op.file, op.opName));
    }
}

// ============================================================
// 用例 1:死旋钮 —— 登记了,当前构建的 run() 却没读
// ============================================================
static void test_no_dead_knobs(const QVector<OperatorSource>& ops) {
    beginCase("1. 没有死旋钮:登记的每个旋钮都被 run() 读到");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    int exempted = 0;
    for (const auto& op : ops) {
        // 审计认不出这个算子的某种读法时,**不能**判成"死旋钮" ——
        // 那是撒谎,而且是"看起来很肯定"地撒谎:
        // 旋钮明明被读了,审计却说它没被读,会把人引到完全错误的方向。
        // 正确做法是**明说认不出**,让这条红在一个诚实的理由上。
        if (!op.helperIssues.isEmpty()) {
            check(false,
                  QString("%1: 审计认不出它的读法(%2)—— "
                          "这条不能判成死旋钮,得先把审计升级")
                      .arg(op.opName, op.helperIssues.join("; ")));
            continue;
        }
        for (const QString& key : op.declared) {
            if (op.readsLive.contains(key)) continue;
            const Exemption* ex = findExemption(op.opName, key);
            if (ex) {
                ++exempted;
                qInfo().noquote() << "  [豁免]" << op.opName << "." << key
                                  << " —— " << ex->reason;
                continue;
            }
            check(false,
                  QString("%1.%2 是死旋钮:defaultParams() 登记了它,"
                          "当前构建的 run() 却从来没读过它 —— "
                          "界面上摆着,调了不生效")
                      .arg(op.opName, key));
        }
    }
    check(true,
          QString("共 %1 个算子,%2 个旋钮落在显式豁免清单里"
                  "(豁免必须写明原因,见 kExemptions)")
              .arg(ops.size()).arg(exempted));
}

// ============================================================
// 用例 2:没登记的旋钮 —— run() 读了,defaultParams() 没登记
// ============================================================
static void test_no_undeclared_knobs(const QVector<OperatorSource>& ops) {
    beginCase("2. 没有没登记的旋钮:run() 读的每个 key 都登记过");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    for (const auto& op : ops) {
        for (const QString& key : op.readsAll) {
            if (op.declared.contains(key)) continue;
            check(false,
                  QString("%1.%2 被 run() 读了,但 defaultParams() 没登记 —— "
                          "写进流水线的参数 JSON 里压根没有这个 key,"
                          "它的默认值只活在 run() 的兜底里")
                      .arg(op.opName, key));
        }
    }
    check(true, "所有算子的 run() 读到的 key 都在 defaultParams() 里登记过");
}

// ============================================================
// 用例 3:默认值只能有一份
// ============================================================
static bool exprHasBareLiteral(const QString& expr) {
    // 去掉所有标识符之后剩下的就是标点/数字/引号。
    // 剩下里有数字 → 有裸数字字面量;有引号 → 有裸字符串字面量。
    //
    // (下标也一并抹掉 —— lo[0] 里的 0 是下标,不是值。理由见
    //  exprWithoutSubscripts 那段。)
    return exprHasBareNumber(expr) || exprHasBareString(expr);
}

static void test_defaults_have_single_source(const QVector<OperatorSource>& ops) {
    beginCase("3. 默认值只有一份:defaultParams() 里不许出现裸字面量");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    // 3a:不许有裸数字/裸字符串字面量
    for (const auto& op : ops) {
        for (const auto& kv : op.defaultExprs) {
            if (exprHasBareLiteral(kv.second)) {
                check(false,
                      QString("%1.%2 的默认值写的是裸字面量(%3) —— "
                              "它得是 operator_defaults.h 里的那个常量,"
                              "不然又是一份抄副本")
                          .arg(op.opName, kv.first, kv.second));
            }
        }
    }
    check(true, "defaultParams() 里没有裸字面量(全部来自具名常量)");

    // 3b:引用的 Defaults 常量必须真的在 operator_defaults.h 里
    //     —— 否则就是在别处又抄了一份 Defaults 结构体
    bool ok = false;
    const QString header = readAll(operatorsDir() + "/operator_defaults.h", &ok);
    check(ok, "能读到 operators/operator_defaults.h");
    if (!ok) return;

    // 扫出 operator_defaults.h 里定义的每一个 `XDefaults::kY`
    QSet<QString> known;
    {
        const QRegularExpression structRe(
            QStringLiteral("struct\\s+(\\w+Defaults)\\s*\\{"));
        auto it = structRe.globalMatch(header);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString name = m.captured(1);
            const int open = m.capturedEnd() - 1;
            const int close = matchBracket(header, open, QLatin1Char('{'), QLatin1Char('}'));
            if (close < 0) continue;
            const QString body = header.mid(open, close - open + 1);
            const QRegularExpression memberRe(QStringLiteral("\\b(k\\w+)\\s*="));
            auto mit = memberRe.globalMatch(body);
            while (mit.hasNext())
                known.insert(name + "::" + mit.next().captured(1));
        }
    }
    check(!known.isEmpty(),
          QString("operator_defaults.h 里读到 %1 个 Defaults 常量").arg(known.size()));

    const QRegularExpression useRe(QStringLiteral("\\b(\\w+Defaults)::(k\\w+)\\b"));
    for (const auto& op : ops) {
        for (const auto& kv : op.defaultExprs) {
            auto uit = useRe.globalMatch(kv.second);
            while (uit.hasNext()) {
                const auto um = uit.next();
                const QString ref = um.captured(1) + "::" + um.captured(2);
                check(known.contains(ref),
                      QString("%1.%2 引用的 %3 确实定义在 operator_defaults.h 里")
                          .arg(op.opName, kv.first, ref));
            }
        }
    }

    // 3i:引用的 Defaults 必须是**本算子**的
    //
    // 为什么这条是第四轮才补上的:
    //   第三轮之前,run() 里的兜底写的是 `XxxDefaults::kY`,
    //   3d 那条查的就是"它是不是本算子的 Defaults" ——
    //   拿别的算子的默认值来顶自己的旋钮,那也是抄副本,
    //   而且比抄成字面量更隐蔽:它看着像"引用了常量",其实引用错了。
    //
    //   第四轮把旋钮全迁到 readXxxKnob 之后,run() 里的兜底
    //   统一变成 defaultParams() —— 3d 查不到 Defaults 引用了,
    //   **那条检查就跟着消失了**。
    //
    //   ⚠ 删掉一条断言 = 少一层保险,而且没人会注意到:
    //   测试还是绿的,只是少查了一件事。
    //   所以这条检查要**搬家** —— 从 run() 的兜底搬到 defaultParams() 上:
    //   声明才是兜底真正指向的地方,兜底现在只是指着它。
    const QRegularExpression ownRe(QStringLiteral("\\b(\\w+Defaults)::(k\\w+)\\b"));
    for (const auto& op : ops) {
        const QString own = ownDefaultsStructName(QFileInfo(op.file).completeBaseName());
        for (const auto& kv : op.defaultExprs) {
            auto uit = ownRe.globalMatch(kv.second);
            while (uit.hasNext()) {
                const auto um = uit.next();
                const QString st = um.captured(1);
                check(st == own,
                      QString("%1.%2 的默认值引用了 %3::%4 —— 那是**别的算子**的默认值,"
                              "本算子该用 %5::(自己的那一份)")
                          .arg(op.opName, kv.first, st, um.captured(2), own));
            }
        }
    }

    // 3c:整个 operators/ 目录下,只有 operator_defaults.h 能定义 Defaults 结构体
    QDir dir(operatorsDir());
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString& f : files) {
        if (!f.endsWith(".cpp") && !f.endsWith(".h")) continue;
        if (f == "operator_defaults.h") continue;
        bool fok = false;
        const QString text = readAll(dir.filePath(f), &fok);
        if (!fok) continue;
        const QRegularExpression structRe(QStringLiteral("struct\\s+(\\w+Defaults)\\b"));
        const auto m = structRe.match(stripComments(text));
        if (m.hasMatch()) {
            check(false,
                  QString("%1 里也定义了一个 Defaults 结构体 —— "
                          "旋钮默认值只能有一份,别处再抄一个就又漂了").arg(f));
        }
    }
    check(true, "Defaults 结构体只在 operator_defaults.h 里定义");
}

// ============================================================
// operator_defaults.h 的常量清单(只读一次)
// ============================================================
static QVector<DefaultsMember> g_defaultsMembers;
static bool g_defaultsMembersLoaded = false;

static const QVector<DefaultsMember>& defaultsMembers() {
    if (!g_defaultsMembersLoaded) {
        g_defaultsMembersLoaded = true;
        bool ok = false;
        const QString header = readAll(operatorsDir() + "/operator_defaults.h", &ok);
        if (ok) g_defaultsMembers = parseDefaultsMembers(header);
    }
    return g_defaultsMembers;
}

// ============================================================
// 用例 3d:兜底不许抄第二份
// ============================================================
//
// 欠账 1 的正身:以前这条审计只看 defaultParams() ——
// 兜底(run() 里读不到 key 时用的值)是**第二份**,它抄了没人管。
//
// 这条查两件事:
//   (a) 兜底表达式里不许有裸字面量 —— 必须指向具名常量
//   (b) 引用的 Defaults 必须是**本算子**的结构体 ——
//       拿别的算子的默认值来顶自己的旋钮,那也是抄副本,
//       而且比抄成字面量更隐蔽:它看着像"引用了常量",其实引用错了。
//
// ⚠ 这条查的是**所有分支**的 run(),不只当前构建那一支。
//   dl_infer 那 6 个旋钮在当前构建里读不到(整段在 #ifdef 里),
//   但那一支的兜底是装上 SDK 之后立刻生效的代码 ——
//   那时候它要是抄了第二份,现在就该报出来。
//
// ⚠ 这条**查不出**兜底表达式外面还套了一层运算的情况:
//   image_source_video 写的是
//       std::max(1, params.value("frame_step").toInt(...))
//   那个 1 在 toInt 的参数外面,看不见。
//   那一层是**安全下限**(防 frame_step=0 把帧循环卡死),
//   不是默认值的副本 —— 但它确实让"声明值"和"实际兜底值"
//   在运行时才相等,而不是在源码里就相等。记在欠账里,没消掉。
//
// ⚠ 第四轮起,这条要查**两种**兜底写法:
//   旧写法  params.value("x").toInt(XxxDefaults::kX)
//   新写法  readIntKnob(params, defaultParams(), "x", &v, &err)
// 全迁完之后旧写法会一个不剩 —— 那时候这条要是还只查旧写法,
// 它就变成**恒真的空断言**了(和第一轮那个坑一模一样)。
// 所以"查到的条数 > 0"那个守卫数的是**两种加起来**。
// ============================================================
static void test_fallbacks_have_single_source(const QVector<OperatorSource>& ops) {
    beginCase("3d. 兜底不许抄第二份:run() 里的兜底值必须来自同一份常量");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    int total = 0;
    for (const auto& op : ops) {
        const QString own = ownDefaultsStructName(QFileInfo(op.file).completeBaseName());

        // 旧写法:params.value("x").toXxx(兜底)
        for (const auto& fb : op.fallbacks) {
            ++total;
            if (exprHasBareNumber(fb.expr) || exprHasBareString(fb.expr)) {
                check(false,
                      QString("%1.%2 的兜底写的是裸字面量(%3) —— "
                              "它得是 operator_defaults.h 里的那个常量,"
                              "不然 run() 用的值和 defaultParams() 声明的值就不是同一个数")
                          .arg(op.opName, fb.key, fb.expr));
                continue;
            }
            const QRegularExpression useRe(QStringLiteral("\\b(\\w+Defaults)::(k\\w+)\\b"));
            auto uit = useRe.globalMatch(fb.expr);
            while (uit.hasNext()) {
                const auto um = uit.next();
                const QString st = um.captured(1);
                check(st == own,
                      QString("%1.%2 的兜底引用了 %3::%4 —— 那是**别的算子**的默认值,"
                              "本算子该用 %5")
                          .arg(op.opName, fb.key, st, um.captured(2), own));
            }
        }

        // 新写法:readXxxKnob(s)(params, <兜底>, ...)
        //
        // 兜底必须来自 defaultParams() —— 传别的东西进来,
        // 就等于在 helper 外面又抄了一份默认值。
        for (const QString& issue : op.helperFallbackIssues) {
            ++total;
            check(false, QString("%1: %2").arg(op.opName, issue));
        }
        for (const auto& fb : op.helperFallbacks) {
            ++total;
            if (exprHasBareNumber(fb.expr) || exprHasBareString(fb.expr)) {
                check(false,
                      QString("%1.%2 的兜底写的是裸字面量(%3) —— "
                              "它得是 operator_defaults.h 里的那个常量")
                          .arg(op.opName, fb.key, fb.expr));
                continue;
            }
            const QRegularExpression useRe(QStringLiteral("\\b(\\w+Defaults)::(k\\w+)\\b"));
            auto uit = useRe.globalMatch(fb.expr);
            while (uit.hasNext()) {
                const auto um = uit.next();
                const QString st = um.captured(1);
                check(st == own,
                      QString("%1.%2 的兜底引用了 %3::%4 —— 那是**别的算子**的默认值,"
                              "本算子该用 %5")
                          .arg(op.opName, fb.key, st, um.captured(2), own));
            }
        }
    }
    check(total > 0,
          QString("一共检查了 %1 处兜底读法(要是 0,说明解析器没认出这种写法,"
                  "这条就成了恒真的空断言)").arg(total));
}

// ============================================================
// 用例 3e:面板初值不许抄第二份
// ============================================================
//
// 同一个默认值抄在三个地方:defaultParams()、run() 兜底、面板 buildUI()。
// 3a 只查了第一个,3d 查了第二个,这条查第三个。
//
// 为什么只查 setValue:面板上别的数字(setRange 的上下界、setSingleStep 的步长、
// setDecimals 的小数位)不是旋钮的**值**,是控件的量程 ——
// 把它们也禁掉,审计第一天就会误报几十条,然后被人关掉。
//
// 而 setValue(...) 的参数就是"界面上初值那份",按定义它是旋钮的值。
//
// ⚠ 这条只看 buildUI(),不看 setParams():
//   setParams 里是"把已存的参数读回界面",那里的默认值来自
//   defaultParams() 或 run() 的兜底,已经在 3a/3d 的射程内。
//   而且 setParams 里的 toInt(0) 那种"类型不对就退成 0"的写法
//   属于另一类问题(静默把坏值变成好值),不是副本问题,另记。
// ============================================================
static void test_panel_initials_have_single_source(const QVector<OperatorSource>& ops) {
    beginCase("3e. 面板初值不许抄第二份:buildUI() 的 setValue 不许有裸数字");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    int panels = 0, total = 0;
    for (const auto& op : ops) {
        if (op.buildUi.isEmpty()) continue;    // 这个算子没有面板
        ++panels;
        for (const QString& arg : op.setValueArgs) {
            ++total;
            if (exprHasBareNumber(arg)) {
                check(false,
                      QString("%1 的面板初值写的是裸数字(%2) —— "
                              "它得是具名常量,不然面板上写着的和算子实际跑的不是同一个数")
                          .arg(op.opName, arg));
            }
        }
    }
    check(panels > 0,
          QString("扫到了 %1 个有参数面板的算子").arg(panels));
    check(total > 0,
          QString("一共检查了 %1 处面板 setValue 初值(要是 0,说明解析器没认出"
                  "buildUI,这条就成了恒真的空断言)").arg(total));
}

// ============================================================
// 用例 3f:字符串型默认值得露在面板上
// ============================================================
//
// 3d/3e 盯的是**数字**。字符串型默认值有它自己的一套病:
//
//   面板上写死一份字面量(defaultParams 写 "known_site",
//   面板 addItem 也写 "known_site"),或者面板压根没给初值
//   (QLineEdit 初始是空的,collect() 把空串写回 params,
//   于是 defaultParams 里声明的那个默认值**被吃掉了**)。
//
// 这两种都是"同一个值的两份",所以这条查:
//   defaultParams() 里引用的是**字符串型**常量时,
//   面板 buildUI() 必须引用**同一个**常量。
//
// 为什么只管字符串型:数字型的初值 3e 已经管了(不许裸数字),
// 而数值型常量在面板上通常表现为 setValue(常量) ——
// 3f 对它们没有增量价值,只会让规则变复杂。
//
// ⚠ 这条的边界:它只要求常量**出现在** buildUI 里,
//   不要求它被用作了控件初值。
//   dl_infer 的 model_path 目前只出现在 setPlaceholderText 里 ——
//   那不算真初值(那一栏初始是空的)。
//   它实际上没事,是因为 dl_infer 的 setParams 用的是
//   params.value(key).toString(默认值) 而不是 params.contains(key) ——
//   key 缺了也会退回默认值。但这条**保证不了**那种写法。
// ============================================================
static void test_string_defaults_reach_the_panel(const QVector<OperatorSource>& ops) {
    beginCase("3f. 字符串型默认值得露在面板上:buildUI 必须引用同一个常量");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    const QVector<DefaultsMember>& members = defaultsMembers();
    check(!members.isEmpty(),
          QString("读到 %1 个 Defaults 常量(带类型)").arg(members.size()));
    if (members.isEmpty()) return;

    int checked = 0;
    for (const auto& op : ops) {
        if (op.buildUi.isEmpty()) continue;    // 没有面板,谈不上初值
        for (const auto& kv : op.defaultExprs) {
            const QRegularExpression useRe(QStringLiteral("\\b(\\w+Defaults)::(k\\w+)\\b"));
            auto uit = useRe.globalMatch(kv.second);
            while (uit.hasNext()) {
                const auto um = uit.next();
                const QString st = um.captured(1);
                const QString mem = um.captured(2);
                bool isString = false;
                for (const auto& dm : members)
                    if (dm.structName == st && dm.memberName == mem) isString = dm.isString;
                if (!isString) continue;       // 数字型的交给 3e
                ++checked;
                check(op.buildUi.contains(st + "::" + mem),
                      QString("%1.%2 的默认值是 %3::%4 —— 面板 buildUI() 里也得引用同一个常量"
                              "(现在是另一份字面量,或者压根没给初值)")
                          .arg(op.opName, kv.first, st, mem));
            }
        }
    }
    check(checked > 0,
          QString("一共核对了 %1 个字符串型默认值(要是 0,说明解析器没认出写法,"
                  "这条就成了恒真的空断言)").arg(checked));
}

// ============================================================
// 用例 3g:面板读回时的兜底也不许是另一份
// ============================================================
//
// 3e 查的是 buildUI() 的**初值**;这条查 setParams() 的**兜底** ——
// 那是面板把存盘里的值读回控件时用的那一档。
//
// 为什么它也是"默认值的一份":
//   存盘里某个旋钮存成了字符串(手改的、老存档、脚本写坏的)时,
//   控件总得显示一个数。显示什么?
//     · 显示 0      → 和声明值不是一回事(0 几乎从来不是声明值)
//     · 显示声明值  → 面板上摆的和算子跑的是同一个数 ✓
//
// ⚠ 特别要抓 **toInt() / toDouble() 不带参数**:
//   看着像"没写兜底",其实是 Qt 的默认参数 0 ——
//   它是**隐式**的,源码里看不见那个 0,lint 也看不见。
//   实测这条抓到了 4 处:blob_count / count_check(两处)/
//   image_source_camera / template_match —— 全是"隐式退成 0"。
// ============================================================
static void test_setparams_fallbacks_have_single_source(
        const QVector<OperatorSource>& ops) {
    beginCase("3g. 面板读回时的兜底也不许是另一份:setParams() 里的 toInt/toDouble");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    int panels = 0, total = 0;
    for (const auto& op : ops) {
        if (op.setParamsBody.isEmpty()) continue;
        ++panels;
        for (const auto& vr : op.setParamsReads) {
            ++total;
            if (vr.expr.isEmpty()) {
                check(false,
                      QString("%1 的 setParams() 里 %2 没有兜底 —— "
                              "toInt()/toDouble() 不带参数是**隐式退成 0**,"
                              "那和声明的默认值不是一回事(面板上显示 0,"
                              "用户点一下\"应用参数\"就把 0 存进流水线了)")
                          .arg(op.opName, vr.keyExpr));
            } else if (exprHasBareNumber(vr.expr)) {
                check(false,
                      QString("%1 的 setParams() 里 %2 的兜底写的是裸数字(%3) —— "
                              "它得是声明的默认值那个常量")
                          .arg(op.opName, vr.keyExpr, vr.expr));
            }
        }
    }
    check(panels > 0,
          QString("扫到了 %1 个有参数面板的算子").arg(panels));
    check(total > 0,
          QString("一共检查了 %1 处 setParams 兜底(要是 0,说明解析器没认出"
                  "这种写法,这条就成了恒真的空断言)").arg(total));
}

// ============================================================
// 用例 3h:兜底值不许是"从库里现抄的常量"
// ============================================================
//
// 3d/3g 查的是"兜底值是不是裸字面量",**查不出这一种**:
//
//     params.value("method").toInt(cv::TM_CCOEFF_NORMED)
//
// cv::TM_CCOEFF_NORMED 不是字面量,3d/3g 看不见它;
// 但它**是另一份默认值** —— "默认用哪个匹配方法"是**本算子自己的决定**,
// 那个决定写在算子文件里,就等于声明值有了第二份。
//
// 实测(2026-10-21):template_match 的匹配方法默认值就抄了三处 ——
// defaultParams()、run() 的兜底、setParams() 的兜底。
// 现在收进 operator_defaults.h 的 TemplateMatchDefaults::kMethod。
//
// ⚠ 这条只查 cv:: 前缀,**故意窄**:
//   · pcbcolor::kDarkMul 这类**共享**常量不算抄副本 ——
//     common/pcb_core/ 是 W6/W7 共用的唯一一份
//     (见 operator_defaults.h 开头的"边界"那一节)。
//   · 规则宁窄勿宽:误报的守卫会被下一个人关掉。
//
// ⚠ 这条也查不出"文件里自己 define 一个 constexpr int kXxx = 3"
//   这种本地常量当默认值 —— 那是另一类抄副本,还没查。
//   记在欠账里,没假装查到了。
// ============================================================
static void test_fallbacks_not_copied_from_library(const QVector<OperatorSource>& ops) {
    beginCase("3h. 兜底值不许是从库里现抄的常量(cv::)");

    if (ops.isEmpty()) {
        check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
        return;
    }

    int total = 0;
    for (const auto& op : ops) {
        for (const auto& fb : op.fallbacks) {
            ++total;
            if (fb.expr.contains(QLatin1String("cv::"))) {
                check(false,
                      QString("%1.%2 的 run() 兜底直接引用了库里的常量(%3) —— "
                              "它得是 operator_defaults.h 里声明的那个具名常量,"
                              "不然\"默认用哪个\"这个决定在算子文件里又有一份")
                          .arg(op.opName, fb.key, fb.expr));
            }
        }
        for (const auto& vr : op.setParamsReads) {
            if (vr.expr.isEmpty()) continue;   // 没兜底是 3g 的事,这条不重复管
            ++total;
            if (vr.expr.contains(QLatin1String("cv::"))) {
                check(false,
                      QString("%1 的 setParams() 里 %2 的兜底直接引用了库里的常量(%3) —— "
                              "它得是 operator_defaults.h 里声明的那个具名常量")
                          .arg(op.opName, vr.keyExpr, vr.expr));
            }
        }
    }
    check(total > 0,
          QString("一共检查了 %1 处兜底(要是 0,说明解析器没认出这种写法,"
                  "这条就成了恒真的空断言)").arg(total));
}

// ============================================================
// 用例 4:豁免清单不能过期
// ============================================================
static void test_exemptions_not_stale() {
    beginCase("4. 豁免清单不过期:装上 SDK 的旋钮不许还挂在豁免里");

    for (const auto& e : kExemptions) {
        const QString opName = QString::fromUtf8(e.opName);
        const bool available = runtimeAvailableFor(opName);
        check(!available,
              QString("%1 的运行时当前确实不可用(豁免成立)").arg(opName));
        if (available) {
            check(false,
                  QString("%1 的运行时现在已经可用了 —— 这条豁免过期了,"
                          "请把 kExemptions 里对应的那条删掉。"
                          "留着它 = 以后有人新加旋钮时,这个算子会被"
                          "\"豁免\"掉而不报红").arg(opName));
        }
        check(!QString::fromUtf8(e.reason).isEmpty() &&
              QString::fromUtf8(e.reason).length() > 10,
              QString("%1 的豁免写明了原因").arg(opName));
    }
}

// ============================================================
// 用例 5:行为等价 —— 空参数 vs 声明的默认参数
// ============================================================
static bool jsonValueEqual(const QJsonValue& a, const QJsonValue& b) {
    if (a.isDouble() && b.isDouble()) {
        const double x = a.toDouble(), y = b.toDouble();
        // ⚠ 这里不能写 std::max —— windows.h 把 max 定义成了宏,
        //   预处理会把 std::max(1.0, ...) 撕成 std::((1.0) > ... ? ...),
        //   编译报 "'(' 右边非法标记 ::"。踩过。
        const double ax = std::fabs(x);
        const double scale = ax > 1.0 ? ax : 1.0;
        return std::fabs(x - y) <= 1e-9 * scale;
    }
    return a == b;
}

struct Probe {
    const char* opName;
    QJsonObject inputs;
    QStringList keys;      // 逐值比较的输出键
    // 必填旋钮:两次运行都带上它。
    //
    // 为什么需要:有些旋钮没有"可信的默认值"可退 —— 缺了就必须报错,
    // 而不是悄悄换一个数(见下面的"必填旋钮"说明)。
    // 探针只比"可选旋钮"的兜底对不对,必填的那些每次都给全。
    QJsonObject extra;
};

// ============================================================
// 必填旋钮 —— 缺了必须报错,不猜默认值
// ============================================================
//
// 有些旋钮没有"声明的默认值"可退:
//
//   color_segment 的 hsv_low / hsv_high
//     那两个区间**就是**用户要调的东西。没有它们等于没配置,
//     补一个默认值会让用户以为自己的配置生效了 ——
//     那是 hsv_params.h 开头警告的"配置看着对、结果全错"。
//     (test_detection_operators.cpp 有测试钉着:"参数完全为空 -> 报 error")
//
//   pcb_inspect 的四个金层标量
//     同理。(test_pcb_operators.cpp 钉着:"H 上界缺失 -> 报错")
//
// 别的旋钮都是**可选**的:缺了就退回 operator_defaults.h 里声明的默认值。
//
// 两种处理必须分开,混成一种会出两种错:
//   · 全当必填 → 少个 key 整条算子罢工(明明有声明过的默认值)
//   · 全当可选 → 用户填错的值被悄悄换成默认值(配置看着对、结果全错)
//
// ⚠ 这份清单是**测试这边对"必填"的登记**,不是代码里的声明 ——
//   代码那边表现为"缺了就报错",审计这边靠"两次运行的成败一致"来验。
// ============================================================
static QJsonObject requiredKnobs(const QString& opName) {
    if (opName == QLatin1String("color_segment")) {
        return QJsonObject{{"hsv_low", QJsonArray{10, 80, 80}},
                           {"hsv_high", QJsonArray{40, 255, 255}}};
    }
    if (opName == QLatin1String("pcb_inspect")) {
        return QJsonObject{{"hsv_h_low", 85}, {"hsv_h_high", 115},
                           {"hsv_s_low", 80}, {"mask_v_mul", 0.8}};
    }
    return {};
}

static void test_empty_params_match_defaults(const QString& maskPath,
                                              const QString& imgPath,
                                              const QString& boardPath) {
    beginCase("5. 空参数跑出来的结果 == 声明的默认参数跑出来的结果");

    QVector<Probe> probes;

    // count_check:输出里把两个阈值原样吐出来 → 这条**能**分辨兜底值
    probes.append({"count_check", QJsonObject{{"count", 5}},
                   {"result", "actual", "expected_min", "expected_max"}, {}});

    // blob_count:一个面积 30 的小块。声明的 min_area=20 → 算 1 个;
    // 旧兜底 50 → 算 0 个。**能**分辨。
    probes.append({"blob_count", QJsonObject{{"mask_path", maskPath}},
                   {"count", "total_area"}, {}});

    // circle_fit:方形块,圆度 0.79、面积 30 → 会被 min_area 滤掉,
    // 于是输出里带上"最小面积 / 最小圆度"那句 warning → **能**分辨。
    probes.append({"circle_fit", QJsonObject{{"mask_path", maskPath}},
                   {"count", "warning"}, {}});

    // 测量类:坐标全是声明的默认值,空参数时兜底必须是同一批数
    probes.append({"line_fit",         QJsonObject{}, {"length", "angle_deg"}, {}});
    probes.append({"point_point_dist", QJsonObject{}, {"distance"}, {}});
    probes.append({"point_line_dist",  QJsonObject{}, {"distance"}, {}});
    probes.append({"caliper_dist",     QJsonObject{}, {"distance"}, {}});
    probes.append({"angle",            QJsonObject{}, {"angle_deg"}, {}});

    // pcb_inspect 真值模式:配方路径的兜底要是对了,才找得到元件。
    probes.append({"pcb_inspect", QJsonObject{{"image_path", boardPath}},
                   {"count", "mode"}, {}});

    // pcb_inspect 自动轮廓模式:min_area_pct 只在这条分支上起作用,
    // 不切过来就抓不到它的兜底漂移。**能**分辨。
    probes.append({"pcb_inspect", QJsonObject{{"image_path", boardPath}},
                   {"count", "mode"}, {{"mode", "auto_contour"}}});

    // color_segment:HSV 区间是必填的(两次运行都给全),
    // 这条比的是"分割结果一样"。
    probes.append({"color_segment", QJsonObject{{"image_path", imgPath}},
                   {"pixel_count"}, {}});

    // 下面这几个:输出里看不出阈值差别,这条只能钉住"两次运行一致"。
    // 具体阈值由用例 6 的真值表钉。
    probes.append({"template_match",
                   QJsonObject{{"image_path", imgPath}, {"template_path", imgPath}},
                   {"score", "pass"}, {}});
    probes.append({"dl_infer",         QJsonObject{}, {}, {}});
    probes.append({"image_source_camera",   QJsonObject{}, {}, {}});
    probes.append({"image_source_dir",      QJsonObject{}, {}, {}});
    probes.append({"image_source_video",    QJsonObject{}, {}, {}});
    probes.append({"pcb_defect_judge",      QJsonObject{}, {}, {}});
    probes.append({"qr_decode",             QJsonObject{{"image_path", imgPath}}, {}, {}});

    for (const auto& p : probes) {
        const QString opName = QString::fromUtf8(p.opName);
        OperatorBase* op = createOperatorByName(opName);
        if (!op) {
            check(false, QString("%1 造不出来(createOperatorByName 返回空)").arg(opName));
            continue;
        }
        // 两次运行都带上:必填旋钮 + 这条探针额外指定的参数
        QJsonObject given = requiredKnobs(opName);
        for (auto it = p.extra.begin(); it != p.extra.end(); ++it)
            given[it.key()] = it.value();

        // 第一次:声明的默认参数(+ 必填 + 额外)
        // 第二次:只有必填 + 额外(其余全靠 run() 里的兜底)
        QJsonObject merged = op->defaultParams();
        for (auto it = given.begin(); it != given.end(); ++it)
            merged[it.key()] = it.value();

        const QJsonObject a = op->run(merged, p.inputs);
        const QJsonObject b = op->run(given, p.inputs);
        delete op;

        for (const QString& k : p.keys) {
            check(jsonValueEqual(a.value(k), b.value(k)),
                  QString("%1 的输出 \"%2\" 在「声明的默认参数」和「只给必填项」下一样"
                          "(默认 %3,只给必填 %4)")
                      .arg(opName, k,
                           a.value(k).toVariant().toString(),
                           b.value(k).toVariant().toString()));
        }
        // 两次运行"有没有出错"必须一致:
        // 一边报 error 一边不报,说明兜底把行为改掉了。
        check(a.contains("error") == b.contains("error"),
              QString("%1 两次运行的成败一致(默认参数:%2,只给必填:%3)")
                  .arg(opName,
                       a.contains("error") ? "报错" : "正常",
                       b.contains("error") ? "报错" : "正常"));
    }
}

// ============================================================
// 用例 6:真值表 —— 关键默认值钉死
// ============================================================
//
// 这一条是**基准不来自被测对象**:期望值写在这里,
// 不是"算子现在返回什么就期望什么"。
// 改默认值必须同时改这里 —— 这是有意的摩擦。
// ============================================================
struct Truth {
    const char* opName;
    const char* key;
    QJsonValue expected;
};

static void test_default_value_truth_table() {
    beginCase("6. 关键默认值符合真值表");

    const QVector<Truth> truths = {
        {"blob_count",           "min_area",       20},
        {"count_check",          "expected_min",   1},
        {"count_check",          "expected_max",   100},
        {"circle_fit",           "min_area",       100},
        {"circle_fit",           "min_circularity", 0.70},
        {"template_match",       "threshold",      0.9},
        {"dl_infer",             "conf_threshold", 0.25},
        {"dl_infer",             "nms_iou",        0.45},
        {"dl_infer",             "input_w",        640},
        {"dl_infer",             "input_h",        640},
        {"image_source_camera",  "exposure",       10.0},
        {"image_source_video",   "max_frames",     30},
        {"image_source_video",   "frame_step",     1},
        {"pcb_inspect",          "min_area_pct",   0.05},
        {"pcb_inspect",          "mode",           "known_site"},
        {"image_source_dir",     "pattern",        "*.png"},
    };

    for (const auto& t : truths) {
        OperatorBase* op = createOperatorByName(QString::fromUtf8(t.opName));
        if (!op) {
            check(false, QString("%1 造不出来").arg(t.opName));
            continue;
        }
        const QJsonObject d = op->defaultParams();
        delete op;
        const QString key = QString::fromUtf8(t.key);
        check(d.contains(key),
              QString("%1.defaultParams() 里有 %2").arg(t.opName, key));
        if (!d.contains(key)) continue;
        check(jsonValueEqual(d.value(key), t.expected),
              QString("%1.%2 = %3(真值表期望 %4)")
                  .arg(t.opName, key,
                       d.value(key).toVariant().toString(),
                       t.expected.toVariant().toString()));
    }

    // color_segment 的两个 HSV 数组
    {
        OperatorBase* op = createOperatorByName("color_segment");
        if (op) {
            const QJsonObject d = op->defaultParams();
            delete op;
            check(d.value("hsv_low").toArray() == QJsonArray({10, 80, 80}),
                  "color_segment.hsv_low = [10,80,80](真值表)");
            check(d.value("hsv_high").toArray() == QJsonArray({40, 255, 255}),
                  "color_segment.hsv_high = [40,255,255](真值表)");
        }
    }
}

// ============================================================
// 用例 7:审计扫的源码就是主程序真在编译的源码
// ============================================================
//
// 欠账 6:审计扫的是**源码**,不是编译产物。
// 如果以后有人用脚本生成 .cpp,或者把算子挪到别的目录,
// 审计会扫不到 —— 用例 0 会报红(算子数掉下去),但不会告诉你为什么。
//
// 这条把"为什么"补上:审计扫的每一个 operators/*.cpp,
// 都必须真的在主程序的 SOURCES 里;SOURCES 里列的每一个,
// 都必须真的在磁盘上。两边对不上,就是"审计查的和产品跑的不是同一份代码"。
//
// 为什么值得单列一条:一个查不到问题的审计比没有审计更坏,
// 因为它让人以为有人看着。
//
// ⚠ 这条查的是**文件清单**,查不了"编译产物里有没有这段代码"。
//   真的要看编译产物,得读 .obj / 符号表 —— 那在 Windows + MSVC
//   下要额外的工具链,不值当。文件清单 + 用例 0 的算子数下限,
//   已经是本项目能负担的强度。
// ============================================================

// CMake 的注释是 # 到行尾,和 C++ 的 // /* */ 都不是一套。
//
// 为什么必须单独处理:CMakeLists.txt 的注释里写着
// "进 SOURCES 而不是只在测试目标里链"这类说明,
// 按 C++ 那套切会把真条目也切掉;更糟的是 ——
// 注释里如果出现 "operators/xxx.cpp",那是**说明**,不是真的在编译,
// 审计不能把它当数。
static QString stripCMakeComments(const QString& src) {
    QString out;
    out.reserve(src.size());
    bool inStr = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) {
                out += c;
                if (i + 1 < src.size()) out += src[++i];
                continue;
            }
            if (c == QLatin1Char('"')) inStr = false;
            out += c;
            continue;
        }
        if (c == QLatin1Char('"')) { inStr = true; out += c; continue; }
        if (c == QLatin1Char('#')) {
            while (i < src.size() && src[i] != QLatin1Char('\n')) ++i;
            if (i < src.size()) out += QLatin1Char('\n');
            continue;
        }
        out += c;
    }
    return out;
}

// 取出 set(<变量> ...) 的那一段内容
static QString extractCMakeSet(const QString& src, const QString& varName) {
    const int at = src.indexOf(QStringLiteral("set(") + varName);
    if (at < 0) return {};
    const int open = src.indexOf(QLatin1Char('('), at);
    if (open < 0) return {};
    const int close = matchBracket(src, open, QLatin1Char('('), QLatin1Char(')'));
    if (close < 0) return {};
    return src.mid(open + 1, close - open - 1);
}

static void test_audited_sources_are_the_compiled_ones() {
    beginCase("7. 审计扫的源码就是主程序真在编译的源码");

    bool ok = false;
    const QString cmake = readAll(QString(W7_SOURCE_DIR) + "/CMakeLists.txt", &ok);
    check(ok, "能读到 W7_VisionMasterLite/CMakeLists.txt");
    if (!ok) return;

    const QString srcBlock = stripCMakeComments(cmake);
    const QString sources = extractCMakeSet(srcBlock, QStringLiteral("SOURCES"));
    check(!sources.isEmpty(), "能在 CMakeLists.txt 里找到 set(SOURCES ...) 那一段");
    if (sources.isEmpty()) return;

    // SOURCES 里列出来的 operators/*.cpp
    QSet<QString> listed;
    {
        const QRegularExpression re(QStringLiteral("operators/(\\w+\\.cpp)"));
        auto it = re.globalMatch(sources);
        while (it.hasNext())
            listed.insert(it.next().captured(1));
    }
    check(listed.size() >= 15,
          QString("主程序的 SOURCES 里列了 %1 个 operators/*.cpp").arg(listed.size()));

    // 磁盘上真的存在的 operators/*.cpp
    QDir dir(operatorsDir());
    QStringList onDisk;
    for (const QString& f : dir.entryList(QDir::Files))
        if (f.endsWith(".cpp")) onDisk.append(f);
    onDisk.sort();

    check(onDisk.size() >= 15,
          QString("operators/ 目录下有 %1 个 .cpp").arg(onDisk.size()));

    // (1) 磁盘上有、SOURCES 里没有 → 主程序根本没编它,审计却在查它
    for (const QString& f : onDisk) {
        check(listed.contains(f),
              QString("operators/%1 在主程序的 SOURCES 里 —— 审计查的必须是真在编译的")
                  .arg(f));
    }
    // (2) SOURCES 里有、磁盘上没有 → CMake 早晚会报错,审计先说清楚
    for (const QString& f : listed) {
        check(QFile::exists(dir.filePath(f)),
              QString("SOURCES 里列的 operators/%1 真实存在").arg(f));
    }
    check(onDisk.size() == listed.size(),
          QString("磁盘上 %1 个 operators/*.cpp,SOURCES 里 %2 个 —— 两边一样多")
              .arg(onDisk.size()).arg(listed.size()));
}

// ============================================================
// 用例 8:运行时缺失的算子必须说得出"为什么"
// ============================================================
//
// 欠账 5 说"面板置灰没有自动化测试"(要真窗口,无头造不出来)。
// 这条能测的是**另一半**:置灰只是"别让人以为能调",
// 真正要紧的是把原因写出来 —— 缺的是什么、去哪儿下载、
// 解压到哪、设哪个环境变量。
//
// 为什么必须单测这一句:「摆着能调但跑不了的旋钮 = 骗人」。
// 光置灰,用户只知道"不让调",不知道为什么;只写"不可用",
// 等于把问题原样推回给用户 —— 那是**没写**,不是**写清楚了**。
//
// 这句话同时被三处用:面板提示、Run 时的 error、这条断言。
// 只有一份,改一处三处一起变(所以它才值得被测)。
// ============================================================
static void test_runtime_missing_is_explained() {
    beginCase("8. 运行时缺失的算子必须说得出为什么");

    struct Runtime {
        const char* opName;
        bool available;
        QString reason;
        const char* mustMention;
    };
    const QVector<Runtime> runtimes = {
        {"dl_infer", DlInfer::runtimeAvailable(),
         DlInfer::runtimeUnavailableReason(), "ONNX"},
        {"image_source_camera", ImageSourceCamera::runtimeAvailable(),
         ImageSourceCamera::runtimeUnavailableReason(), "MVS"},
    };

    for (const auto& r : runtimes) {
        const QString opName = QString::fromUtf8(r.opName);
        if (r.available) {
            check(r.reason.isEmpty(),
                  QString("%1 的运行时可用 → 不该挂着\"缺失原因\"文案").arg(opName));
        } else {
            check(r.reason.length() >= 20,
                  QString("%1 的运行时不可用 → 原因文案不是一句空话(%2 字)")
                      .arg(opName).arg(r.reason.length()));
            check(r.reason.contains(QString::fromUtf8(r.mustMention)),
                  QString("%1 的原因文案里点名了缺的是 %2")
                      .arg(opName, QString::fromUtf8(r.mustMention)));
            check(r.reason.contains(QLatin1String("_DIR")),
                  QString("%1 的原因文案里说了要设哪个环境变量").arg(opName));
        }
    }
}

// ============================================================
// 用例 9:旋钮值类型不对时,必须说出来
// ============================================================
//
// 2026-10-21 第三轮:五个测量算子的坐标旋钮改走 operators/param_read.h。
// 2026-10-21 第四轮:**全部**算子都迁过去了 ——
//   整数旋钮 readIntKnob / 小数旋钮 readDoubleKnob / 文本旋钮 readTextKnob。
//
// 为什么这条要单独钉:旧代码 params.value("x1").toInt(默认值)
// 在类型不对时是**静默**的。实测(Qt 行为探针,不是照文档猜):
//   · 字符串 "100" → 返回默认值(Qt **不解析**字符串)
//   · 小数 25.7     → 返回默认值(**不截断**)
// 于是用户写进去的值被**整个丢掉**,算子拿默认值算,
// 还报出一个看着正常的结果 —— 而这些结果直接决定判废。
//
// 这条测的是"**说出来了**",不是"值对不对" ——
// 值对不对由真值表(用例 6)管。
//
// ------------------------------------------------------------
// ⚠ "坏值"必须按旋钮类型分别取,不能一个值打天下
// ------------------------------------------------------------
//   整数旋钮:字符串 "100" 是坏的;25.7 也是坏的(整数旋钮不能有小数)
//   小数旋钮:字符串 "0.9" 是坏的;25.7 是**好的** —— 小数旋钮存小数天经地义
//   文本旋钮:数字 100 是坏的;"100" 是**好的** —— 文本旋钮存字符串天经地义
//
// 所以每个用例有两个"坏值",而且**小数旋钮还多一条反向断言**:
// 25.7 不许报错 ——
// 只查"该报错的时候报错了"会漏掉"不该报错的时候也报错",
// 那同样是错的(用户填了合法值却被拒,而且没说是哪不对)。
// ============================================================
static void test_knob_type_mismatch_is_reported(const QString& maskPath,
                                                 const QString& imgPath,
                                                 const QString& boardPath) {
    beginCase("9. 旋钮值类型不对时,必须说出来(不能静默按默认值算)");

    // 这个算子要什么输入才能走到"读旋钮"那一步。
    //
    // ⚠ 这个前提很重要:旋钮读在 run() 的**哪一段**,决定了它报不报错。
    //   比如 template_match 的 method/threshold 读在 imread **之后** ——
    //   没给图的话它先报 "imread failed",根本走不到旋钮那一步。
    //   所以测它必须给一张真的图。
    //
    // ⚠ ImgTpl 比 Img 多给一个 template_path:
    //   template_match 的模板路径优先从**上游**取(输入口接的图),
    //   取不到才用参数里的 template_path。
    //   而 method/threshold 读在 imread **之后** ——
    //   模板路径不给对,它先报 "imread failed",走不到旋钮那一步。
    //   (template_path 那个用例反而**不能**给上游模板:
    //   给了就绕过参数里那一次读,那个旋钮就测不到了。)
    enum class Need { None, Mask, Img, ImgTpl, Board, Judge };

    struct Case {
        const char* opName;
        const char* badKey;
        w7::KnobKind kind;
        Need need;
    };
    const QVector<Case> cases = {
        // 测量类(第三轮迁的)
        {"line_fit",          "x1",      w7::KnobKind::Int,    Need::None},
        {"point_point_dist",  "x2",      w7::KnobKind::Int,    Need::None},
        {"caliper_dist",      "ax",      w7::KnobKind::Int,    Need::None},
        {"point_line_dist",   "cy",      w7::KnobKind::Int,    Need::None},
        {"angle",             "y4",      w7::KnobKind::Int,    Need::None},
        // 整数旋钮(第四轮迁的)
        {"blob_count",        "min_area",       w7::KnobKind::Int, Need::Mask},
        {"count_check",       "expected_min",   w7::KnobKind::Int, Need::None},
        {"circle_fit",        "min_area",       w7::KnobKind::Int, Need::Mask},
        {"image_source_video","max_frames",     w7::KnobKind::Int, Need::None},
        {"image_source_camera","trigger_mode",  w7::KnobKind::Int, Need::None},
        {"template_match",    "method",         w7::KnobKind::Int, Need::ImgTpl},
        // 小数旋钮
        {"circle_fit",        "min_circularity", w7::KnobKind::Double, Need::Mask},
        {"image_source_camera","exposure",       w7::KnobKind::Double, Need::None},
        {"pcb_inspect",       "min_area_pct",   w7::KnobKind::Double, Need::Board},
        {"template_match",    "threshold",      w7::KnobKind::Double, Need::ImgTpl},
        // 文本旋钮
        {"image_source_dir",  "dir_path",        w7::KnobKind::Text, Need::None},
        {"image_source_video","video_path",      w7::KnobKind::Text, Need::None},
        {"image_source_camera","camera_ip",      w7::KnobKind::Text, Need::None},
        {"dl_infer",          "model_path",      w7::KnobKind::Text, Need::None},
        {"pcb_inspect",       "mode",            w7::KnobKind::Text, Need::Board},
        {"template_match",    "template_path",   w7::KnobKind::Text, Need::Img},
        // 判废算子:旋钮在上游组件**之后**才读,得先真的跑一遍 pcb_inspect。
        {"pcb_defect_judge",  "dark_min_pct",    w7::KnobKind::Double, Need::Judge},
        {"pcb_defect_judge",  "mask_v_mul",      w7::KnobKind::Double, Need::Judge},
    };

    // 前置:给判废算子准备组件清单。
    //
    // 为什么不能编一个假清单:那会让测试**测不到真东西** ——
    // 判废算子拿到认不出的标签会先报错,根本走不到旋钮那一步。
    // 这里跑的是**真的** pcb_inspect,拿的是**真的**组件清单。
    QJsonArray judgeComps;
    {
        OperatorBase* insp = createOperatorByName("pcb_inspect");
        if (!insp) {
            check(false, "造不出 pcb_inspect(判废算子的前置)");
        } else {
            QJsonObject ip = insp->defaultParams();
            const QJsonObject req = requiredKnobs("pcb_inspect");
            for (auto it = req.begin(); it != req.end(); ++it) ip[it.key()] = it.value();
            judgeComps = insp->run(ip, QJsonObject{{"image_path", boardPath}})
                             .value("components").toArray();
            delete insp;
        }
        check(!judgeComps.isEmpty(),
              QString("前置:pcb_inspect 产出 %1 个组件(给 pcb_defect_judge 用)")
                  .arg(judgeComps.size()));
    }

    // 坏值:按旋钮类型取
    auto badValue1 = [](w7::KnobKind kind) -> QJsonValue {
        switch (kind) {
        case w7::KnobKind::Int:    return QJsonValue("100");   // 字符串
        case w7::KnobKind::Double: return QJsonValue("0.9");   // 字符串
        case w7::KnobKind::Text:   return QJsonValue(100);     // 数字
        }
        return QJsonValue();
    };
    auto badValue2 = [](w7::KnobKind kind) -> QJsonValue {
        switch (kind) {
        case w7::KnobKind::Int:    return QJsonValue(25.7);    // 小数不是整数
        case w7::KnobKind::Double: return QJsonValue(25.7);    // 合法!不能报错
        case w7::KnobKind::Text:   return QJsonValue(25.7);    // 数字不是文本
        }
        return QJsonValue();
    };

    for (const auto& c : cases) {
        const QString opName = QString::fromUtf8(c.opName);
        OperatorBase* op = createOperatorByName(opName);
        if (!op) {
            check(false, QString("%1 造不出来").arg(opName));
            continue;
        }
        const QString badKey = QString::fromUtf8(c.badKey);

        // 这个算子要什么输入
        QJsonObject inputs;
        switch (c.need) {
        case Need::None: break;
        case Need::Mask: inputs["mask_path"] = maskPath; break;
        case Need::Img:  inputs["image_path"] = imgPath; break;
        case Need::ImgTpl:
            inputs["image_path"] = imgPath;
            inputs["template_path"] = imgPath;
            break;
        case Need::Board: inputs["image_path"] = boardPath; break;
        case Need::Judge:
            inputs["image_path"] = boardPath;
            inputs["components"] = judgeComps;
            break;
        }
        // 必填旋钮(HSV 区间那些)每次都给全 ——
        // 缺了它们算子会先报"阈值不合法",走不到旋钮那一步。
        const QJsonObject req = requiredKnobs(opName);
        QJsonObject base;
        for (auto it = req.begin(); it != req.end(); ++it) base[it.key()] = it.value();

        // ① 坏值一:必须报错,而且要**点名是哪个旋钮**
        QJsonObject p1 = base;
        p1[badKey] = badValue1(c.kind);
        const QJsonObject o1 = op->run(p1, inputs);
        const QString e1 = o1.value("error").toString();
        check(e1.contains(badKey),
              QString("%1 的 %2 存成错类型(%3) → 必须报错并点名这个旋钮(实际:\"%4\")")
                  .arg(opName, badKey, badValue1(c.kind).toVariant().toString(), e1));

        // ② 坏值二:
        //    整数旋钮:25.7 也是坏的(旧代码是**整个值丢掉**,不是四舍五入)
        //    文本旋钮:数字不是文本
        //    小数旋钮:25.7 是**合法的** —— 不许报错
        QJsonObject p2 = base;
        p2[badKey] = badValue2(c.kind);
        const QJsonObject o2 = op->run(p2, inputs);
        const QString e2 = o2.value("error").toString();
        if (c.kind == w7::KnobKind::Double) {
            check(!e2.contains(badKey),
                  QString("%1 的 %2 是小数旋钮,存 25.7 是**合法**的 → 不许报错(实际:\"%3\")")
                      .arg(opName, badKey, e2));
        } else {
            check(e2.contains(badKey),
                  QString("%1 的 %2 存成小数(25.7) → 必须报错并点名这个旋钮(实际:\"%3\")")
                      .arg(opName, badKey, e2));
        }

        // ③ 缺这个旋钮:不算错(用户没配),按声明的默认值跑
        //
        //    判据是"错误信息里不许提到这个旋钮",而不是"不许有错误" ——
        //    有些算子本来就会因为别的理由报错(没给图、目录不存在),
        //    那些错误和这个旋钮没关系,不该算这条的失败。
        const QJsonObject o3 = op->run(base, inputs);
        const QString e3 = o3.value("error").toString();
        check(!e3.contains(badKey),
              QString("%1 缺 %2 → 不该因为这个旋钮报错(实际:\"%3\")")
                  .arg(opName, badKey, e3));

        delete op;
    }
}

// ============================================================
// 造测试素材
// ============================================================
static void makeProbeImages(QString& maskPath, QString& imgPath) {
    QDir().mkpath("images");

    // 一个面积 30 的白块(5x6)。
    // 为什么是 30:blob_count 声明的 min_area = 20,旧兜底是 50 ——
    // 30 正好卡在中间,兜底一漂,count 就从 1 变成 0。
    cv::Mat mask = cv::Mat::zeros(100, 100, CV_8UC1);
    mask(cv::Rect(10, 10, 5, 6)).setTo(255);
    maskPath = "images/knob_probe_mask.png";
    cv::imwrite(maskPath.toStdString(), mask);

    // 一张有纹理的图(给 template_match / pcb_inspect 用)
    cv::Mat img(120, 120, CV_8UC3);
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x)
            img.at<cv::Vec3b>(y, x) = cv::Vec3b((x * 3) % 256, (y * 5) % 256, (x + y) % 256);
    imgPath = "images/knob_probe_img.png";
    cv::imwrite(imgPath.toStdString(), img);
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);

    qInfo().noquote() << "W7 旋钮审计 回归测试";

    // 当前构建的运行时状态 —— 问算子自己,不靠猜
    const bool hasOnnx = DlInfer::runtimeAvailable();
    const bool hasMvs  = ImageSourceCamera::runtimeAvailable();
    qInfo().noquote() << "运行时:ONNX Runtime ="
                      << (hasOnnx ? "有" : "无")
                      << "  MVS SDK =" << (hasMvs ? "有" : "无");

    // 解析全部算子源文件
    QVector<OperatorSource> ops;
    QDir dir(operatorsDir());
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString& f : files) {
        if (!f.endsWith(".cpp")) continue;
        OperatorSource op;
        if (parseOperator(dir.filePath(f), hasOnnx, hasMvs, op))
            ops.append(op);
    }
    qInfo().noquote() << "扫到" << ops.size() << "个算子源文件";

    QString maskPath, imgPath;
    makeProbeImages(maskPath, imgPath);

    // 真值板图(来自 W6 的生成器,CMake 每次构建都重新拷过来)。
    // pcb_inspect 的两条探针要靠它:没有板图,那两条探针只会
    // 测到"imread 失败",等于什么都没测。
    const QString boardPath = "images/baseline.png";
    check(QFile::exists(boardPath),
          QString("能找到真值板图 %1(CMake 每次构建会把 W6 的 images/ 拷过来)")
              .arg(boardPath));

    test_audit_is_not_vacuous(ops);
    test_no_dead_knobs(ops);
    test_no_undeclared_knobs(ops);
    test_defaults_have_single_source(ops);
    test_fallbacks_have_single_source(ops);
    test_panel_initials_have_single_source(ops);
    test_string_defaults_reach_the_panel(ops);
    test_setparams_fallbacks_have_single_source(ops);
    test_fallbacks_not_copied_from_library(ops);
    test_exemptions_not_stale();
    test_empty_params_match_defaults(maskPath, imgPath, boardPath);
    test_default_value_truth_table();
    test_audited_sources_are_the_compiled_ones();
    test_runtime_missing_is_explained();
    test_knob_type_mismatch_is_reported(maskPath, imgPath, boardPath);

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
