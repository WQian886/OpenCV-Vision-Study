// ============================================================
// W7 dl_infer 推理路径 的回归测试(假 ONNX Runtime)
// ============================================================
//
// 测的是:dl_infer 那半边 #ifdef HAS_ONNXRUNTIME 的代码。
//
// ⚠ 这半边代码在写这个套件之前**从来没有被编译过一次** ——
//   本机没装 ONNX Runtime,CMake 走空实现,那半边根本不参与编译。
//   于是"推理跑不通"这件事,连"是代码错了"还是"是环境缺了"都分不清。
//
// 这里用**假 ONNX Runtime**(third_party/fake_onnxruntime/)顶上:
//   · 推理路径**真的被编译、真的被跑**
//   · 模型吐出来的张量是**脚本**给的 -> 想让它吐什么就吐什么
//   · 送进去的张量被**记下来** -> 预处理做对没有,一眼看得见
//
// 拿假的算子去验"真算子接得上吗"是自欺欺人;
// 但拿**真的 dl_infer 源码**跑**假 SDK**,验的是 dl_infer 自己的逻辑 ——
// 那是本项目的目的。
//
// ============================================================
// 它抓到了什么(写这套测试之前没人知道)
// ============================================================
//
// 1. **张量的形状是 NCHW,数据却是 HWC**。
//    形状写的是 {1,3,H,W},喂进去的却是 cv::Mat 的行优先 HWC ——
//    通道被打散,模型看到的是"红红红绿绿绿蓝蓝蓝"三张图。
//    后果:不报错、不崩溃,只是**全检测不出来**。
//    (输出那半边解包成 (通道, 框) 是**对的** —— 两边一对照,
//     输入这半边就露馅了。)
//
// 2. **输入名的指针悬空**。
//    `session.GetInputNameAllocated(0, ...).get()` —— 那个临时对象
//    整条语句结束就死了, 指针指向已释放的内存。
//    真 SDK 上是 use-after-free(而且不崩, 只是名字可能变成乱码);
//    假 SDK 把释放掉的内存清零, 于是它变成一个**确定**的错。
//
// 3. **模型输出的维度没检查**。
//    输出不是 3 维时, 代码直接读 oshape[1]/oshape[2] —— 越界, 崩。
//
// ============================================================

#include "operators/dl_infer.h"

// ⚠ 这是**假**的 ONNX Runtime —— 头文件在 third_party/fake_onnxruntime/。
//   CMake 把那个目录加进本目标的 include 路径。
#include <onnxruntime_cxx_api.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QDebug>
#include <QApplication>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <exception>

using namespace w7;

// ============================================================
// ⚠ 编译期守卫:这个套件**必须**跑在"带假 SDK"的那份 dl_infer 上。
//   万一 CMake 哪次改坏了(比如忘了定义 HAS_ONNXRUNTIME),
//   这个套件会**测到空实现** —— 而空实现里根本没有推理,
//   断言会"莫名其妙地全红",或者更糟:某些断言恒真。
//   宁可编译不过,也不要测错东西。
// ============================================================
#ifndef W7_ONNXRUNTIME_FAKE
#error "这个套件必须在 HAS_ONNXRUNTIME + 假 SDK 下编译(见 CMakeLists 的 w7_dl_infer_test)"
#endif
#ifndef HAS_ONNXRUNTIME
#error "这个套件必须在定义了 HAS_ONNXRUNTIME 的情况下编译"
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
static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// ============================================================
// 测试数据
// ============================================================
//
// ⚠ 数字是**算出来的**,不是抄来的 —— 见下面每处的注释。
//   这套测试的每个期望值都能从
//   「letterbox 的算式 + toOriginal 的算式」手推出来。
//
// 图像: 20 x 10, 纯色 BGR(10, 20, 30)
// 网络输入: 32 x 32
//   scale = min(32/20, 32/10) = 1.6
//   newW  = round(20*1.6) = 32   newH = round(10*1.6) = 16
//   padX  = (32-32)/2 = 0        padY = (32-16)/2 = 8
//   内容区 = 第 8..23 行(全宽), 其余是 114 灰边
// 归一化后:
//   R = 30/255   G = 20/255   B = 10/255   边 = 114/255
//
static const int kImgW = 20, kImgH = 10;
static const int kNetW = 32, kNetH = 32;
static const float kScale = 1.6f;
static const int   kPadX = 0, kPadY = 8;
static const float kR = 30.f / 255.f;
static const float kG = 20.f / 255.f;
static const float kB = 10.f / 255.f;
static const float kPad = 114.f / 255.f;

// 检测框(网络坐标):中心 (16,24), 宽高 (16,16)
//   x = (16 - 0)/1.6 = 10   y = (24 - 8)/1.6 = 10
//   w = 16/1.6 = 10          h = 16/1.6 = 10
//   裁到原图(20x10): x1=max(0,10-5)=5  y1=max(0,10-5)=5
//                     x2=min(20,10+5)=15 y2=min(10,10+5)=10
//   -> 框 = (5, 5, 10, 5)
static const int kBoxX = 5, kBoxY = 5, kBoxW = 10, kBoxH = 5;

static QString makeImage(const QString& name) {
    const QString path = QDir::current().filePath(name);
    cv::Mat img(kImgH, kImgW, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::imwrite(path.toStdString(), img);
    return path;
}

// 假模型文件:假 SDK 只检查"文件在不在",不看内容。
static QString makeModel(const QString& name) {
    const QString path = QDir::current().filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("not a real model, the fake SDK only checks existence");
        f.close();
    }
    return path;
}

static QJsonObject mkParams(const QString& modelPath) {
    QJsonObject p;
    p["model_path"]     = modelPath;
    p["input_w"]        = kNetW;
    p["input_h"]        = kNetH;
    p["conf_threshold"] = 0.25;
    p["nms_iou"]        = 0.45;
    p["class_names"]    = "person";
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

// --- 脚本化的模型输出 ---

// yolov8: 形状 (1, 4+类别数, 框数) -> 去掉 batch 是 (通道, 框)
//   通道布局: [cx, cy, w, h, 类别分数...]
static void scriptYolov8(float score) {
    fakeOrt::setOutputTensor({1, 5, 1}, {16.f, 24.f, 16.f, 16.f, score});
}

// yolov5: 形状 (1, 框数, 5+类别数) -> 去掉 batch 是 (框, 属性)
//   属性布局: [cx, cy, w, h, objectness, 类别分数...]
static void scriptYolov5(float obj, float cls) {
    fakeOrt::setOutputTensor({1, 1, 6}, {16.f, 24.f, 16.f, 16.f, obj, cls});
}

// --- 跑一次,别让它把异常带到外面 ---
struct RunOutcome {
    bool threw = false;
    QString what;
    QJsonObject out;
};
static RunOutcome runDl(DlInfer& op, const QJsonObject& params, const QJsonObject& inputs) {
    RunOutcome r;
    try { r.out = op.run(params, inputs); }
    catch (const std::exception& e) { r.threw = true; r.what = QString::fromUtf8(e.what()); }
    catch (...) { r.threw = true; r.what = QStringLiteral("非 std::exception 的异常"); }
    return r;
}

// ============================================================
// 用例 1:推理路径整体跑通
// ============================================================
static void test_inference_runs() {
    beginCase("1. 推理路径整体跑通(以前这半边代码一次都没编译过)");

    fakeOrt::reset();
    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");
    scriptYolov8(0.9f);

    const RunOutcome r = runDl(op, mkParams(model), mkInputs(img));

    check(!r.threw, QString("没抛异常(实际 \"%1\")").arg(r.what));
    check(!r.out.contains("error"),
          QString("没有 error(实际 \"%1\")").arg(r.out.value("error").toString()));
    check(r.out.value("count").toInt() == 1,
          QString("检出 1 个框(实际 %1)").arg(r.out.value("count").toInt()));
    check(fakeOrt::runCount() == 1,
          QString("模型真的被调了一次(实际 %1)").arg(fakeOrt::runCount()));

    // 检测框
    const QJsonArray dets = r.out.value("detections").toArray();
    check(dets.size() == 1, QString("detections 里有 1 项(实际 %1)").arg(dets.size()));
    if (dets.size() == 1) {
        const QJsonObject d = dets.at(0).toObject();
        check(d.value("class_id").toInt() == 0, "class_id = 0");
        check(d.value("class").toString() == "person",
              QString("class = person(实际 \"%1\")").arg(d.value("class").toString()));
        check(near(float(d.value("conf").toDouble()), 0.9f),
              QString("conf = 0.9(实际 %1)").arg(float(d.value("conf").toDouble())));
        check(d.value("x").toInt() == kBoxX && d.value("y").toInt() == kBoxY &&
              d.value("w").toInt() == kBoxW && d.value("h").toInt() == kBoxH,
              QString("框 = (%1,%2,%3,%4)(实际 %5,%6,%7,%8)")
                  .arg(kBoxX).arg(kBoxY).arg(kBoxW).arg(kBoxH)
                  .arg(d.value("x").toInt()).arg(d.value("y").toInt())
                  .arg(d.value("w").toInt()).arg(d.value("h").toInt()));
    }

    // 其余字段
    check(r.out.value("image_path").toString() == img, "回显了 image_path");
    check(r.out.value("model").toString() == model, "回显了 model");
    check(r.out.value("layout").toString() == "yolov8", "回显了 layout");
    check(r.out.value("inference_ms").toDouble() >= 0.0, "inference_ms 是个数");

    // 标注图真的写盘了
    const QString marked = r.out.value("marked_path").toString();
    check(!marked.isEmpty() && QFile::exists(marked),
          QString("标注图写盘了(%1)").arg(marked));
}

// ============================================================
// 用例 2:预处理做对没有 —— 这才是最容易错的那一半
// ============================================================
static void test_preprocessing() {
    beginCase("2. 预处理:letterbox + BGR2RGB + 归一化 + NCHW");

    fakeOrt::reset();
    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");
    scriptYolov8(0.9f);
    runDl(op, mkParams(model), mkInputs(img));

    const std::vector<int64_t> shape = fakeOrt::lastInputShape();
    check(shape.size() == 4 && shape[0] == 1 && shape[1] == 3 &&
          shape[2] == kNetH && shape[3] == kNetW,
          QString("张量形状是 NCHW (1,3,%1,%2)(实际 %3)").arg(kNetH).arg(kNetW)
              .arg(shape.size() == 4
                       ? QString("%1,%2,%3,%4").arg(shape[0]).arg(shape[1])
                             .arg(shape[2]).arg(shape[3])
                       : QString("不是 4 维")));

    const std::vector<float> d = fakeOrt::lastInputData();
    const size_t plane = static_cast<size_t>(kNetH) * kNetW;
    if (d.size() != 3 * plane) {
        check(false, QString("张量数据量对(实际 %1,期望 %2)").arg(d.size()).arg(3 * plane));
        return;
    }
    check(true, "张量数据量对");

    // 通道 0 必须是 **R 平面**:值只有 R 的内容值和边值两种。
    // ⚠ 通道要是被打散(HWC 塞进 NCHW),这里就会混进 G/B 的值。
    int rPlaneBad = 0, gPlaneBad = 0, bPlaneBad = 0;
    for (size_t i = 0; i < plane; ++i) {
        const float rv = d[0 * plane + i];
        const float gv = d[1 * plane + i];
        const float bv = d[2 * plane + i];
        if (!near(rv, kR) && !near(rv, kPad)) ++rPlaneBad;
        if (!near(gv, kG) && !near(gv, kPad)) ++gPlaneBad;
        if (!near(bv, kB) && !near(bv, kPad)) ++bPlaneBad;
    }
    check(rPlaneBad == 0,
          QString("通道 0 是 R 平面(混进了 %1 个别的通道的值)").arg(rPlaneBad));
    check(gPlaneBad == 0,
          QString("通道 1 是 G 平面(混进了 %1 个别的通道的值)").arg(gPlaneBad));
    check(bPlaneBad == 0,
          QString("通道 2 是 B 平面(混进了 %1 个别的通道的值)").arg(bPlaneBad));

    // 具体取值
    const size_t contentRow = 8;   // 第一个内容行
    check(near(d[0 * plane + contentRow * kNetW + 0], kR),
          QString("R 通道在内容区 = %1(实际 %2)").arg(kR).arg(d[0 * plane + contentRow * kNetW + 0]));
    check(near(d[1 * plane + contentRow * kNetW + 0], kG),
          QString("G 通道在内容区 = %1(实际 %2)").arg(kG).arg(d[1 * plane + contentRow * kNetW + 0]));
    check(near(d[2 * plane + contentRow * kNetW + 0], kB),
          QString("B 通道在内容区 = %1(实际 %2)").arg(kB).arg(d[2 * plane + contentRow * kNetW + 0]));
    check(near(d[0 * plane + 0 * kNetW + 0], kPad),
          QString("补边是 114 灰(实际 %1)").arg(d[0 * plane + 0 * kNetW + 0]));

    // 归一化:所有值都得在 0..1
    bool allInRange = true;
    for (float v : d) if (v < -1e-6f || v > 1.f + 1e-6f) { allInRange = false; break; }
    check(allInRange, "所有值都在 0..1(归一化做了)");
}

// ============================================================
// 用例 3:yolov5 布局
// ============================================================
static void test_yolov5_layout() {
    beginCase("3. yolov5 布局(和 v8 长得完全不一样)");

    fakeOrt::reset();
    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");
    scriptYolov5(0.9f, 0.8f);

    QJsonObject p = mkParams(model);
    p["output_layout"] = "yolov5";
    const RunOutcome r = runDl(op, p, mkInputs(img));

    check(!r.threw, QString("没抛异常(实际 \"%1\")").arg(r.what));
    check(r.out.value("count").toInt() == 1,
          QString("检出 1 个框(实际 %1)").arg(r.out.value("count").toInt()));
    if (r.out.value("count").toInt() == 1) {
        const QJsonObject d = r.out.value("detections").toArray().at(0).toObject();
        // ⚠ v5 是**两级**相乘: objectness * 类别分数 = 0.9*0.8 = 0.72
        check(near(float(d.value("conf").toDouble()), 0.72f),
              QString("conf = obj*cls = 0.72(实际 %1)").arg(float(d.value("conf").toDouble())));
        check(d.value("x").toInt() == kBoxX && d.value("y").toInt() == kBoxY &&
              d.value("w").toInt() == kBoxW && d.value("h").toInt() == kBoxH,
              QString("框和 v8 一样(实际 %1,%2,%3,%4)")
                  .arg(d.value("x").toInt()).arg(d.value("y").toInt())
                  .arg(d.value("w").toInt()).arg(d.value("h").toInt()));
    }
}

// ============================================================
// 用例 4:阈值和布局真的在被读
// ============================================================
static void test_thresholds_and_layout_are_read() {
    beginCase("4. 阈值和布局真的在被读(不然这条用例就是空转)");

    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");

    // 阈值调到 0.95 -> 0.9 的框该被滤掉
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);
        QJsonObject p = mkParams(model);
        p["conf_threshold"] = 0.95;
        const RunOutcome r = runDl(op, p, mkInputs(img));
        check(r.out.value("count").toInt() == 0,
              QString("阈值调到 0.95 -> 检出 0 个(实际 %1)")
                  .arg(r.out.value("count").toInt()));
    }

    // 布局配错 -> 解析不出来
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);          // v8 的数据
        QJsonObject p = mkParams(model);
        p["output_layout"] = "yolov5";  // 却按 v5 解析
        const RunOutcome r = runDl(op, p, mkInputs(img));
        check(r.out.value("count").toInt() == 0,
              QString("用 v5 的布局去读 v8 的数据 -> 检出 0 个(实际 %1)")
                  .arg(r.out.value("count").toInt()));
    }
}

// ============================================================
// 用例 5:NMS(同类的重框要合并,不同类的不能合并)
// ============================================================
static void test_nms() {
    beginCase("5. NMS:同类的重框合成一个,不同类的各留各的");

    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");

    // 两个**完全重合**的框,同一个类别
    {
        fakeOrt::reset();
        // yolov8 形状 (1, 4+类别数, 框数) -> 二维视图 (通道, 框)
        //   行 = 通道, 列 = 第几个框 —— 所以是按行填的:
        //   [cx0, cx1] / [cy0, cy1] / [w0, w1] / [h0, h1] / [分数0, 分数1]
        fakeOrt::setOutputTensor({1, 5, 2},
                                 {16.f, 16.f,  24.f, 24.f,  16.f, 16.f,
                                  16.f, 16.f,  0.9f, 0.9f});
        const RunOutcome r = runDl(op, mkParams(model), mkInputs(img));
        check(r.out.value("count").toInt() == 1,
              QString("两个重合的同类框 -> 合成 1 个(实际 %1)")
                  .arg(r.out.value("count").toInt()));
    }

    // 两个**完全重合**的框,但类别不同 -> 不许合并
    // (并排的两个人不该被 NMS 合成一个)
    {
        fakeOrt::reset();
        // 4 + 2 类 = 6 个通道:最后两行是各框的类别分数
        fakeOrt::setOutputTensor({1, 6, 2},
                                 {16.f, 16.f,  24.f, 24.f,  16.f, 16.f,
                                  16.f, 16.f,  0.9f, 0.1f,  0.1f, 0.9f});
        QJsonObject p = mkParams(model);
        p["class_names"] = "person,car";
        const RunOutcome r = runDl(op, p, mkInputs(img));
        check(r.out.value("count").toInt() == 2,
              QString("两个重合的**不同类**框 -> 留 2 个(实际 %1)")
                  .arg(r.out.value("count").toInt()));
    }
}

// ============================================================
// 用例 6:类别名取不到时的兜底
// ============================================================
static void test_class_name_fallback() {
    beginCase("6. 类别名取不到时的兜底(不能空,也不能越界)");

    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");

    // 没给类别名 -> 用 "cls0" 这种编号,不能是空串
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);
        QJsonObject p = mkParams(model);
        p["class_names"] = "";
        const RunOutcome r = runDl(op, p, mkInputs(img));
        const QJsonArray dets = r.out.value("detections").toArray();
        const QString cls = dets.size() == 1
                            ? dets.at(0).toObject().value("class").toString() : QString();
        check(!cls.isEmpty(),
              QString("没给类别名 -> 兜底成编号,不能是空串(实际 \"%1\")").arg(cls));
        check(cls == "cls0",
              QString("兜底是 cls0(实际 \"%1\")").arg(cls));
    }
}

// ============================================================
// 用例 7:错误路径
// ============================================================
static void test_error_paths() {
    beginCase("5. 错误路径:每一种病因都要说清是哪种");

    DlInfer op;
    const QString img = makeImage("dl_in.png");
    const QString model = makeModel("dl_model.onnx");

    // 上游没给图
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);
        QJsonObject in;
        in["node_uuid"] = "u-dl";
        const RunOutcome r = runDl(op, mkParams(model), in);
        check(r.out.value("error").toString().contains("image_path"),
              QString("上游没给图 -> 报错点名 image_path(实际 \"%1\")")
                  .arg(r.out.value("error").toString()));
    }

    // 模型文件不存在
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);
        QJsonObject p = mkParams(QDir::current().filePath("no_such_model.onnx"));
        const RunOutcome r = runDl(op, p, mkInputs(img));
        check(r.out.value("error").toString().contains("模型文件不存在"),
              QString("模型文件不存在 -> 报错(实际 \"%1\")")
                  .arg(r.out.value("error").toString()));
    }

    // model_path 类型不对 -> 点名旋钮
    {
        fakeOrt::reset();
        scriptYolov8(0.9f);
        QJsonObject p = mkParams(model);
        p["model_path"] = 123;
        const RunOutcome r = runDl(op, p, mkInputs(img));
        const QString e = r.out.value("error").toString();
        check(e.contains("model_path") && e.contains("不是文本"),
              QString("model_path 类型不对 -> 点名这个旋钮(实际 \"%1\")").arg(e));
    }

    // 模型加载失败(文件在,但加载不了)
    //
    // ⚠ 这里必须用**全新的模型路径**:
    //   dl_infer 把会话按模型路径缓存起来(static,进程级的),
    //   用例 1 已经把 "dl_model.onnx" 的会话建好了 ——
    //   再用同一个路径,压根不会去"加载",也就不会失败。
    //   这种跨用例的耦合最容易让人以为"测试没生效"。
    {
        fakeOrt::reset();
        const QString freshModel = makeModel("dl_model_fail.onnx");
        scriptYolov8(0.9f);
        fakeOrt::setFailModelLoad(true);
        const RunOutcome r = runDl(op, mkParams(freshModel), mkInputs(img));
        fakeOrt::setFailModelLoad(false);
        const QString e = r.out.value("error").toString();
        check(e.contains("模型加载失败"),
              QString("模型加载失败 -> 报错(实际 \"%1\")").arg(e));
        check(e.contains("fake-ort"),
              QString("报错里带得下底层原因(实际 \"%1\")").arg(e));
    }

    // 模型输出的维度不对 -> 报错,不许崩
    {
        fakeOrt::reset();
        fakeOrt::setOutputTensor({1, 5}, {1.f, 2.f, 3.f, 4.f, 5.f});
        const RunOutcome r = runDl(op, mkParams(model), mkInputs(img));
        check(!r.threw, QString("输出维度不对也不许抛异常(实际 \"%1\")").arg(r.what));
        check(r.out.value("error").toString().contains("维度"),
              QString("输出维度不对 -> 报错说清(实际 \"%1\")")
                  .arg(r.out.value("error").toString()));
    }
}

// ============================================================
// 用例 6:模型只加载一次(缓存)
// ============================================================
static void test_model_cache() {
    beginCase("6. 同一个模型跑两次:模型只该加载一次");

    fakeOrt::reset();
    DlInfer op;
    const QString img = makeImage("dl_in.png");
    // ⚠ 同样必须是**全新的模型路径** —— 会话缓存是进程级的 static,
    //   用例 1 已经把 "dl_model.onnx" 建好了,再用它就只会命中缓存。
    const QString model = makeModel("dl_model_cache.onnx");
    scriptYolov8(0.9f);

    runDl(op, mkParams(model), mkInputs(img));
    const int after1 = fakeOrt::sessionCount();
    runDl(op, mkParams(model), mkInputs(img));
    const int after2 = fakeOrt::sessionCount();

    check(after1 == 1, QString("第一次建了一个会话(实际 %1)").arg(after1));
    check(after2 == 1,
          QString("第二次没再建(实际一共 %1 次)—— 每帧都重新加载几百 MB 权重不可接受")
              .arg(after2));
    check(fakeOrt::runCount() == 2,
          QString("但模型确实被调了两次(实际 %1)").arg(fakeOrt::runCount()));
}

int main(int argc, char* argv[]) {
    // ⚠ 必须在 QApplication 构造**之前**设好。
    //   dl_infer 拖了 QtWidgets(参数面板),链接的是 Widgets。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    qInfo().noquote() << "W7 dl_infer 推理路径 回归测试(假 ONNX Runtime)";

    test_inference_runs();
    test_preprocessing();
    test_yolov5_layout();
    test_thresholds_and_layout_are_read();
    test_nms();
    test_class_name_fallback();
    test_error_paths();
    test_model_cache();

    qInfo().noquote() << "\n================================";
    qInfo().noquote() << "通过:" << g_pass << "  失败:" << g_fail;
    qInfo().noquote() << "================================";
    qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
                      << (g_fail == 0 ? "  RESULT=PASS" : "  RESULT=FAIL");
    return g_fail == 0 ? 0 : 1;
}
