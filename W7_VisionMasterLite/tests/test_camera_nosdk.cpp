// ============================================================
// W7 image_source_camera「没装 MVS SDK」那条分支 的回归测试
// ============================================================
//
// ⚠ 这一条分支才是**实际发货**的那一半 ——
//   本仓库绝大多数人(和所有 CI runner)都没装 MVS SDK。
//   它以前一行都没测过,于是「相机 IP 填错了」和「SDK 没装」
//   两种病因在日志里长得一样,用户只能靠猜。
//
// ⚠ 它必须是**独立的目标**,不能和带假 SDK 的那个合到一个可执行文件里 ——
//   一个带 HAS_MVS_SDK、一个不带,同一个二进制里会有两个
//   ImageSourceCamera::run,那是 ODR 违规,链接器会挑一个(而且挑哪个还不一定)。
//
// ============================================================

#include "operators/image_source_camera.h"

#include <QApplication>
#include <QJsonObject>
#include <QDebug>

using namespace w7;

// ============================================================
// ⚠ 编译期守卫
// ============================================================
#ifdef HAS_MVS_SDK
#error "这个套件必须在**没有** HAS_MVS_SDK 的配置下编译(见 CMakeLists 的 w7_camera_nosdk_test)"
#endif

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

static QString errOf(const QJsonObject& out) {
    return out.value("error").toString();
}

static QJsonObject mkParams() {
    QJsonObject p;
    p["camera_ip"]    = "192.168.1.10";
    p["exposure"]     = 5.0;
    p["trigger_mode"] = 0;
    return p;
}

// ============================================================
// 用例 1:这个构建里确实没有 MVS SDK
// ============================================================
static void test_runtime_is_unavailable() {
    beginCase("1. 这个构建里确实没有 MVS SDK");

    check(!ImageSourceCamera::runtimeAvailable(),
          "runtimeAvailable() 说没有(这个目标故意不定义 HAS_MVS_SDK)");
    check(!ImageSourceCamera::runtimeUnavailableReason().isEmpty(),
          "不可用的原因不是空的");
    check(ImageSourceCamera::runtimeUnavailableReason().contains("MVS_DIR"),
          QString("原因里点名了环境变量(实际 \"%1\")")
              .arg(ImageSourceCamera::runtimeUnavailableReason()));
}

// ============================================================
// 用例 2:没装 SDK 时 run() 要说清是 SDK 的事
// ============================================================
static void test_run_reports_sdk_missing() {
    beginCase("2. 没装 SDK 时 run() 的报错");

    ImageSourceCamera op;
    const QJsonObject out = op.run(mkParams(), QJsonObject{});
    const QString e = errOf(out);

    check(out.contains("error"), "报了错");
    check(!e.isEmpty(), "报错不是空的");
    check(e == ImageSourceCamera::runtimeUnavailableReason(),
          QString("用的是面板上那一句(同一份,不是另抄的)(实际 \"%1\")").arg(e));
    check(e.contains("MVS"), QString("说清了是 MVS 的问题(实际 \"%1\")").arg(e));
    check(!out.contains("image_path"), "没产出图像路径(没相机哪来的图)");
}

// ============================================================
// 用例 3:旋钮类型不对 —— **另一种**病因,要说另一种话
// ============================================================
static void test_knob_errors_are_different_disease() {
    beginCase("3. 旋钮类型不对(另一种病因)");

    ImageSourceCamera op;
    const QString sdkMessage = ImageSourceCamera::runtimeUnavailableReason();

    QJsonObject p = mkParams();
    p["camera_ip"] = 12345;                 // IP 存成了数字
    const QString eIp = errOf(op.run(p, QJsonObject{}));
    check(eIp.contains("camera_ip"),
          QString("点名了是哪个旋钮(实际 \"%1\")").arg(eIp));
    check(eIp != sdkMessage,
          QString("和「SDK 没装」那句话**不是**同一句(实际 \"%1\")").arg(eIp));

    p = mkParams();
    p["exposure"] = "十毫秒";               // 曝光存成了文本
    const QString eExp = errOf(op.run(p, QJsonObject{}));
    check(eExp.contains("exposure"),
          QString("点名了是哪个旋钮(实际 \"%1\")").arg(eExp));
    check(eExp != eIp, "两个旋钮的报错**不是**同一句");

    p = mkParams();
    p["trigger_mode"] = "软触发";           // 触发模式存成了文本
    const QString eTrig = errOf(op.run(p, QJsonObject{}));
    check(eTrig.contains("trigger_mode"),
          QString("点名了是哪个旋钮(实际 \"%1\")").arg(eTrig));
    check(eTrig != eIp && eTrig != eExp, "三个旋钮的报错**互不相同**");
}

// ============================================================
// 用例 4:releaseSession() 在没装 SDK 时是空操作
// ============================================================
static void test_release_is_noop_without_sdk() {
    beginCase("4. releaseSession() 没装 SDK 时是空操作");

    ImageSourceCamera::releaseSession();
    ImageSourceCamera::releaseSession();
    check(true, "连着调两次也没崩(没装 SDK 时本来就没有会话)");
}

int main(int argc, char* argv[]) {
    // ⚠ 必须在 QApplication 构造**之前**设好。
    //   image_source_camera 拖了 QtWidgets(参数面板),链接的是 Widgets。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 image_source_camera「没装 MVS SDK」回归测试";

    test_runtime_is_unavailable();
    test_run_reports_sdk_missing();
    test_knob_errors_are_different_disease();
    test_release_is_noop_without_sdk();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
