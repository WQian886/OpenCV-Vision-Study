#pragma once
// ============================================================
// image_source_dir — 文件夹批量图像源(对标 VM "文件夹图像")
// 输入:无 (起点算子,从 dir_path + pattern 参数读图)
// 输出:image (cv::Mat) + image_path + count + current_index + files[]
// params: dir_path(string), pattern(string,默认 "*.png")
//
// 用法:
//   - 把这个节点放在流水线最前面
//   - dir_path 填文件夹路径(相对或绝对)
//   - Run 时依次取 files[0..count-1],每张图都让下游算子跑一次
// ============================================================

#include "operator_base.h"
#include "param_form.h"

class QLineEdit;
class QSpinBox;
class QLabel;

namespace w7 {

class ImageSourceDir : public OperatorBase {
public:
    QString name() const override { return "image_source_dir"; }
    QString displayName() const override { return "文件夹批量图像"; }
    QString category() const override { return "acquisition"; }
    QString description() const override {
        return "按通配符批量加载文件夹内的图像(对标 VM '文件夹图像' 图像源)";
    }
    QJsonObject defaultParams() const override;
    QJsonObject run(const QJsonObject& params, const QJsonObject& inputs) override;
    QWidget* createParamWidget(QWidget* parent,
                               const QJsonObject& params) override;
};

// === 文件夹批量图像参数面板 ===
class ImageSourceDirForm : public ParamForm {
    Q_OBJECT
public:
    explicit ImageSourceDirForm(QWidget* parent = nullptr);
protected:
    void buildUI() override;
    QJsonObject collect() const override;
    void setParams(const QJsonObject& params) override;
private:
    QLineEdit* m_dirPath = nullptr;
    QLineEdit* m_pattern = nullptr;
};

} // namespace w7