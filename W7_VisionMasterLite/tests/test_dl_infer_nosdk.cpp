// ============================================================
// W7 dl_infer「没装 ONNX Runtime」那条分支 的回归测试
// ============================================================
//
// 测的是:#ifndef HAS_ONNXRUNTIME 那半边 —— 也就是**实际发货的那一半**。
//
// ⚠ 本仓库绝大多数人(和所有 CI runner)都没装 ONNX Runtime,
//   所以他们拿到的就是这个分支。
//   而它以前**一行都没测过** ——
//   于是「模型路径写错了」和「SDK 没装」这两种病因在日志里长得一样,
//   用户只能靠猜。
//
// 这条分支要做的事很少,但每一件都是"替用户分清病因":
//   · 上游没给图            -> 说清是上游的事
//   · 模型文件不存在        -> 说清是路径的事
//   · 两样都没问题          -> 才轮到说"SDK 没装",而且要说清**怎么装**
//
// ============================================================
// ⚠ 这套和 tests/test_dl_infer.cpp **不是同一份代码** ——
//   一个编了 HAS_ONNXRUNTIME,一个没编。
//   同一个可执行文件里不能有两个 DlInfer::run ——
//   所以它们是两个**独立的目标**,不是一个目标里的两个用例。
//
//   这也正是「代码升级与审计升级必须同步做」的另一面:
//   两条分支都要有测试,缺一条就等于那条分支没存在过。
// ============================================================

#include "operators/dl_infer.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QDebug>
#include <QApplication>

using namespace w7;

// ============================================================
// ⚠ 编译期守卫:这个套件**必须**跑在"没装 SDK"的那份 dl_infer 上。
//   万一 CMake 哪次改坏了(比如手滑给这个目标也定义了
//   HAS_ONNXRUNTIME),这个套件会去测**另一半**代码 ——
//   而那一半的运行时是"可用"的,所有"SDK 没装"的断言都会红。
//   宁可编译不过,也不要测错东西。
// ============================================================
#ifdef HAS_ONNXRUNTIME
#error "这个套件必须在**没有** HAS_ONNXRUNTIME 的情况下编译(见 CMakeLists 的 w7_dl_infer_nosdk_test)"
#endif

// ============================================================
// 极简断言
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

static QString makeModel(const QString& name) {
    const QString path = QDir::current().filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("not a real model, this branch only checks existence");
        f.close();
    }
    return path;
}

static QJsonObject mkParams(const QString& modelPath) {
    QJsonObject p;
    p["model_path"]     = modelPath;
    p["input_w"]        = 640;
    p["input_h"]        = 640;
    p["conf_threshold"] = 0.25;
    p["nms_iou"]        = 0.45;
    p["class_names"]    = "person,bicycle,car";
    p["output_layout"]  = "yolov8";
    return p;
}

static QJsonObject mkInputs(const QString& imgPath) {
    QJsonObject in;
    in["image_path"]  = imgPath;
    in["node_uuid"]   = QStringLiteral("u-dl");
    in["frame_index"] = 0;
    return in;
}

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 dl_infer(没装 ONNX Runtime 那条分支) 回归测试";

    DlInfer op;

    // ============================================================
    // 用例 1:这个构建里**确实**没有运行时
    // ============================================================
    beginCase("1. 这个构建里确实没有 ONNX Runtime");

    check(!DlInfer::runtimeAvailable(),
          "runtimeAvailable() 说没有(这个目标故意不定义 HAS_ONNXRUNTIME)");

    // 不可用的原因必须说清**怎么修**
    const QString reason = DlInfer::runtimeUnavailableReason();
    check(!reason.isEmpty(), "不可用的原因不是空的");
    check(reason.contains("ONNXRUNTIME_DIR"),
          QString("原因里点名了环境变量(实际 \"%1\")").arg(reason));

    // ============================================================
    // 用例 2:每一种病因都要说清是哪种
    // ============================================================
    beginCase("2. 每一种病因都要说清是哪种");

    const QString model = makeModel("dl_model.onnx");
    const QString img   = QDir::current().filePath("dl_in.png");

    // 上游没给图
    {
        QJsonObject in;
        in["node_uuid"] = "u-dl";
        const QJsonObject out = op.run(mkParams(model), in);
        check(out.value("error").toString().contains("image_path"),
              QString("上游没给图 -> 报错点名 image_path(实际 \"%1\")")
                  .arg(out.value("error").toString()));
    }

    // 模型文件不存在
    {
        QJsonObject p = mkParams(QDir::current().filePath("no_such_model.onnx"));
        const QJsonObject out = op.run(p, mkInputs(img));
        check(out.value("error").toString().contains("模型文件不存在"),
              QString("模型文件不存在 -> 报错(实际 \"%1\")")
                  .arg(out.value("error").toString()));
    }

    // model_path 类型不对 -> 点名旋钮
    {
        QJsonObject p = mkParams(model);
        p["model_path"] = 123;
        const QJsonObject out = op.run(p, mkInputs(img));
        const QString e = out.value("error").toString();
        check(e.contains("model_path") && e.contains("不是文本"),
              QString("model_path 类型不对 -> 点名这个旋钮(实际 \"%1\")").arg(e));
    }

    // 一切正常 -> 才轮到说"SDK 没装",而且要说清是哪种病
    {
        const QJsonObject out = op.run(mkParams(model), mkInputs(img));
        const QString e = out.value("error").toString();
        check(e.contains("ONNX"),
              QString("都正常的时候才说 SDK 的事(实际 \"%1\")").arg(e));
        check(e.contains("模型文件本身是存在的"),
              QString("顺便说清模型文件是在的(两种病因两种药)(实际 \"%1\")").arg(e));
    }

    // ============================================================
    // 用例 3:守卫 —— 这条用例不是空转
    // ============================================================
    beginCase("3. 守卫:这条用例不是空转");
    {
        // 拿"模型文件不存在"的输入,必须和"一切正常"的输入给出**不同**的错。
        // 一样的话,说明这条用例根本没在看错误信息。
        const QJsonObject missing = op.run(
            mkParams(QDir::current().filePath("no_such_model.onnx")), mkInputs(img));
        const QJsonObject ok = op.run(mkParams(model), mkInputs(img));
        check(missing.value("error").toString() != ok.value("error").toString(),
              "两种病因给出的是不同的报错(不然这条用例是恒真的)");
        check(ok.value("error").toString().contains("安装"),
              "没装的时候说得出「要装」(不是只说「不可用」)");
    }

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
