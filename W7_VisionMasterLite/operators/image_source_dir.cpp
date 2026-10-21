#include "image_source_dir.h"
#include "operator_defaults.h"
#include "param_read.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QIntValidator>

#include <opencv2/imgcodecs.hpp>

namespace w7 {

QJsonObject ImageSourceDir::defaultParams() const {
    QJsonObject p;
    p["dir_path"] = QString::fromUtf8(ImageSourceDirDefaults::kDirPath);
    p["pattern"]  = QString::fromUtf8(ImageSourceDirDefaults::kPattern);
    return p;
}

QJsonObject ImageSourceDir::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;
    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    // 兜底值来自 defaultParams() —— 声明值只有一份。
    QString dirPath;
    QString pattern;
    QString err;
    if (!readTextKnob(params, defaultParams(), "dir_path", &dirPath, &err) ||
        !readTextKnob(params, defaultParams(), "pattern", &pattern, &err)) {
        output["error"] = err;
        return output;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        output["error"] = "目录不存在: " + dirPath;
        return output;
    }

    // QDir::entryList 返回按名字排序的文件名列表
    QStringList nameFilters;
    nameFilters << pattern;
    QFileInfoList entryList = dir.entryInfoList(nameFilters, QDir::Files, QDir::Name);

    // 序列化为 QJsonArray 供下游算子用 + QJsonObject 存元信息
    QJsonArray filesJson;
    for (const auto& fi : entryList) {
        filesJson.append(fi.absoluteFilePath());
    }

    // === 取第几张 ===
    // Run 引擎按帧循环,每轮通过 inputs["frame_index"] 告诉源"这轮第几张"。
    // 以前这里写死 0,于是整个目录只会跑第一张 —— 注释里说的
    // "每张图都让下游算子跑一次"其实从没发生过。
    // 越界就停在最后一张:引擎算出来的帧数可能和实际文件数对不上
    // (运行中往目录里塞了图),停在最后一张比报错更符合"抽检"语义。
    const int want = qMax(0, inputs.value("frame_index").toInt(0));
    const int idx  = qMin(want, entryList.size() - 1);

    output["count"]         = entryList.size();
    output["files"]         = filesJson;
    output["current_index"] = idx;
    // 引擎靠 frame_count 决定循环几轮 —— 和视频源同一个键
    output["frame_count"]   = entryList.size();

    // 当前激活的图（W7 Run 引擎不传 cv::Mat,只传 path 字符串,下游算子各自 imread）
    if (entryList.isEmpty()) {
        output["error"] = QString("目录为空或无 %1 匹配: %2")
                              .arg(pattern, dirPath);
        return output;
    }
    const QString file = entryList.at(idx).absoluteFilePath();
    cv::Mat img = cv::imread(file.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + file;
        return output;
    }
    // 只输出 path + metadata(下游算子各自 imread;image 字段暂未用)
    output["image_path"] = file;
    output["width"]      = img.cols;
    output["height"]     = img.rows;
    output["channels"]   = img.channels();
    return output;
}

QWidget* ImageSourceDir::createParamWidget(QWidget* parent, const QJsonObject& /*params*/) {
    auto* form = new ImageSourceDirForm(parent);
    form->initialize();
    return form;
}

// ----------------------------------------------------------------
// 参数面板实现
// ----------------------------------------------------------------
ImageSourceDirForm::ImageSourceDirForm(QWidget* parent) : ParamForm(parent) {}

void ImageSourceDirForm::buildUI() {
    // 目录路径:用声明的默认值当初值。
    //
    // ⚠ 这里以前是 new QLineEdit(空) + setPlaceholderText("如 images 或 D:/test_imgs")。
    //   两个问题:
    //   1. 框里初始是空的 → collect() 把空串写回 params →
    //      defaultParams() 声明的默认值("images")被吃掉了,
    //      run() 拿到空串,报的是"目录不存在: "。
    //   2. 提示语里那个 "images" 是 kDirPath 的抄本。
    //   现在初值直接取常量,提示语只留"例子"那一半。
    m_dirPath = new QLineEdit(QString::fromUtf8(ImageSourceDirDefaults::kDirPath));
    m_dirPath->setPlaceholderText(tr("例:D:/test_imgs"));

    m_pattern = new QLineEdit(QString::fromUtf8(ImageSourceDirDefaults::kPattern));
    // 提示语里原来写着 "如 *.png 或 *.jpg" —— 那个 "*.png" 又是
    // kPattern 的抄本。提示语只留"例子"那一半。
    m_pattern->setPlaceholderText(tr("例:*.jpg"));

    m_layout->addRow(tr("目录路径"), m_dirPath);
    m_layout->addRow(tr("文件名通配符"), m_pattern);
}

void ImageSourceDirForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**。
    // ⚠ 以前是 toString() 不带参数 —— 那是**空串**,不是声明值:
    //   存盘里路径存成了数字时,面板显示空串,而算子按声明值
    //   ("images" / "*.png")跑 —— 界面上摆的和实际跑的不是一回事。
    const QJsonObject d = ImageSourceDir().defaultParams();
    if (params.contains("dir_path"))
        m_dirPath->setText(knobTextForWidget(params.value("dir_path"), d, "dir_path"));
    if (params.contains("pattern"))
        m_pattern->setText(knobTextForWidget(params.value("pattern"), d, "pattern"));
}

QJsonObject ImageSourceDirForm::collect() const {
    QJsonObject p;
    p["dir_path"] = m_dirPath->text();
    p["pattern"]  = m_pattern->text();
    return p;
}

} // namespace w7