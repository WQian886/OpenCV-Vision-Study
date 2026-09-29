// ============================================================
// W7 图像源 — 无头回归测试
//
// 为什么需要它:
//   本次给 Run 引擎加了「帧循环」(文件夹源 / 视频源共用),
//   契约是:源算子输出 frame_count,引擎按 inputs["frame_index"]
//   逐轮调用,源据此返回第几张/第几帧。
//
//   这个契约以前是写在注释里但没实现的 —— image_source_dir
//   的注释说"每张图都让下游算子跑一次",实际永远只跑第一张。
//   注释会骗人,断言不会。
//
// 测试自己用 VideoWriter 造一段临时视频,不依赖任何外部素材。
// ============================================================

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <windows.h>

#include "operators/image_source_video.h"
#include "operators/image_source_dir.h"
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

using namespace w7;

// ============================================================
// 极简断言(与前三个测试同一套风格)
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
// 造一段 5 帧的测试视频:每帧整片填不同灰度值,肉眼和程序都能分辨
// ============================================================
static const char* kTestVideo = "images/_test_src.avi";
static const int kNumFrames = 5;

static bool makeTestVideo() {
    QDir().mkpath("images");
    cv::VideoWriter w(kTestVideo, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                      25.0, cv::Size(160, 120));
    if (!w.isOpened()) return false;
    for (int i = 0; i < kNumFrames; ++i) {
        cv::Mat f(120, 160, CV_8UC3, cv::Scalar(30 + i * 40, 30 + i * 40, 30 + i * 40));
        w.write(f);
    }
    w.release();
    return QFile::exists(kTestVideo);
}

// 读回快照的灰度,用来确认「拿到的是第几帧」
static int snapshotGray(const QString& path) {
    cv::Mat m = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
    if (m.empty()) return -1;
    // 注意:cvtColor 原地重载返回 void,不能链式调 .at()
    cv::Mat gray;
    cv::cvtColor(m, gray, cv::COLOR_BGR2GRAY);
    return gray.at<uchar>(0, 0);
}

// ------------------------------------------------------------
// 用例 1:按 frame_index 取到对应的帧
// 这是整个帧循环的地基:引擎换帧,源必须真的换帧
// ------------------------------------------------------------
static void test_video_frame_index() {
    beginCase("视频源按 frame_index 取对应帧");
    ImageSourceVideo src;
    const QJsonObject p = src.defaultParams();
    QJsonObject p2 = p;
    p2["video_path"] = kTestVideo;
    p2["max_frames"] = kNumFrames;

    QJsonObject first = src.run(p2, QJsonObject{{"frame_index", 0}});
    check(!first.contains("error"),
          QString("第 0 帧无错误 (%1)").arg(first.value("error").toString("无")));
    check(first.value("frame_count").toInt() == kNumFrames,
          QString("frame_count = %1 (期望 %2)")
              .arg(first.value("frame_count").toInt()).arg(kNumFrames));
    check(first.value("current_index").toInt() == 0, "current_index = 0");

    const int gray0 = snapshotGray(first.value("image_path").toString());
    check(gray0 >= 0, "第 0 帧快照可读回");

    // 逐帧取,灰度应该一次比一次亮 40 —— 直接验证"没每次都返回第 0 帧"
    bool allDistinct = true;
    for (int i = 1; i < kNumFrames; ++i) {
        QJsonObject o = src.run(p2, QJsonObject{{"frame_index", i}});
        if (o.contains("error")) { allDistinct = false; break; }
        const int g = snapshotGray(o.value("image_path").toString());
        if (g <= gray0) allDistinct = false;
    }
    check(allDistinct, "5 帧内容各不相同(没每次都返回第 0 帧)");
}

// ------------------------------------------------------------
// 用例 2:快照真的落盘了
// 视频帧没有"路径",而下游算子只认 image_path ——
// 源不落盘,整条链在视频源后面就断了
// ------------------------------------------------------------
static void test_video_snapshot_on_disk() {
    beginCase("视频帧快照落盘");
    ImageSourceVideo src;
    QJsonObject p = src.defaultParams();
    p["video_path"] = kTestVideo;
    QJsonObject o = src.run(p, QJsonObject{{"frame_index", 2}});
    const QString path = o.value("image_path").toString();
    check(!path.isEmpty(), "有 image_path 输出");
    check(QFile::exists(path), QString("快照文件存在: %1").arg(path));
}

// ------------------------------------------------------------
// 用例 3:越界 / 文件不存在 → 友好错误,不崩
// 产线上算子不能因为一个坏输入把整条流水线带崩
// ------------------------------------------------------------
static void test_video_errors() {
    beginCase("视频源错误处理");
    ImageSourceVideo src;

    QJsonObject missing = src.defaultParams();
    missing["video_path"] = "images/根本没有这个文件.avi";
    QJsonObject o1 = src.run(missing, QJsonObject{{"frame_index", 0}});
    check(o1.value("error").toString().contains("不存在"),
          QString("文件不存在有提示 (实际 '%1')").arg(o1.value("error").toString()));

    QJsonObject ok = src.defaultParams();
    ok["video_path"] = kTestVideo;
    QJsonObject o2 = src.run(ok, QJsonObject{{"frame_index", 9999}});
    check(o2.value("error").toString().contains("读取失败"),
          QString("越界索引有提示 (实际 '%1')").arg(o2.value("error").toString()));

    // 不是视频的文件(拿 png 冒充)也该挡下来
    QJsonObject notVideo = src.defaultParams();
    notVideo["video_path"] = "images/baseline.png";
    QJsonObject o3 = src.run(notVideo, QJsonObject{{"frame_index", 0}});
    check(o3.contains("error"),
          QString("非视频文件被挡下 (error='%1')").arg(o3.value("error").toString()));
}

// ------------------------------------------------------------
// 用例 4:文件夹源按 frame_index 取第 N 张
//
// 这一条是本次改动里最该被测的:以前写死 index 0,
// 注释宣称"每张图都跑一遍"但实际只跑第一张。
// 引擎现在按 frame_index 逐张问,源必须真的换张。
// ------------------------------------------------------------
static void test_dir_frame_index() {
    beginCase("文件夹源按 frame_index 取第 N 张");
    ImageSourceDir src;
    QJsonObject p = src.defaultParams();
    p["dir_path"] = "images";
    p["pattern"]  = "_srcframe_*.png";

    // 先造 4 张名字可排序的图
    for (int i = 0; i < 4; ++i) {
        cv::Mat m(40, 40, CV_8UC3, cv::Scalar(10 + i * 20, 0, 0));
        cv::imwrite(QString("images/_srcframe_%1.png").arg(i, 2, 10, QChar('0')).toStdString(), m);
    }

    QJsonObject o0 = src.run(p, QJsonObject{{"frame_index", 0}});
    QJsonObject o1 = src.run(p, QJsonObject{{"frame_index", 1}});
    QJsonObject o2 = src.run(p, QJsonObject{{"frame_index", 2}});

    check(o0.value("frame_count").toInt() == 4,
          QString("frame_count = 4 (实际 %1)").arg(o0.value("frame_count").toInt()));
    check(o0.value("image_path").toString() != o1.value("image_path").toString(),
          "index 0 和 1 取到不同文件");
    check(o1.value("image_path").toString() != o2.value("image_path").toString(),
          "index 1 和 2 取到不同文件");
    check(o2.value("current_index").toInt() == 2, "current_index = 2");

    // 越界停在最后一张,而不是报错 —— 抽检语义
    QJsonObject o3  = src.run(p, QJsonObject{{"frame_index", 3}});
    QJsonObject o99 = src.run(p, QJsonObject{{"frame_index", 99}});
    check(!o99.contains("error"),
          QString("越界索引不报错 (error='%1')").arg(o99.value("error").toString()));
    check(o99.value("image_path").toString() == o3.value("image_path").toString(),
          "越界索引回落到最后一张");
    check(o99.value("current_index").toInt() == 3,
          QString("current_index 回落到 3 (实际 %1)")
              .arg(o99.value("current_index").toInt()));
}

// ------------------------------------------------------------
// 用例 5:空目录 / 无匹配 → 友好错误
// ------------------------------------------------------------
static void test_dir_errors() {
    beginCase("文件夹源错误处理");
    ImageSourceDir src;

    QJsonObject noDir = src.defaultParams();
    noDir["dir_path"] = "images/根本不存在的目录";
    QJsonObject o1 = src.run(noDir, QJsonObject{{"frame_index", 0}});
    check(o1.value("error").toString().contains("不存在"),
          QString("目录不存在有提示 (实际 '%1')").arg(o1.value("error").toString()));

    QJsonObject noMatch = src.defaultParams();
    noMatch["dir_path"] = "images";
    noMatch["pattern"]  = "*.根本不存在的扩展名";
    QJsonObject o2 = src.run(noMatch, QJsonObject{{"frame_index", 0}});
    check(o2.value("error").toString().contains("为空"),
          QString("无匹配有提示 (实际 '%1')").arg(o2.value("error").toString()));
}

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    SetConsoleOutputCP(CP_UTF8);
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 图像源 回归测试(帧循环契约)";

    if (!makeTestVideo()) {
        qWarning() << "无法创建测试视频(VideoWriter 打不开),剩余用例跳过";
        qInfo().noquote() << "SUMMARY: pass=0 fail=1  RESULT=FAIL";
        return 1;
    }
    beginCase("测试视频生成");
    check(QFile::exists(kTestVideo), "临时测试视频已生成");

    test_video_frame_index();
    test_video_snapshot_on_disk();
    test_video_errors();
    test_dir_frame_index();
    test_dir_errors();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
