// ============================================================
// W7 image_source_camera 相机路径 的回归测试(假 MVS SDK)
// ============================================================
//
// 测的是:image_source_camera 那半边 #ifdef HAS_MVS_SDK 的代码。
//
// ⚠ 这半边代码在写这个套件之前**从来没有被编译过一次** ——
//   本机没装 MVS SDK,CMake 走空实现,那半边根本不参与编译。
//   于是"相机连不上"这件事,连"是代码错了"还是"是环境缺了"都分不清。
//   更糟的是:那段代码里**引用了一个真 SDK 里根本不存在的常量**
//   (MV_TRIGGER_MODE_SOFT),也就是说**装上 SDK 的机器上它编不过** ——
//   而这件事永远不会有人发现。
//
// 这里用**假 MVS SDK**(third_party/fake_mvs/)顶上:
//   · 相机路径**真的被编译、真的被跑**
//   · 枚举到的相机、返回的帧、每一调用的返回值都是**脚本**给的
//   · 假件把 SDK 这边经历了什么都**记下来** -> 断言能看着它
//
// ⚠ 假件故意**不提供** MV_TRIGGER_MODE_SOFT ——
//   真 SDK 里没有它。假件要是补上,就把这个 bug 遮住了。
//   「假的东西要在该严的地方严」:
//
// ============================================================
// 它抓到了什么(写这套测试之前没人知道)
// ============================================================
//
// 1. **MV_TRIGGER_MODE_SOFT 这个常量不存在** —— 编译期就红。
//    软触发在真 SDK 里是两件事:
//        TriggerMode = ON  **加上** TriggerSource = SOFTWARE
//    只设 TriggerMode 的话,相机还在等外部信号 -> 永远不出图。
//
// 2. **IP 对不上也照样连** —— 之前是 `selected = 0` 然后"找到就改",
//    没找到就用第 0 台。用户填 A 的 IP、相机接在 B 上,
//    于是连的是 B、输出里报的是 A —— 静默失效。
//
// 3. **Bayer 一律按 BG 解马赛克** —— Bayer 有 RG/GR/GB/BG 四种排布,
//    其余三种的颜色是错的,而且不报错、不崩溃。
//
// 4. **改了 IP 不重连** —— 会话状态是个不记是谁的布尔量。
//
// 5. **句柄只开不关** —— StopGrabbing / CloseDevice / DestroyHandle
//    一次都没调过。
//
// 6. **SDK 的返回值全都不看** —— 打开失败、开始取流失败、
//    设参数失败,统统当成功往下走。
//
// 7. **缓冲区多大都不看** —— 相机给的字节数不够,照样按
//    宽 x 高 x 通道去读 -> 越界,崩的是整个程序。
//
// ============================================================

#include "operators/image_source_camera.h"

// ⚠ 这是**假的** MVS SDK —— 头文件在 third_party/fake_mvs/。
//   CMake 把那个目录加进本目标的 include 路径。
#include <MvCameraControl.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>
#include <QDebug>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

using namespace w7;

// ============================================================
// ⚠ 编译期守卫:这个套件**必须**跑在"带假 SDK"的那份算子上。
//   万一 CMake 哪次改坏了(比如忘了定义 HAS_MVS_SDK),
//   这个套件会**测到空实现** —— 而空实现里根本没有相机。
//   宁可编译不过,也不要测错东西。
// ============================================================
#ifndef W7_MVS_FAKE
#error "这个套件必须在 HAS_MVS_SDK + 假 SDK 下编译(见 CMakeLists 的 w7_camera_test)"
#endif
#ifndef HAS_MVS_SDK
#error "这个套件必须在定义了 HAS_MVS_SDK 的情况下编译"
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

// ============================================================
// 小工具
// ============================================================

// 会话是**进程级**的,假件的状态也是进程级的。
// 两者必须一起复位,否则计数会串。
static void resetAll() {
    ImageSourceCamera::releaseSession();
    fakeMvs::reset();
}

// 让假 SDK 枚举到一台相机
static void scriptDevice(const QString& ip) {
    fakeMvs::Device d;
    d.ip = fakeMvs::ipToUint(ip.toStdString());
    d.layerType = MV_GIGE_DEVICE;
    fakeMvs::setDevices(std::vector<fakeMvs::Device>{d});
}

// 让假 SDK 能吐出一帧 2x2 的 BGR8 图。
//
// ⚠ 失败路径的用例也要给帧:否则一旦那段"失败处理"被删掉,
//   算子会一路走到 imwrite —— 而没有帧的时候那是**空图**,
//   cv::imwrite 对空图会抛异常,整个套件**崩掉**。
//   崩掉也是"没绿",但那不是一条能说明问题的红。
static void scriptFrame() {
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));
}

static QJsonObject mkParams(const QString& ip, double exposure = 5.0, int trigger = 0) {
    QJsonObject p;
    p["camera_ip"]   = ip;
    p["exposure"]    = exposure;
    p["trigger_mode"] = trigger;
    return p;
}

static QJsonObject runCam(const QJsonObject& params) {
    ImageSourceCamera op;
    return op.run(params, QJsonObject{});
}

// 算子把帧写到系统临时目录,测试读完要删掉 ——
// 测试产物不许留在别人的地盘上。
static QStringList g_tempFiles;
static void cleanupTempFiles() {
    for (int i = 0; i < g_tempFiles.size(); ++i) QFile::remove(g_tempFiles[i]);
    g_tempFiles.clear();
}

static QString errOf(const QJsonObject& out) {
    return out.value("error").toString();
}

// ============================================================
// 用例 1:这个构建里确实有 MVS SDK(假的那套)
// ============================================================
static void test_runtime_is_available() {
    beginCase("1. 这个构建里确实有 MVS SDK");

    check(ImageSourceCamera::runtimeAvailable(),
          "runtimeAvailable() 说有(这个目标故意定义了 HAS_MVS_SDK)");
    check(ImageSourceCamera::runtimeUnavailableReason().isEmpty(),
          "可用时不该有「不可用的原因」");
    check(fakeMvs::enumCount() == 0,
          QString("还没跑过,SDK 那边一次都没被碰过(实际 %1 次)").arg(fakeMvs::enumCount()));
}

// ============================================================
// 用例 2:正常取到一帧(BGR8)
// ============================================================
static void test_bgr8_frame() {
    beginCase("2. 正常取到一帧(BGR8)");

    resetAll();
    scriptDevice("10.0.0.1");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(4, 3);
    std::vector<unsigned char> data(4 * 3 * 3);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>((i * 7) % 251);
    fakeMvs::setFrameData(data);

    const QJsonObject out = runCam(mkParams("10.0.0.1"));

    check(!out.contains("error"), QString("没报错(实际 \"%1\")").arg(errOf(out)));
    check(out.value("width").toInt() == 4,
          QString("宽度是 4(实际 %1)").arg(out.value("width").toInt()));
    check(out.value("height").toInt() == 3,
          QString("高度是 3(实际 %1)").arg(out.value("height").toInt()));
    check(out.value("channels").toInt() == 3,
          QString("三通道(实际 %1)").arg(out.value("channels").toInt()));
    check(out.value("camera_ip").toString() == "10.0.0.1",
          QString("报的 IP 是连上的那台(实际 \"%1\")").arg(out.value("camera_ip").toString()));

    check(fakeMvs::enumCount() == 1,
          QString("枚举了一次(实际 %1)").arg(fakeMvs::enumCount()));
    check(fakeMvs::createCount() == 1,
          QString("建了一次句柄(实际 %1)").arg(fakeMvs::createCount()));
    check(fakeMvs::openCount() == 1,
          QString("打开了一次(实际 %1)").arg(fakeMvs::openCount()));
    check(fakeMvs::startGrabCount() == 1,
          QString("开始取流一次(实际 %1)").arg(fakeMvs::startGrabCount()));
    check(fakeMvs::getBufferCount() == 1,
          QString("取了一帧(实际 %1)").arg(fakeMvs::getBufferCount()));
    check(fakeMvs::freeBufferCount() == 1,
          QString("**缓冲区还回去了**(实际 %1)—— 不还就是漏").arg(fakeMvs::freeBufferCount()));

    // 曝光:毫秒 -> 微秒
    check(fakeMvs::lastFloatKey() == "ExposureTime",
          QString("曝光是设给 ExposureTime 的(实际 \"%1\")")
              .arg(QString::fromStdString(fakeMvs::lastFloatKey())));
    check(qAbs(fakeMvs::lastFloatValue() - 5000.0f) < 0.01f,
          QString("曝光 5ms -> 5000us(实际 %1)").arg(fakeMvs::lastFloatValue()));

    // 连续模式:TriggerMode=OFF,而且**不该**去设 TriggerSource
    check(fakeMvs::lastEnumValueFor("TriggerMode") == MV_TRIGGER_MODE_OFF,
          QString("TriggerMode 设成了 OFF(实际 0x%1)")
              .arg(fakeMvs::lastEnumValueFor("TriggerMode"), 8, 16, QChar('0')));
    check(fakeMvs::enumCallCountFor("TriggerSource") == 0,
          QString("连续模式没去设 TriggerSource(实际设了 %1 次)")
              .arg(fakeMvs::enumCallCountFor("TriggerSource")));

    // 落盘的那张图,内容要跟相机给的一致
    const QString path = out.value("image_path").toString();
    if (!path.isEmpty()) g_tempFiles << path;
    check(QFileInfo::exists(path), QString("临时文件写出来了(%1)").arg(path));
    const cv::Mat got = cv::imread(path.toStdString(), cv::IMREAD_UNCHANGED);
    check(!got.empty(), "那张图读得回来");
    if (!got.empty()) {
        check(got.rows == 3 && got.cols == 4,
              QString("尺寸对(实际 %1x%2)").arg(got.cols).arg(got.rows));
        check(got.channels() == 3, "三通道");
        bool same = got.total() * 3 == static_cast<std::size_t>(data.size());
        for (int i = 0; same && i < got.total(); ++i) {
            const cv::Vec3b& p = got.at<cv::Vec3b>(i);
            const unsigned char* q = &data[static_cast<std::size_t>(i) * 3];
            same = (p[0] == q[0] && p[1] == q[1] && p[2] == q[2]);
        }
        check(same, "像素和相机给的**逐个相同**(不是被清零、不是被换通道)");
    }
}

// ============================================================
// 用例 3:灰度图(Mono8)
// ============================================================
static void test_mono8_frame() {
    beginCase("3. 灰度图(Mono8)");

    resetAll();
    scriptDevice("10.0.0.2");
    fakeMvs::setPixelType(PixelType_Gvsp_Mono8);
    fakeMvs::setFrameSize(4, 3);
    std::vector<unsigned char> data(4 * 3);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 3);
    fakeMvs::setFrameData(data);

    const QJsonObject out = runCam(mkParams("10.0.0.2"));

    check(!out.contains("error"), QString("没报错(实际 \"%1\")").arg(errOf(out)));
    check(out.value("channels").toInt() == 1,
          QString("**一**通道(实际 %1)—— 灰度图别当成三通道").arg(out.value("channels").toInt()));

    const QString path = out.value("image_path").toString();
    if (!path.isEmpty()) g_tempFiles << path;
    const cv::Mat got = cv::imread(path.toStdString(), cv::IMREAD_UNCHANGED);
    check(!got.empty() && got.channels() == 1,
          QString("落盘那张也是一通道(实际 %1)").arg(got.empty() ? -1 : got.channels()));
}

// ============================================================
// 用例 4:Bayer —— **不能**一律按 BG 解
// ============================================================
//
// ⚠ 图不能太小。实测:OpenCV 的去马赛克在 4x4 上会**退化** ——
//   解出来的图是灰的(三个通道一模一样),RG 解和 BG 解结果相同,
//   于是这条用例变成"测不出转换码选对没有"。
//   16x16 才解出真正的颜色(RG 解和 BG 解差 128)。
//
//   真相机的图很大,所以这不是生产问题 —— 但**测试数据**必须够大,
//   否则测的就不是"选对转换码",而是"OpenCV 在小图上退化"。
//
// 同一份原始数据,按 RG 解和按 BG 解出来的颜色**不一样**
// (实测:两者的 R 和 B 正好互换)—— 所以"算子选了哪个转换码"是能测出来的。
static void test_bayer_uses_the_right_code() {
    beginCase("4. Bayer 四种排布各用各的转换码");

    const int N = 16;
    std::vector<unsigned char> raw(static_cast<std::size_t>(N) * N);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<unsigned char>((i * 13) % 256);

    const cv::Mat src(N, N, CV_8UC1, raw.data());
    cv::Mat asRG, asBG;
    cv::cvtColor(src, asRG, cv::COLOR_BayerRG2BGR);
    cv::cvtColor(src, asBG, cv::COLOR_BayerBG2BGR);
    check(cv::norm(asRG, asBG, cv::NORM_INF) > 1.0,
          "先确认一下:同一份原始数据按 RG 解和按 BG 解**确实不一样**(不然这条用例是恒真的)");

    struct Case {
        MvGvspPixelType type;
        const char*      name;
        cv::Mat          expected;
    };
    std::vector<Case> cases;
    {
        cv::Mat m; cv::cvtColor(src, m, cv::COLOR_BayerRG2BGR);
        cases.push_back({PixelType_Gvsp_BayerRG8, "RG", m});
    }
    {
        cv::Mat m; cv::cvtColor(src, m, cv::COLOR_BayerGR2BGR);
        cases.push_back({PixelType_Gvsp_BayerGR8, "GR", m});
    }
    {
        cv::Mat m; cv::cvtColor(src, m, cv::COLOR_BayerGB2BGR);
        cases.push_back({PixelType_Gvsp_BayerGB8, "GB", m});
    }
    {
        cv::Mat m; cv::cvtColor(src, m, cv::COLOR_BayerBG2BGR);
        cases.push_back({PixelType_Gvsp_BayerBG8, "BG", m});
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const QString ip = QString("10.0.1.%1").arg(i + 1);

        resetAll();
        scriptDevice(ip);
        fakeMvs::setPixelType(cases[i].type);
        fakeMvs::setFrameSize(N, N);
        fakeMvs::setFrameData(raw);

        const QJsonObject out = runCam(mkParams(ip));
        const QString what = QString("Bayer %1 排布").arg(cases[i].name);
        check(!out.contains("error"), QString("%1:没报错(实际 \"%2\")").arg(what).arg(errOf(out)));
        check(out.value("channels").toInt() == 3,
              QString("%1:解完马赛克是三通道(实际 %2)").arg(what).arg(out.value("channels").toInt()));

        const QString path = out.value("image_path").toString();
        if (!path.isEmpty()) g_tempFiles << path;
        const cv::Mat got = cv::imread(path.toStdString(), cv::IMREAD_UNCHANGED);
        if (got.empty() || got.size() != cases[i].expected.size()) {
            check(false, QString("%1:落盘那张图读不回来").arg(what));
            continue;
        }
        const double diff = cv::norm(got, cases[i].expected, cv::NORM_INF);
        check(diff < 1.0,
              QString("%1:用的是**自己那个**转换码(和期望差 %2)")
                  .arg(what).arg(diff));
    }
}

// ============================================================
// 用例 5:不认识的像素格式要报错
// ============================================================
static void test_unsupported_pixel_format() {
    beginCase("5. 不认识的像素格式");

    resetAll();
    scriptDevice("10.0.0.3");
    fakeMvs::setPixelType(PixelType_Gvsp_RGB8_Packed);
    fakeMvs::setFrameSize(4, 3);
    fakeMvs::setFrameData(std::vector<unsigned char>(4 * 3 * 3, 5));

    const QJsonObject out = runCam(mkParams("10.0.0.3"));
    const QString e = errOf(out);

    check(out.contains("error"), "报了错");
    check(e.contains("不支持的像素格式"), QString("说清了是像素格式的问题(实际 \"%1\")").arg(e));
    check(e.contains("0x02180014"),
          QString("把格式的**值**也写出来了(实际 \"%1\")").arg(e));
    check(fakeMvs::freeBufferCount() == 1,
          QString("出错时缓冲区也还回去了(实际 %1)").arg(fakeMvs::freeBufferCount()));
}

// ============================================================
// 用例 6:取帧失败
// ============================================================
static void test_get_buffer_fails() {
    beginCase("6. 取帧失败");

    resetAll();
    scriptDevice("10.0.0.4");
    fakeMvs::setGetBufferRet(-1);

    const QJsonObject out = runCam(mkParams("10.0.0.4"));
    const QString e = errOf(out);
    check(out.contains("error"), "报了错");
    check(e.contains("GetImageBuffer") && e.contains("-1"),
          QString("说清了是哪一步、错误码是多少(实际 \"%1\")").arg(e));
}

// ============================================================
// 用例 7:相机给的缓冲区太小 -> 不能越界读
// ============================================================
static void test_buffer_too_small() {
    beginCase("7. 缓冲区太小");

    resetAll();
    scriptDevice("10.0.0.5");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(4, 3);
    fakeMvs::setFrameData(std::vector<unsigned char>(4 * 3 * 3, 9));
    fakeMvs::setBufferSize(10);          // 明明要 36 字节,只给 10

    const QJsonObject out = runCam(mkParams("10.0.0.5"));
    const QString e = errOf(out);
    check(out.contains("error"), "报了错,而不是越界读崩掉");
    check(e.contains("缓冲区太小"), QString("说清了是缓冲区的问题(实际 \"%1\")").arg(e));
    check(e.contains("36") && e.contains("10"),
          QString("把**要多少**和**给了多少**都写出来了(实际 \"%1\")").arg(e));
}

// ============================================================
// 用例 8:枚举不到相机 / 枚举失败 —— 两种病因两种药
// ============================================================
static void test_enum_failures() {
    beginCase("8. 枚举不到相机 / 枚举失败");

    resetAll();
    fakeMvs::setDevices(std::vector<fakeMvs::Device>{});
    const QJsonObject none = runCam(mkParams("10.0.0.6"));
    const QString eNone = errOf(none);
    check(none.contains("error"), "枚举到 0 台时报了错");
    check(eNone.contains("未发现 GigE 相机"),
          QString("说的是\"没发现相机\"(实际 \"%1\")").arg(eNone));
    check(eNone.contains("10.0.0.6"),
          QString("把用户填的 IP 点出来了(实际 \"%1\")").arg(eNone));

    resetAll();
    fakeMvs::setEnumRet(-1);
    const QJsonObject bad = runCam(mkParams("10.0.0.7"));
    const QString eBad = errOf(bad);
    check(bad.contains("error"), "枚举调用失败时报了错");
    check(eBad.contains("枚举相机失败"),
          QString("说的是\"枚举调用失败\"(实际 \"%1\")").arg(eBad));
    check(eNone != eBad,
          "两种病因给出的是不同的报错(不然这两条用例是恒真的)");
}

// ============================================================
// 用例 9:IP 对不上 —— 不许偷偷连第一台
// ============================================================
static void test_ip_must_match() {
    beginCase("9. IP 对不上不许偷偷连第一台");

    resetAll();
    scriptDevice("10.0.0.1");          // 相机接在这台
    // ⚠ 也要给一帧:万一段落退化成"连了第 0 台",
    //   它就能真的取到一帧 —— 于是断言是**干净地红**,
    //   而不是崩在 cv::imwrite 上(空图会抛异常)。
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));
    const int before = fakeMvs::createCount();

    const QJsonObject out = runCam(mkParams("10.0.0.99"));  // 用户填的是另一台
    const QString e = errOf(out);

    check(out.contains("error"), "报了错,而不是连了别的相机");
    check(e.contains("没找到 IP=10.0.0.99"),
          QString("点名了要找哪台(实际 \"%1\")").arg(e));
    check(e.contains("10.0.0.1"),
          QString("把**实际找到的**相机也写出来了(实际 \"%1\")—— "
                  "这一条还顺带钉住了 IP 的字节序").arg(e));
    check(fakeMvs::createCount() == before,
          QString("**没去连**那台不相干的相机(建句柄 %1 次)").arg(fakeMvs::createCount()));
}

// ============================================================
// 用例 10:SDK 每一步失败都要说出来
// ============================================================
static void test_sdk_step_failures() {
    beginCase("10. SDK 每一步失败都要说出来");

    // 每一步都用**不同的 IP** —— 换了 IP 才会重新连接,
    // 否则上一轮连好的会话会被直接复用,压根走不到那一步。
    struct Case {
        const char* what;
        const char* ip;
        void (*setup)();
        const char* expect;
    };
    const Case cases[] = {
        {"建句柄失败", "10.1.0.1", []() { fakeMvs::setCreateRet(-2); }, "CreateHandle"},
        {"打开设备失败", "10.1.0.2", []() { fakeMvs::setOpenRet(-3); }, "OpenDevice"},
        {"开始取流失败", "10.1.0.3", []() { fakeMvs::setStartGrabRet(-4); }, "StartGrabbing"},
    };

    QStringList messages;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        resetAll();
        scriptDevice(cases[i].ip);
        scriptFrame();
        cases[i].setup();

        const QJsonObject out = runCam(mkParams(cases[i].ip));
        const QString e = errOf(out);
        check(out.contains("error"), QString("%1:报了错").arg(cases[i].what));
        check(e.contains(cases[i].expect),
              QString("%1:点名了是哪一步(实际 \"%2\")").arg(cases[i].what).arg(e));
        check(fakeMvs::openHandles() == 0,
              QString("%1:半开的句柄也还回去了(实际还开着 %1 个)")
                  .arg(cases[i].what).arg(fakeMvs::openHandles()));
        messages << e;
    }

    // 设参数失败:那两步在**每次**取帧前都会走,不用换 IP
    resetAll();
    scriptDevice("10.1.0.4");
    scriptFrame();
    fakeMvs::setSetEnumRet(-5);
    {
        const QJsonObject out = runCam(mkParams("10.1.0.4"));
        const QString e = errOf(out);
        check(e.contains("TriggerMode"),
              QString("设 TriggerMode 失败要报错(实际 \"%1\")").arg(e));
        messages << e;
    }

    resetAll();
    scriptDevice("10.1.0.5");
    scriptFrame();
    fakeMvs::setSetFloatRet(-6);
    {
        const QJsonObject out = runCam(mkParams("10.1.0.5"));
        const QString e = errOf(out);
        check(e.contains("ExposureTime"),
              QString("设 ExposureTime 失败要报错(实际 \"%1\")").arg(e));
        messages << e;
    }

    resetAll();
    scriptDevice("10.1.0.6");
    scriptFrame();
    fakeMvs::setSetCommandRet(-7);
    {
        const QJsonObject out = runCam(mkParams("10.1.0.6", 5.0, 1));
        const QString e = errOf(out);
        check(e.contains("软触发"),
              QString("软触发命令失败要报错(实际 \"%1\")").arg(e));
        messages << e;
    }

    bool allDifferent = true;
    for (int i = 0; i < messages.size(); ++i)
        for (int j = i + 1; j < messages.size(); ++j)
            if (messages[i] == messages[j]) allDifferent = false;
    check(allDifferent, "每一步的报错**互不相同**(不然有些用例是恒真的)");
}

// ============================================================
// 用例 11:软触发 —— 两件事都要做
// ============================================================
static void test_soft_trigger() {
    beginCase("11. 软触发");

    resetAll();
    scriptDevice("10.0.0.8");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));

    const QJsonObject out = runCam(mkParams("10.0.0.8", 5.0, 1));
    check(!out.contains("error"), QString("没报错(实际 \"%1\")").arg(errOf(out)));

    check(fakeMvs::lastEnumValueFor("TriggerMode") == MV_TRIGGER_MODE_ON,
          QString("TriggerMode 设成了 ON(实际 0x%1)")
              .arg(fakeMvs::lastEnumValueFor("TriggerMode"), 8, 16, QChar('0')));
    check(fakeMvs::enumCallCountFor("TriggerSource") == 1,
          QString("**TriggerSource 也设了**(实际 %1 次)—— "
                  "只设 TriggerMode 的话相机在等外部信号,永远不出图")
              .arg(fakeMvs::enumCallCountFor("TriggerSource")));
    check(fakeMvs::lastEnumValueFor("TriggerSource") == MV_TRIGGER_SOURCE_SOFTWARE,
          QString("TriggerSource 设成了 SOFTWARE(实际 0x%1)")
              .arg(fakeMvs::lastEnumValueFor("TriggerSource"), 8, 16, QChar('0')));
    check(fakeMvs::setCommandCount() == 1,
          QString("发了一次 TriggerSoftware 命令(实际 %1 次)").arg(fakeMvs::setCommandCount()));
    check(fakeMvs::lastCommandKey() == "TriggerSoftware",
          QString("命令名是 TriggerSoftware(实际 \"%1\")")
              .arg(QString::fromStdString(fakeMvs::lastCommandKey())));
}

// ============================================================
// 用例 12:改了 IP 要重连,改了触发模式要生效
// ============================================================
static void test_reconnect_and_reapply() {
    beginCase("12. 改了 IP 要重连 / 改了触发模式要生效");

    resetAll();
    scriptDevice("10.0.0.1");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));

    runCam(mkParams("10.0.0.1", 5.0, 0));
    check(fakeMvs::openCount() == 1,
          QString("第一次连了一次(实际 %1)").arg(fakeMvs::openCount()));
    check(fakeMvs::destroyCount() == 0,
          QString("还没释放过句柄(实际 %1 次)").arg(fakeMvs::destroyCount()));

    // 同一个 IP 再跑一次:不该重连
    runCam(mkParams("10.0.0.1", 5.0, 0));
    check(fakeMvs::openCount() == 1,
          QString("同一个 IP 不该重连(实际连了 %1 次)").arg(fakeMvs::openCount()));

    // 换 IP:要重连,而且**旧的要先关掉**
    resetAll();
    {
        fakeMvs::Device a; a.ip = fakeMvs::ipToUint("10.0.0.1");
        fakeMvs::Device b; b.ip = fakeMvs::ipToUint("10.0.0.2");
        fakeMvs::setDevices(std::vector<fakeMvs::Device>{a, b});
    }
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));

    runCam(mkParams("10.0.0.1", 5.0, 0));
    const int opensAfterFirst = fakeMvs::openCount();
    runCam(mkParams("10.0.0.2", 5.0, 0));
    check(fakeMvs::openCount() == opensAfterFirst + 1,
          QString("换 IP 之后重连了(实际连了 %1 次)").arg(fakeMvs::openCount()));
    check(fakeMvs::destroyCount() == 1,
          QString("旧句柄被释放了(实际释放 %1 次)—— 不释放就是漏")
              .arg(fakeMvs::destroyCount()));

    // 换 IP 之后,触发模式要**立刻生效**
    runCam(mkParams("10.0.0.2", 5.0, 1));
    check(fakeMvs::lastEnumValueFor("TriggerMode") == MV_TRIGGER_MODE_ON,
          QString("同一台相机上把触发模式改成软触发,下一次就生效了"
                  "(实际 0x%1)")
              .arg(fakeMvs::lastEnumValueFor("TriggerMode"), 8, 16, QChar('0')));
    check(fakeMvs::lastEnumValueFor("TriggerMode") != MV_TRIGGER_MODE_OFF,
          "触发模式不是还停在旧值(旋钮改了要生效)");
}

// ============================================================
// 用例 13:句柄不能漏
// ============================================================
static void test_no_handle_leak() {
    beginCase("13. 句柄不能漏");

    resetAll();
    scriptDevice("10.0.0.9");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));

    runCam(mkParams("10.0.0.9", 5.0, 0));
    check(fakeMvs::openHandles() == 1,
          QString("开着一个句柄(实际 %1)").arg(fakeMvs::openHandles()));

    ImageSourceCamera::releaseSession();
    check(fakeMvs::openHandles() == 0,
          QString("释放之后没有句柄还开着(实际 %1)").arg(fakeMvs::openHandles()));
    check(fakeMvs::stopGrabCount() >= 1,
          QString("停过取流(实际 %1 次)").arg(fakeMvs::stopGrabCount()));
    check(fakeMvs::closeCount() >= 1,
          QString("关过设备(实际 %1 次)").arg(fakeMvs::closeCount()));
    check(fakeMvs::destroyCount() >= 1,
          QString("销毁过句柄(实际 %1 次)").arg(fakeMvs::destroyCount()));
}

// ============================================================
// 用例 14:旋钮类型不对要点名旋钮(不是 SDK 的错)
// ============================================================
static void test_knob_type_errors() {
    beginCase("14. 旋钮类型不对要点名旋钮");

    resetAll();
    scriptDevice("10.0.0.10");
    fakeMvs::setPixelType(PixelType_Gvsp_BGR8_Packed);
    fakeMvs::setFrameSize(2, 2);
    fakeMvs::setFrameData(std::vector<unsigned char>(2 * 2 * 3, 3));

    QJsonObject p = mkParams("10.0.0.10");
    p["camera_ip"] = 12345;                 // IP 存成了数字
    const QJsonObject out = runCam(p);
    const QString e = errOf(out);
    check(out.contains("error"), "报了错");
    check(e.contains("camera_ip"),
          QString("点名了是哪个旋钮(实际 \"%1\")").arg(e));
    check(!e.contains("未发现 GigE 相机"),
          QString("说的是\"旋钮类型不对\",不是\"SDK 没装/没发现相机\"(实际 \"%1\")").arg(e));
    check(fakeMvs::enumCount() == 0,
          QString("旋钮就不对,压根没去碰 SDK(实际碰了 %1 次)").arg(fakeMvs::enumCount()));
}

int main(int argc, char* argv[]) {
    // ⚠ 必须在 QApplication 构造**之前**设好。
    //   image_source_camera 拖了 QtWidgets(参数面板),链接的是 Widgets。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 image_source_camera 相机路径 回归测试(假 MVS SDK)";

    test_runtime_is_available();
    test_bgr8_frame();
    test_mono8_frame();
    test_bayer_uses_the_right_code();
    test_unsupported_pixel_format();
    test_get_buffer_fails();
    test_buffer_too_small();
    test_enum_failures();
    test_ip_must_match();
    test_sdk_step_failures();
    test_soft_trigger();
    test_reconnect_and_reapply();
    test_no_handle_leak();
    test_knob_type_errors();

    cleanupTempFiles();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
