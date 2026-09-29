#include "image_source_video.h"

#include <QDir>
#include <QFileInfo>
#include <QSpinBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QJsonArray>

#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

namespace w7 {

QJsonObject ImageSourceVideo::defaultParams() const {
    QJsonObject p;
    p["video_path"] = "images/sample.avi";
    p["max_frames"] = 30;    // 最多处理多少帧 —— 视频可能有几千帧,全跑一遍很久
    p["frame_step"] = 1;     // 隔几帧取一帧 —— 快速粗看时抽帧
    return p;
}

QJsonObject ImageSourceVideo::run(const QJsonObject& params,
                                 const QJsonObject& inputs) {
    QJsonObject output;
    const QString videoPath = params.value("video_path").toString();
    const int maxFrames = params.value("max_frames").toInt(30);
    const int frameStep = std::max(1, params.value("frame_step").toInt(1));

    if (!QFileInfo::exists(videoPath)) {
        output["error"] = QString("视频文件不存在: %1").arg(videoPath);
        return output;
    }

    // === 打开视频 ===
    // 必须 try/catch:往 VideoCapture 里塞一个非视频文件(比如误选了 png),
    // OpenCV 的 image-sequence 后端会走到
    //   CV_Assert(!filename_pattern.empty())
    // 直接抛 cv::Exception —— 不是返回 false,是抛。
    // === 扩展名预检 ===
    // 误选成 png/jpg 是最常见的操作失误,而 VideoCapture 拿到图片会走
    // image-sequence 后端,那里有一句
    //     CV_Assert(!filename_pattern.empty())
    // 直接抛异常。先挡一道,能给用户一句人话,
    // 而不是把 OpenCV 的断言原文甩到他脸上。
    const QString suffix = QFileInfo(videoPath).suffix().toLower();
    static const QStringList kVideoExt =
        {"avi", "mp4", "mov", "mkv", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "ts"};
    if (!kVideoExt.contains(suffix)) {
        output["error"] = QString("%1 不是常见的视频格式(扩展名 .%2)。"
                                  "支持:%3")
                              .arg(videoPath, suffix, kVideoExt.join(" / "));
        return output;
    }

    // === 打开 + 取第 N 帧 ===
    // 整段包在 try 里:不只是 open 会抛,isOpened()/get()/read()
    // 在后端状态不对时同样可能抛 cv::Exception。
    // 产线上的算子不能因为用户选错文件就把整条流水线带崩。
    //
    // 「seek 到第 N 帧」在压缩视频上是不可靠的:关键帧间隔不固定,
    // CAP_PROP_POS_FRAMES 跳过去经常落在半个 GOP 里,拿到的是花屏。
    // 所以这里老老实实从头顺序解码到第 N 帧 —— 慢,但一定对。
    // 真要随机跳帧,得先解全片建关键帧索引表,那属于另一件事。
    const int wantIndex = inputs.value("frame_index").toInt(0);

    double fps = 0.0;
    int totalFrames = 0;
    int usable = maxFrames;
    int decoded = -1;
    cv::Mat frame;

    try {
        cv::VideoCapture cap(videoPath.toStdString());
        if (!cap.isOpened()) {
            output["error"] = QString("无法打开视频: %1\n"
                                      "提示:OpenCV 的视频解码依赖 FFmpeg 后端,"
                                      "MP4(H.264) 有时能读有时不能,"
                                      "不确定就先用 AVI 试试").arg(videoPath);
            return output;
        }

        fps = cap.get(cv::CAP_PROP_FPS);
        totalFrames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
        // 帧数在有些封装里读出来是 0(比如流媒体容器),这时不设上限,
        // 由 frame_index 索引时读到末尾自然停
        usable = (totalFrames > 0) ? std::min(maxFrames, totalFrames) : maxFrames;

        for (int i = 0; i <= wantIndex; ++i) {
            if (!cap.read(frame) || frame.empty()) { decoded = -1; break; }
            decoded = i;
        }
    } catch (const cv::Exception& e) {
        output["error"] = QString("解码视频时出错: %1")
                              .arg(QString::fromUtf8(e.what()));
        return output;
    }

    if (decoded < 0) {
        output["error"] = QString("第 %1 帧读取失败(视频可能只有 %2 帧)")
                              .arg(wantIndex).arg(totalFrames);
        return output;
    }

    // === 落盘成临时 png,下游拿到的就是普通 image_path ===
    // 帧没有"文件路径",而 Run 引擎只传 path 字符串让下游各自 imread。
    // 所以视频源也必须产出一个真实文件,才能和图像源统一。
    const QString snapPath =
        QString("images/w7_video_f%1.png").arg(decoded, 4, 10, QChar('0'));
    QDir().mkpath("images");
    if (!cv::imwrite(snapPath.toStdString(), frame)) {
        output["error"] = "帧快照写盘失败: " + snapPath;
        return output;
    }

    output["image_path"]    = snapPath;
    output["video_path"]    = videoPath;
    output["current_index"] = decoded;
    // 引擎靠这个数决定要不要继续下一帧
    output["frame_count"]   = usable;
    output["frame_step"]    = frameStep;
    output["fps"]           = (fps > 0) ? fps : 0.0;
    output["width"]         = frame.cols;
    output["height"]        = frame.rows;
    output["channels"]      = frame.channels();
    return output;
}

QWidget* ImageSourceVideo::createParamWidget(QWidget* parent,
                                             const QJsonObject& /*params*/) {
    auto* form = new ImageSourceVideoForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// ImageSourceVideoForm
// ============================================================
ImageSourceVideoForm::ImageSourceVideoForm(QWidget* parent) : ParamForm(parent) {}

void ImageSourceVideoForm::buildUI() {
    m_videoPath = new QLineEdit;
    m_videoPath->setPlaceholderText("如 images/sample.avi");

    m_maxFrames = new QSpinBox;
    m_maxFrames->setRange(1, 100000);
    m_maxFrames->setValue(30);
    m_maxFrames->setToolTip(tr("最多处理多少帧。视频动辄几千帧,全跑一遍很慢"));

    m_frameStep = new QSpinBox;
    m_frameStep->setRange(1, 1000);
    m_frameStep->setValue(1);
    m_frameStep->setToolTip(tr("隔几帧取一帧。快速粗看时抽帧,比如 10 = 每 10 帧取 1"));

    m_layout->addRow(tr("视频路径"), m_videoPath);
    m_layout->addRow(tr("最多处理帧数"), m_maxFrames);
    m_layout->addRow(tr("抽帧间隔"), m_frameStep);
}

void ImageSourceVideoForm::setParams(const QJsonObject& params) {
    if (params.contains("video_path")) m_videoPath->setText(params["video_path"].toString());
    if (params.contains("max_frames")) m_maxFrames->setValue(params["max_frames"].toInt(30));
    if (params.contains("frame_step")) m_frameStep->setValue(params["frame_step"].toInt(1));
}

QJsonObject ImageSourceVideoForm::collect() const {
    QJsonObject p;
    p["video_path"] = m_videoPath->text();
    p["max_frames"] = m_maxFrames->value();
    p["frame_step"] = m_frameStep->value();
    return p;
}

} // namespace w7
