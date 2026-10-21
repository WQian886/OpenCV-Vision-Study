// ============================================================
// dl_infer.cpp
// 依赖: ONNX Runtime (onnxruntime_cxx_api.h / onnxruntime.lib)
// 编译条件: ONNXRUNTIME_DIR 环境变量指向 SDK 根目录(默认 D:\onnxruntime)
// 没装 SDK 时此 cpp 的推理部分编译成空实现,算子仍可注册、仍可配参数,
// Run 时给出友好错误 —— W7 其余部分完全不受影响。
// 与 image_source_camera.cpp 的 MVS 处理是同一套模式。
// ============================================================

#include "dl_infer.h"
#include "dl_postprocess.h"
#include "artifact_path.h"
#include "operator_defaults.h"
#include "param_read.h"

#include <QFileInfo>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QElapsedTimer>
#include <memory>
#include <map>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace w7 {

// ============================================================
// 运行时探测
// ============================================================
bool DlInfer::runtimeAvailable() {
#ifdef HAS_ONNXRUNTIME
    return true;
#else
    return false;
#endif
}

QString DlInfer::runtimeUnavailableReason() {
    if (runtimeAvailable()) return QString();
    return QStringLiteral(
        "ONNX Runtime 未安装,无法推理。"
        "请下载 ONNX Runtime Windows 版,解压到 D:\\onnxruntime,"
        "并设置环境变量 ONNXRUNTIME_DIR=D:\\onnxruntime 后重新编译。");
}

QJsonObject DlInfer::defaultParams() const {
    QJsonObject p;
    p["model_path"]    = QString::fromUtf8(DlInferDefaults::kModelPath);
    p["input_w"]       = DlInferDefaults::kInputW;
    p["input_h"]       = DlInferDefaults::kInputH;
    p["conf_threshold"]= DlInferDefaults::kConfThreshold;
    p["nms_iou"]       = DlInferDefaults::kNmsIou;
    p["class_names"]   = QString::fromUtf8(DlInferDefaults::kClassNames);
    // yolov8=(1,4+nc,N) / yolov5=(1,N,5+nc)
    p["output_layout"] = QString::fromUtf8(DlInferDefaults::kOutputLayout);
    return p;
}

#ifndef HAS_ONNXRUNTIME
// 没装 ONNX Runtime 时的空实现。
// 仍然做「不依赖运行时的部分」—— 检查模型文件存不存在,
// 这样用户能分清「模型路径写错了」还是「SDK 没装」,两种病因两种药。
QJsonObject DlInfer::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject out;
    // 兜底回到声明的默认值:参数 JSON 缺 model_path 时,
    // 算子按"声明的默认模型路径"去找,而不是按空路径 —— 空路径的
    // 报错信息("模型文件不存在: ")会把人引向"路径填错了",
    // 而真实原因是"这个 key 压根没写"。
    //
    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    QString modelPath;
    {
        QString err;
        if (!readTextKnob(params, defaultParams(), "model_path", &modelPath, &err)) {
            out["error"] = err;
            return out;
        }
    }

    if (inputs.value("image_path").toString().isEmpty()) {
        out["error"] = "上游未提供 image_path";
        return out;
    }
    if (!QFileInfo::exists(modelPath)) {
        out["error"] = QString("模型文件不存在: %1").arg(modelPath);
        return out;
    }
    out["error"] = DlInfer::runtimeUnavailableReason()
                   + "模型文件本身是存在的(这是好事,说明路径对)。";
    return out;
}
#else
// ============================================================
// 装了 ONNX Runtime 的真实推理路径
// ============================================================
namespace {

// 进程内缓存:同一个模型反复 Run 不该每次都重新加载
// (加载几百 MB 的权重一次要 1~2 秒,流水线里每帧都付这个代价不可接受)
struct SessionCache {
    std::map<QString, std::unique_ptr<Ort::Session>> sessions;
    std::unique_ptr<Ort::Env> env;
};

SessionCache& cache() {
    static SessionCache c;
    if (!c.env)
        c.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "w7");
    return c;
}

} // namespace

QJsonObject DlInfer::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject out;

    QString imgPath = inputs.value("image_path").toString();
    if (imgPath.isEmpty()) { out["error"] = "上游未提供 image_path"; return out; }

    // 兜底回到声明的默认值:参数 JSON 缺 model_path 时,
    // 算子按"声明的默认模型路径"去找,而不是按空路径 —— 空路径的
    // 报错信息("模型文件不存在: ")会把人引向"路径填错了",
    // 而真实原因是"这个 key 压根没写"。
    //
    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    QString modelPath;
    {
        QString err;
        if (!readTextKnob(params, defaultParams(), "model_path", &modelPath, &err)) {
            out["error"] = err;
            return out;
        }
    }
    if (!QFileInfo::exists(modelPath)) {
        out["error"] = QString("模型文件不存在: %1").arg(modelPath);
        return out;
    }

    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) { out["error"] = "imread failed: " + imgPath; return out; }

    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    int inputW = 0, inputH = 0;
    double confThreshD = 0.0, iouThreshD = 0.0;
    QString layout;
    // class_names 以前**没有兜底**:缺这个 key 时类名列表是空的,
    // 检测框上只剩 "cls3" 这种编号,标不出是哪个类别。
    // 现在兜底回到声明的默认值 —— 少一个 key 不该悄悄换一套语义。
    QString classNamesCsv;
    {
        QString err;
        if (!readIntKnob(params, defaultParams(), "input_w", &inputW, &err) ||
            !readIntKnob(params, defaultParams(), "input_h", &inputH, &err) ||
            !readDoubleKnob(params, defaultParams(), "conf_threshold", &confThreshD, &err) ||
            !readDoubleKnob(params, defaultParams(), "nms_iou", &iouThreshD, &err) ||
            !readTextKnob(params, defaultParams(), "output_layout", &layout, &err) ||
            !readTextKnob(params, defaultParams(), "class_names", &classNamesCsv, &err)) {
            out["error"] = err;
            return out;
        }
    }
    const cv::Size netInput(inputW, inputH);
    const float confThresh = static_cast<float>(confThreshD);
    const float iouThresh   = static_cast<float>(iouThreshD);
    QStringList classNames = classNamesCsv.split(',', Qt::SkipEmptyParts);
    for (auto& s : classNames) s = s.trimmed();

    // ---------- 加载模型(带缓存) ----------
    SessionCache& sc = cache();
    auto it = sc.sessions.find(modelPath);
    if (it == sc.sessions.end()) {
        try {
            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            so.SetIntraOpNumThreads(4);
            auto sess = std::make_unique<Ort::Session>(
                *sc.env, modelPath.toStdString().c_str(), so);
            it = sc.sessions.emplace(modelPath, std::move(sess)).first;
        } catch (const Ort::Exception& e) {
            out["error"] = QString("模型加载失败: %1").arg(QString::fromUtf8(e.what()));
            return out;
        }
    }
    Ort::Session& session = *it->second;

    // ---------- 预处理:letterbox + BGR2RGB + /255 + NCHW ----------
    // 顺序不能错:先 letterbox 再换通道最后归一化。
    // 常见错误是归一化跑在 RGB 转换之前 —— 数值一样但通道序错了,结果全错
    float scale = 1.f; int padX = 0, padY = 0;
    cv::Mat lb = dl::letterbox(img, netInput, &scale, &padX, &padY);
    cv::Mat rgb, f32;
    cv::cvtColor(lb, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(f32, CV_32FC3, 1.0 / 255.0);

    std::vector<int64_t> shape{1, 3, netInput.height, netInput.width};
    auto inputTensor = Ort::Value::CreateTensor<float>(
        sc.env->GetAllocatorInfo(), f32.data, f32.total(), shape.data(), shape.size());
    const char* inNames[]  = {session.GetInputNameAllocated(0, sc.env->GetAllocatorInfo()).get()};
    const char* outNames[] = {session.GetOutputNameAllocated(0, sc.env->GetAllocatorInfo()).get()};

    // ---------- 推理 ----------
    QElapsedTimer timer;
    timer.start();
    auto outputTensor = session.Run(Ort::RunOptions{nullptr}, inNames, &inputTensor, 1,
                                    outNames, 1);
    const qint64 elapsed = timer.elapsed();

    // ---------- 后处理 ----------
    auto info = outputTensor[0].GetTensorTypeAndShapeInfo();
    std::vector<int64_t> oshape = info.GetShape();
    size_t total = info.GetElementCount();
    const float* odata = outputTensor[0].GetTensorData<float>();

    // ORT 给的是一维连续内存。OpenCV 的 Mat 没有三维,
    // 所以把 batch 维(恒为 1)去掉,包成二维视图:
    //   yolov8 (1, 4+nc, N) → (4+nc, N)  C-major
    //   yolov5 (1, N, 5+nc) → (N, 5+nc)  N-major
    // 两种布局长得完全不一样,包错形状不会报错,只是全检测不出来 ——
    // 所以 output_layout 必须由用户显式指定,不能靠猜通道数。
    cv::Mat om((int)oshape[1], (int)oshape[2], CV_32F,
               const_cast<float*>(odata));

    std::vector<dl::Detection> dets;
    if (layout == "yolov5")
        dets = dl::parseYolov5(om, cv::Size(img.cols, img.rows), netInput,
                               padX, padY, scale, confThresh);
    else
        dets = dl::parseYolov8(om, cv::Size(img.cols, img.rows), netInput,
                               padX, padY, scale, confThresh);
    dets = dl::nms(std::move(dets), iouThresh);

    cv::Mat marked = dl::drawDetections(img, dets, classNames);
    // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
    // 理由见 artifact_path.h 的说明。
    const QString markedPath = w7::artifactPath(
        "dl_infer",
        inputs.value("node_uuid").toString(),
        inputs.value("frame_index").toInt(-1));
    QDir().mkpath(QFileInfo(markedPath).absolutePath());
    cv::imwrite(markedPath.toStdString(), marked);

    QJsonArray arr;
    for (const auto& d : dets) {
        QJsonObject o;
        o["class_id"] = d.classId;
        o["class"]    = (d.classId >= 0 && d.classId < classNames.size())
                          ? classNames[d.classId] : QString("cls%1").arg(d.classId);
        o["conf"]     = d.conf;
        o["x"] = (int)d.box.x;     o["y"] = (int)d.box.y;
        o["w"] = (int)d.box.width; o["h"] = (int)d.box.height;
        arr.append(o);
    }

    out["image_path"]   = imgPath;
    out["marked_path"]  = markedPath;
    out["detections"]   = arr;
    out["count"]        = arr.size();
    out["inference_ms"] = (double)elapsed;
    out["model"]        = modelPath;
    out["layout"]       = layout;
    return out;
}
#endif // HAS_ONNXRUNTIME

QWidget* DlInfer::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new DlInferForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// DlInferForm
// ============================================================
DlInferForm::DlInferForm(QWidget* parent) : ParamForm(parent) {}

void DlInferForm::buildUI() {
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText(QString::fromUtf8(DlInferDefaults::kModelPath));
    m_inW = new QSpinBox; m_inW->setRange(32, 4096); m_inW->setValue(DlInferDefaults::kInputW);
    m_inH = new QSpinBox; m_inH->setRange(32, 4096); m_inH->setValue(DlInferDefaults::kInputH);
    m_conf = new QDoubleSpinBox;
    m_conf->setRange(0.0, 1.0); m_conf->setSingleStep(0.05); m_conf->setValue(DlInferDefaults::kConfThreshold);
    m_iou = new QDoubleSpinBox;
    m_iou->setRange(0.0, 1.0); m_iou->setSingleStep(0.05); m_iou->setValue(DlInferDefaults::kNmsIou);
    m_classes = new QLineEdit(QString::fromUtf8(DlInferDefaults::kClassNames));
    // ⚠ 这里以前是 setPlaceholderText("person,bicycle,car") ——
    //   那是 kClassNames 的**一份抄本**,而且只抄了前三个(逗号类名表
    //   一共六个)。抄本比原文更容易骗人:它看起来像"举个例子",
    //   于是没人去核对它和声明值是不是一回事。
    //
    //   现在直接用声明值当初值:用户一眼看到检测框上会标什么,
    //   而且这一栏 collect() 写回 params 时写的就是它 ——
    //   面板上摆的和算子实际用的是同一个串。
    //
    //   (model_path 那边只放 placeholder,不预填:模型文件得用户自己指,
    //    一个空框比一个写着不存在的路径的框诚实。)
    m_outLayout = new QComboBox;
    m_outLayout->addItem(tr("YOLOv8  (1, 4+类别数, N)"),   QString::fromUtf8(DlInferDefaults::kOutputLayout));
    m_outLayout->addItem(tr("YOLOv5  (1, N, 5+类别数)"),   "yolov5");

    m_layout->addRow(tr("模型路径 .onnx"), m_modelPath);
    m_layout->addRow(tr("输入宽"),  m_inW);
    m_layout->addRow(tr("输入高"),  m_inH);
    m_layout->addRow(tr("置信度阈值"), m_conf);
    m_layout->addRow(tr("NMS IoU 阈值"), m_iou);
    m_layout->addRow(tr("类别名(逗号分隔)"), m_classes);
    m_layout->addRow(tr("输出布局"), m_outLayout);

    // 运行时缺失时明确告诉用户,而不是等 Run 完才在日志里看到
    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    if (DlInfer::runtimeAvailable()) {
        m_hint->setText(tr("✅ 已检测到 ONNX Runtime"));
    } else {
        m_hint->setText(tr("⚠️ %1").arg(DlInfer::runtimeUnavailableReason()));
        m_hint->setStyleSheet("color:#c05000;");
        // ============================================================
        // 旋钮置灰:运行时不在,这几个旋钮调了也不生效
        // ============================================================
        // 界面上摆一个"调了不生效"的旋钮 = 骗人。
        // 用户在这里填的每一个数字,当前这个构建里都不会被读到 ——
        // 真正读它们的那段代码在 #ifdef HAS_ONNXRUNTIME 里,没编进来。
        //
        // 置灰 + 写明原因,是唯一诚实的做法:用户一眼就知道
        // "不是我填错了,是这个功能现在没装上"。
        //
        // model_path 不置灰:空实现里它**真的被读**
        // (用来区分"路径写错了"和"SDK 没装"两种病因)。
        // ============================================================
        m_inW->setEnabled(false);
        m_inH->setEnabled(false);
        m_conf->setEnabled(false);
        m_iou->setEnabled(false);
        m_classes->setEnabled(false);
        m_outLayout->setEnabled(false);
    }
    m_layout->addRow(m_hint);
}

void DlInferForm::setParams(const QJsonObject& params) {
    m_modelPath->setText(params.value("model_path").toString(
                              QString::fromUtf8(DlInferDefaults::kModelPath)));
    m_inW->setValue(params.value("input_w").toInt(DlInferDefaults::kInputW));
    m_inH->setValue(params.value("input_h").toInt(DlInferDefaults::kInputH));
    m_conf->setValue(params.value("conf_threshold").toDouble(DlInferDefaults::kConfThreshold));
    m_iou->setValue(params.value("nms_iou").toDouble(DlInferDefaults::kNmsIou));
    m_classes->setText(params.value("class_names").toString(
                            QString::fromUtf8(DlInferDefaults::kClassNames)));
    const int idx = m_outLayout->findData(params.value("output_layout").toString(
                               QString::fromUtf8(DlInferDefaults::kOutputLayout)));
    if (idx >= 0) m_outLayout->setCurrentIndex(idx);
}

QJsonObject DlInferForm::collect() const {
    QJsonObject p;
    p["model_path"]     = m_modelPath->text();
    p["input_w"]        = m_inW->value();
    p["input_h"]        = m_inH->value();
    p["conf_threshold"] = m_conf->value();
    p["nms_iou"]        = m_iou->value();
    p["class_names"]    = m_classes->text();
    p["output_layout"]  = m_outLayout->currentData().toString();
    return p;
}

} // namespace w7
