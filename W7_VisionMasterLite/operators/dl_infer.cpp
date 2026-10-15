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

QJsonObject DlInfer::defaultParams() const {
    QJsonObject p;
    p["model_path"]    = "models/yolov8n.onnx";
    p["input_w"]       = 640;
    p["input_h"]       = 640;
    p["conf_threshold"]= 0.25;
    p["nms_iou"]       = 0.45;
    p["class_names"]   = "person,bicycle,car,motorcycle,airplane,bus";
    p["output_layout"] = "yolov8";   // yolov8=(1,4+nc,N) / yolov5=(1,N,5+nc)
    return p;
}

#ifndef HAS_ONNXRUNTIME
// 没装 ONNX Runtime 时的空实现。
// 仍然做「不依赖运行时的部分」—— 检查模型文件存不存在,
// 这样用户能分清「模型路径写错了」还是「SDK 没装」,两种病因两种药。
QJsonObject DlInfer::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject out;
    const QString modelPath = params.value("model_path").toString();

    if (inputs.value("image_path").toString().isEmpty()) {
        out["error"] = "上游未提供 image_path";
        return out;
    }
    if (!QFileInfo::exists(modelPath)) {
        out["error"] = QString("模型文件不存在: %1").arg(modelPath);
        return out;
    }
    out["error"] = "ONNX Runtime 未安装,无法推理。模型文件本身是存在的"
                   "(这是好事,说明路径对)。请下载 ONNX Runtime Windows 版,"
                   "解压到 D:\\onnxruntime 并设置环境变量 "
                   "ONNXRUNTIME_DIR=D:\\onnxruntime 后重新编译。";
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

    const QString modelPath = params.value("model_path").toString();
    if (!QFileInfo::exists(modelPath)) {
        out["error"] = QString("模型文件不存在: %1").arg(modelPath);
        return out;
    }

    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) { out["error"] = "imread failed: " + imgPath; return out; }

    const cv::Size netInput(params.value("input_w").toInt(640),
                            params.value("input_h").toInt(640));
    const float confThresh = params.value("conf_threshold").toDouble(0.25);
    const float iouThresh   = params.value("nms_iou").toDouble(0.45);
    const QString layout   = params.value("output_layout").toString("yolov8");
    QStringList classNames = params.value("class_names").toString()
                                 .split(',', Qt::SkipEmptyParts);
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
    m_modelPath->setPlaceholderText("models/yolov8n.onnx");
    m_inW = new QSpinBox; m_inW->setRange(32, 4096); m_inW->setValue(640);
    m_inH = new QSpinBox; m_inH->setRange(32, 4096); m_inH->setValue(640);
    m_conf = new QDoubleSpinBox;
    m_conf->setRange(0.0, 1.0); m_conf->setSingleStep(0.05); m_conf->setValue(0.25);
    m_iou = new QDoubleSpinBox;
    m_iou->setRange(0.0, 1.0); m_iou->setSingleStep(0.05); m_iou->setValue(0.45);
    m_classes = new QLineEdit;
    m_classes->setPlaceholderText("person,bicycle,car");
    m_outLayout = new QComboBox;
    m_outLayout->addItem(tr("YOLOv8  (1, 4+类别数, N)"),   "yolov8");
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
        m_hint->setText(tr("⚠️ 未检测到 ONNX Runtime,此算子 Run 时会返回提示错误。"
                           "设置环境变量 ONNXRUNTIME_DIR 后重新编译即可启用。"));
        m_hint->setStyleSheet("color:#c05000;");
    }
    m_layout->addRow(m_hint);
}

void DlInferForm::setParams(const QJsonObject& params) {
    m_modelPath->setText(params.value("model_path").toString());
    m_inW->setValue(params.value("input_w").toInt(640));
    m_inH->setValue(params.value("input_h").toInt(640));
    m_conf->setValue(params.value("conf_threshold").toDouble(0.25));
    m_iou->setValue(params.value("nms_iou").toDouble(0.45));
    m_classes->setText(params.value("class_names").toString());
    const int idx = m_outLayout->findData(params.value("output_layout").toString("yolov8"));
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
