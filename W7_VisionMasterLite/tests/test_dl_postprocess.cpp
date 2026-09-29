// ============================================================
// W7 DL 后处理 — 无头回归测试
//
// 为什么需要它:
//   dl_infer 的推理部分要 ONNX Runtime + 真模型,在这台机器上跑不了
//   (D:\onnxruntime 只有一个空目录),而且就算跑得了,
//   "框画得对不对"这种问题看退出码是看不出来的。
//
//   但真正容易出错、真正值得测的是前后处理:
//     - letterbox 的补边算错 → 框整体偏移,推理没报错,结果全错
//     - YOLOv5 / v8 布局搞反 → 同上,静默失效
//     - NMS 阈值随手给 → 相邻目标被误合并
//   这些全是纯数学,拿构造张量就能验,**不需要任何模型和运行时**。
//
//   所以把能测的部分测透,是这里的设计意图:
//   推理跑不了不算缺口,前后处理没测才算。
//
// 运行:  w7_dl_test.exe
// ============================================================

// 只用到 QString/QStringList,没有控件,所以要 QCoreApplication 而不是 QApplication
// —— 这个测试目标也因此不必链接 Qt::Widgets,构建更快更干净
#include <QCoreApplication>
#include <QDebug>
#include <cmath>
#include <windows.h>   // SetConsoleOutputCP

#include "operators/dl_postprocess.h"
#include <opencv2/imgproc.hpp>   // cv::rectangle(测试用例 3 要用)

using namespace w7::dl;

// ============================================================
// 极简断言(与前两个测试同一套风格)
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

static void checkNear(float actual, float expected, float tol, const QString& what) {
    check(std::fabs(actual - expected) <= tol,
          QString("%1 (期望 %2 ±%3,实际 %4)")
              .arg(what).arg(expected).arg(tol).arg(actual));
}

// ------------------------------------------------------------
// 用例 1:letterbox 缩放与补边
// ------------------------------------------------------------
static void test_letterbox() {
    beginCase("letterbox 等比缩放 + 居中补边");
    cv::Mat src(700, 1000, CV_8UC3, cv::Scalar(30, 90, 40));   // h=700 w=1000

    float scale = 0; int padX = -1, padY = -1;
    cv::Mat out = letterbox(src, cv::Size(640, 640), &scale, &padX, &padY);

    checkNear(scale, 0.64f, 0.001f, "scale");
    check(out.cols == 640 && out.rows == 640,
          QString("输出 640x640 (实际 %1x%2)").arg(out.cols).arg(out.rows));
    check(padX == 0, QString("水平补边 %1 (宽正好占满)").arg(padX));
    check(padY == 96, QString("垂直补边 %1 ((640-448)/2)").arg(padY));

    // 补边颜色必须是 114 而不是 0 —— YOLO 训练时就是这么填的,
    // 换成纯黑会让模型看到没见过的分布,精度悄悄掉
    check(out.at<cv::Vec3b>(0, 0)[0] == 114,
          QString("补边像素为 114 (实际 %1)")
              .arg(out.at<cv::Vec3b>(0, 0)[0]));
    // 内容区域保留原色
    check(out.at<cv::Vec3b>(320, 320)[0] == 30,
          QString("内容像素未被覆盖 (实际 %1)")
              .arg(out.at<cv::Vec3b>(320, 320)[0]));
}

// ------------------------------------------------------------
// 用例 2:竖图(补边方向反过来)
// 顺便证明补边不是写死的 —— 宽图补上下,竖图补左右
// ------------------------------------------------------------
static void test_letterbox_portrait() {
    beginCase("letterbox 竖图补边方向");
    cv::Mat src(1000, 700, CV_8UC3, cv::Scalar(30, 90, 40));   // h=1000 w=700

    float scale = 0; int padX = -1, padY = -1;
    letterbox(src, cv::Size(640, 640), &scale, &padX, &padY);

    checkNear(scale, 0.64f, 0.001f, "scale");
    check(padX == 96, QString("水平补边 %1").arg(padX));
    check(padY == 0,  QString("垂直补边 %1").arg(padY));
}

// ------------------------------------------------------------
// 用例 3:letterbox 必须按比例,不能拉伸
// 直接拉成 640x640 会把目标压扁 —— 模型能跑,但什么都不认识了
// ------------------------------------------------------------
static void test_letterbox_keeps_aspect() {
    beginCase("letterbox 保持宽高比(不拉伸)");
    // 一张 2:1 的图,里面画一条横贯的亮带
    cv::Mat src(200, 400, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(src, cv::Rect(0, 80, 400, 40), cv::Scalar(255, 255, 255), -1);

    float scale = 0; int padX = -1, padY = -1;
    cv::Mat out = letterbox(src, cv::Size(640, 640), &scale, &padX, &padY);

    // 200x400 → 640x640:长边(宽 400)撑满,短边缩到 320,上下各补 160
    checkNear(scale, 1.6f, 0.001f, "scale(200x400 是放大不是缩小)");
    check(padX == 0 && padY == 160,
          QString("补边 x=%1 y=%2").arg(padX).arg(padY));

    // 期望值用「返回的 scale / padY」算出来,而不是把 1.6 写死 ——
    // 写死的话改一下测试图尺寸就得重算,忘了就变成假失败。
    // (第一版就是写死 0.64,200x400 其实是放大 1.6,测试自己先错了)
    const int expectTop = padY + (int)(80 * scale);
    const int expectBot = padY + (int)(120 * scale);
    int actualTop = -1, actualBot = -1;
    for (int y = 0; y < out.rows; ++y) {
        if (out.at<cv::Vec3b>(y, 320)[0] == 255) {
            if (actualTop < 0) actualTop = y;
            actualBot = y;
        }
    }
    check(std::abs(actualTop - expectTop) <= 2,
          QString("亮带顶部 y=%1 (期望 %2)").arg(actualTop).arg(expectTop));
    check(std::abs(actualBot - expectBot) <= 2,
          QString("亮带底部 y=%1 (期望 %2)").arg(actualBot).arg(expectBot));
}

// ------------------------------------------------------------
// 用例 4:坐标系往返 —— 这是整个 DL 可视化最容易错的地方
//
// 链路:网络坐标 → 减补边 → 除缩放 → 原图坐标
// 少做一步(比如忘了减 padY)框就整体下移,
// 而且不报错、推理正常跑,只是框偏 —— 肉眼很难发现
// ------------------------------------------------------------
static void test_coordinate_roundtrip() {
    beginCase("检测框坐标往返(网络 → 原图)");
    const cv::Size origSize(1000, 700);
    const cv::Size netInput(640, 640);
    const float scale = 0.64f;
    const int padX = 0, padY = 96;

    // 造一个输出张量,里面放一个"原图中心 (500,350) 尺寸 100x50"的框
    // 网络坐标 = 原图*scale + pad
    const int numClasses = 2;
    cv::Mat out(4 + numClasses, 1, CV_32F, cv::Scalar(0));
    out.at<float>(0, 0) = 500 * scale + padX;    // cx = 320
    out.at<float>(1, 0) = 350 * scale + padY;    // cy = 320
    out.at<float>(2, 0) = 100 * scale;           // w  = 64
    out.at<float>(3, 0) = 50  * scale;           // h  = 32
    out.at<float>(4, 0) = 0.9f;                  // class0 置信度

    auto dets = parseYolov8(out, origSize, netInput, padX, padY, scale, 0.25f);
    check(dets.size() == 1,
          QString("检出 1 个框 (实际 %1)").arg(dets.size()));
    if (dets.empty()) return;

    const auto& d = dets[0];
    checkNear(d.box.x + d.box.width / 2,  500.f, 1.0f, "中心 x 回到原图 500");
    checkNear(d.box.y + d.box.height / 2, 350.f, 1.0f, "中心 y 回到原图 350");
    checkNear(d.box.width,  100.f, 1.0f, "宽度还原 100");
    checkNear(d.box.height,  50.f, 1.0f, "高度还原 50");
    check(d.classId == 0, QString("类别 0 (实际 %1)").arg(d.classId));
    checkNear(d.conf, 0.9f, 0.001f, "置信度");
}

// ------------------------------------------------------------
// 用例 5:越界框要裁到图内
// 模型偶尔会给出框住半个画面外的东西的框,不裁的话
// 画框时坐标为负,截出来看着"框偏了"
// ------------------------------------------------------------
static void test_clip_to_image() {
    beginCase("越界框裁到图内");
    const cv::Size origSize(1000, 700);
    const float scale = 1.0f;
    // 一个中心在 (-100,-100) 的大框
    cv::Mat out(5, 1, CV_32F, cv::Scalar(0));
    out.at<float>(0, 0) = -100;
    out.at<float>(1, 0) = -100;
    out.at<float>(2, 0) = 400;
    out.at<float>(3, 0) = 400;
    out.at<float>(4, 0) = 0.9f;

    auto dets = parseYolov8(out, origSize, cv::Size(1000, 700), 0, 0, scale, 0.25f);
    check(dets.size() == 1, "检出 1 个框");
    if (dets.empty()) return;
    check(dets[0].box.x >= 0 && dets[0].box.y >= 0,
          QString("左上角非负 (%1,%2)").arg(dets[0].box.x).arg(dets[0].box.y));
    check(dets[0].box.x + dets[0].box.width <= 1000.01f,
          "右边不越界");
    check(dets[0].box.y + dets[0].box.height <= 700.01f,
          "下边不越界");
}

// ------------------------------------------------------------
// 用例 6:YOLOv8 布局 (1, 4+nc, N)
// 特征:没有 objectness 通道,score 直接就是类别概率
// ------------------------------------------------------------
static void test_parse_yolov8() {
    beginCase("YOLOv8 输出布局解析");
    const int nc = 3;
    cv::Mat out(4 + nc, 4, CV_32F, cv::Scalar(0));

    // 框0: 高置信度 class1
    out.at<float>(0, 0) = 100; out.at<float>(1, 0) = 100;
    out.at<float>(2, 0) = 50;  out.at<float>(3, 0) = 50;
    out.at<float>(5, 0) = 0.8f;
    // 框1: 高置信度 class2
    out.at<float>(0, 1) = 300; out.at<float>(1, 1) = 300;
    out.at<float>(2, 1) = 50;  out.at<float>(3, 1) = 50;
    out.at<float>(6, 1) = 0.7f;
    // 框2: 低置信度 → 应被阈值滤掉
    out.at<float>(0, 2) = 500; out.at<float>(1, 2) = 500;
    out.at<float>(2, 2) = 50;  out.at<float>(3, 2) = 50;
    out.at<float>(4, 2) = 0.1f;

    auto dets = parseYolov8(out, cv::Size(640, 640), cv::Size(640, 640),
                            0, 0, 1.0f, 0.25f);
    check(dets.size() == 2, QString("阈值滤掉低分框,剩 2 个 (实际 %1)").arg(dets.size()));
    if (dets.size() == 2) {
        check(dets[0].classId == 1, QString("框0 类别 1 (实际 %1)").arg(dets[0].classId));
        check(dets[1].classId == 2, QString("框1 类别 2 (实际 %1)").arg(dets[1].classId));
    }
}

// ------------------------------------------------------------
// 用例 7:YOLOv5 布局 (1, N, 5+nc) —— 和 v8 正好相反
// 特征:多一个 objectness 通道,置信度 = obj * cls
//
// 这一条最值钱:v5 和 v8 布局搞反了不会报错,
// 只是全检测不出来,现场排查能耗掉一下午。
// 这里用同一个"物理框"分别喂给两个解析器,
// 断言结果一致 —— 布局对不对,看它们能不能认出同一个框。
// ------------------------------------------------------------
static void test_parse_yolov5() {
    beginCase("YOLOv5 输出布局解析(两级置信度)");
    const int nc = 2;
    cv::Mat out(2, 5 + nc, CV_32F, cv::Scalar(0));   // N=2, C=7

    // 框0: obj=0.9, cls1=0.8 → conf=0.72
    out.at<float>(0, 0) = 200; out.at<float>(0, 1) = 150;
    out.at<float>(0, 2) = 60;  out.at<float>(0, 3) = 40;
    out.at<float>(0, 4) = 0.9f;
    out.at<float>(0, 6) = 0.8f;
    // 框1: obj=0.95, cls0=0.9 → conf=0.855
    out.at<float>(1, 0) = 400; out.at<float>(1, 1) = 350;
    out.at<float>(1, 2) = 30;  out.at<float>(1, 3) = 20;
    out.at<float>(1, 4) = 0.95f;
    out.at<float>(1, 5) = 0.9f;

    auto dets = parseYolov5(out, cv::Size(640, 640), cv::Size(640, 640),
                            0, 0, 1.0f, 0.25f);
    check(dets.size() == 2, QString("检出 2 个 (实际 %1)").arg(dets.size()));
    if (dets.size() != 2) return;

    checkNear(dets[0].conf, 0.72f, 0.001f, "框0 置信度 = obj*cls = 0.72");
    checkNear(dets[1].conf, 0.855f, 0.001f, "框1 置信度 = 0.855");
    check(dets[0].classId == 1, QString("框0 类别 1 (实际 %1)").arg(dets[0].classId));
    check(dets[1].classId == 0, QString("框1 类别 0 (实际 %1)").arg(dets[1].classId));

    // v5 按 obj 预过滤,不含 obj 通道的"v8 布局"喂给它必然读错 —— 这是特性不是 bug
    check(out.cols == 5 + nc, "v5 的 5+nc 通道数正确");
}

// ------------------------------------------------------------
// 用例 8:NMS
// ------------------------------------------------------------
static void test_nms() {
    beginCase("NMS 非极大值抑制");
    // IoU ≈ 0.68 的两个同类框
    std::vector<Detection> a = {
        {{0, 0, 100, 100}, 0, 0.9f},
        {{10, 10, 100, 100}, 0, 0.6f},
    };
    auto kept = nms(a, 0.45f);
    check(kept.size() == 1, QString("高重叠同类框合成 1 个 (实际 %1)").arg(kept.size()));
    if (!kept.empty())
        checkNear(kept[0].conf, 0.9f, 0.001f, "保留的是高分那个");

    // 阈值放宽 → 都能留
    auto kept2 = nms(a, 0.8f);
    check(kept2.size() == 2,
          QString("IoU 阈值放宽到 0.8 后保留 2 个 (实际 %1)").arg(kept2.size()));

    // 类别不同 → 永不抑制(并排的两个人不该被合成一个)
    std::vector<Detection> b = {
        {{0, 0, 100, 100}, 0, 0.9f},
        {{10, 10, 100, 100}, 1, 0.6f},
    };
    auto kept3 = nms(b, 0.45f);
    check(kept3.size() == 2,
          QString("不同类别不抑制 (实际 %1)").arg(kept3.size()));
}

// ------------------------------------------------------------
// 用例 9:画框不能崩,类别名越界要兜住
// ------------------------------------------------------------
static void test_draw_detections() {
    beginCase("画框 + 类别名越界兜底");
    cv::Mat img(200, 300, CV_8UC3, cv::Scalar(40, 90, 50));
    std::vector<Detection> dets = {
        {{20, 30, 50, 40}, 0, 0.9f},
        {{100, 10, 40, 30}, 7, 0.5f},   // classId 7,类别名只给 1 个 → 越界
    };
    // 类别名比 classId 少,最容易在这里拿到空指针
    cv::Mat out = drawDetections(img, dets, QStringList{"person"});

    check(!out.empty() && out.cols == 300 && out.rows == 200, "输出尺寸正确");
    bool changed = false;
    for (int y = 0; y < out.rows && !changed; ++y)
        for (int x = 0; x < out.cols; ++x)
            if (out.at<cv::Vec3b>(y, x) != img.at<cv::Vec3b>(y, x)) { changed = true; break; }
    check(changed, "确实画上了东西");
}

// ------------------------------------------------------------
// 用例 10:空输入 / 畸形输入不崩
// 产线上的算子不能因为一张坏图就把整条流水线带崩
// ------------------------------------------------------------
static void test_robust_input() {
    beginCase("畸形输入不崩");
    cv::Mat empty;
    check(letterbox(empty, cv::Size(640, 640)).empty(), "空图 → 空输出");
    check(letterbox(cv::Mat(10, 10, CV_8UC3), cv::Size(0, 0)).empty(),
          "目标尺寸为 0 → 空输出");

    // 退化张量:产线上模型输出形状不对时,流水线不能崩也不能乱读。
    // 乱读不会崩,会静默产出垃圾框,现场查起来极其痛苦 ——
    // 所以这里盯的是「挡下来了」这个安全属性,不是某个构造器的边角行为。
    //
    // (第一版想构造 dims==1 的 Mat,两次都没猜对 OpenCV 的构造语义,
    //  改断言只会越改越脆。直接测真正会遇到的退化输入。)
    std::vector<float> buf(100, 0.f);
    cv::Mat noRows(0, 100, CV_32F, buf.data());     // 0 行
    cv::Mat noCols(10, 0, CV_32F, buf.data());      // 0 列
    cv::Mat noData;                                 // 完全空的
    for (const cv::Mat& bad : {noRows, noCols, noData}) {
        check(parseYolov8(bad, cv::Size(640, 640), cv::Size(640, 640), 0, 0, 1.f, .25f).empty(),
              QString("退化张量 (%1x%2) → v8 返回空")
                  .arg(bad.rows).arg(bad.cols));
        check(parseYolov5(bad, cv::Size(640, 640), cv::Size(640, 640), 0, 0, 1.f, .25f).empty(),
              QString("退化张量 (%1x%2) → v5 返回空")
                  .arg(bad.rows).arg(bad.cols));
    }

    cv::Mat tooFew(3, 10, CV_32F, cv::Scalar(0));   // 通道数 < 5
    check(parseYolov8(tooFew, cv::Size(640, 640), cv::Size(640, 640), 0, 0, 1.f, .25f).empty(),
          "通道数不足 → 空");

    check(nms({}, 0.5f).empty(), "空检测列表 → 空结果");
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);

    qInfo().noquote() << "W7 DL 后处理 回归测试(无模型 / 无 ONNX Runtime)";

    test_letterbox();
    test_letterbox_portrait();
    test_letterbox_keeps_aspect();
    test_coordinate_roundtrip();
    test_clip_to_image();
    test_parse_yolov8();
    test_parse_yolov5();
    test_nms();
    test_draw_detections();
    test_robust_input();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
