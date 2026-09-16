#include "image_source_dir.h"

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
    p["dir_path"] = "images";
    p["pattern"]  = "*.png";
    return p;
}

QJsonObject ImageSourceDir::run(const QJsonObject& params, const QJsonObject& /*inputs*/) {
    QJsonObject output;
    QString dirPath  = params.value("dir_path").toString();
    QString pattern  = params.value("pattern").toString("*.png");

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

    output["count"]         = entryList.size();
    output["files"]         = filesJson;
    output["current_index"] = 0;

    // 当前激活的图（W7 Run 引擎不传 cv::Mat,只传 path 字符串,下游算子各自 imread）
    if (entryList.isEmpty()) {
        output["error"] = QString("目录为空或无 %1 匹配: %2")
                              .arg(pattern, dirPath);
        return output;
    }
    QString firstFile = entryList.first().absoluteFilePath();
    cv::Mat img = cv::imread(firstFile.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + firstFile;
        return output;
    }
    // 只输出 path + metadata(下游算子各自 imread;image 字段暂未用)
    output["image_path"] = firstFile;
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
    m_dirPath = new QLineEdit;
    m_dirPath->setPlaceholderText("如 images 或 D:/test_imgs");

    m_pattern = new QLineEdit("*.png");
    m_pattern->setPlaceholderText("如 *.png 或 *.jpg");

    m_layout->addRow(tr("目录路径"), m_dirPath);
    m_layout->addRow(tr("文件名通配符"), m_pattern);
}

void ImageSourceDirForm::setParams(const QJsonObject& params) {
    if (params.contains("dir_path")) m_dirPath->setText(params["dir_path"].toString());
    if (params.contains("pattern"))  m_pattern->setText(params["pattern"].toString());
}

QJsonObject ImageSourceDirForm::collect() const {
    QJsonObject p;
    p["dir_path"] = m_dirPath->text();
    p["pattern"]  = m_pattern->text();
    return p;
}

} // namespace w7