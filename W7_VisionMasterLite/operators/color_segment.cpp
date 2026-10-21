#include "color_segment.h"
#include "hsv_params.h"
#include "operator_defaults.h"
#include "artifact_path.h"
#include <QSpinBox>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>

namespace w7 {

// readHsvTriple / jsonTypeName 现在住在 operators/hsv_params.h。
// 原来是本文件里的 static 副本 —— 后来 pcb_inspect.cpp:83 又踩了同一个坑
// (`lo[0].toInt(85)`,空数组解引用,同样的崩法)。抄副本这条路在这个项目里
// 已经埋了两次雷(上一次是 EXPECTS[6] 抄了一份 ROI 几何中心),
// 所以守卫只保留一份定义,两个算子共用。

QJsonObject ColorSegment::defaultParams() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{ColorSegmentDefaults::kHsvLowH,
                                ColorSegmentDefaults::kHsvLowS,
                                ColorSegmentDefaults::kHsvLowV};
    p["hsv_high"] = QJsonArray{ColorSegmentDefaults::kHsvHighH,
                                ColorSegmentDefaults::kHsvHighS,
                                ColorSegmentDefaults::kHsvHighV};
    return p;
}

QJsonObject ColorSegment::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    int lo[3] = {0, 0, 0};
    int hi[3] = {0, 0, 0};
    QString err;
    if (!readHsvTriple(params.value("hsv_low"),  "hsv_low",  lo, &err) ||
        !readHsvTriple(params.value("hsv_high"), "hsv_high", hi, &err)) {
        output["error"] = QString("HSV 阈值不合法: %1").arg(err);
        return output;
    }
    const cv::Scalar low (lo[0], lo[1], lo[2]);
    const cv::Scalar high(hi[0], hi[1], hi[2]);

    cv::Mat hsv, mask;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, low, high, mask);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);

    // ============================================================
    // mask 落盘的文件名必须**每个节点实例 + 每帧唯一**
    //
    // Run 引擎只传字符串路径,所以算子之间靠"写文件 → 下游 imread"传数据。
    // 文件名写死成 w7_color_segment_mask.png 的话,一条流水线里放两个
    // 颜色分割(抓红 + 抓蓝,很常见)时,第二个会覆盖第一个的文件,
    // 而下游 blob_count 拿着同一个路径读回来,读到的是**第二个的 mask**。
    //
    // 结果:不崩、不报错、数出来的数就是错的。产线上这种错最难查 ——
    // 因为每一步都"成功"了。
    //
    // 命名规则已抽到 artifact_path.h,全项目只有那一个地方生成路径 ——
    // 这里原来自己拼的 "w7_seg_..." 是当时就地发明的,
    // 别的 9 个算子还各写各的固定名(见 artifact_path.h 的说明)。
    //
    // 保留历史文件名 images/w7_color_segment_mask.png:
    // 有两条断言直接 imread 那个固定路径(见 test_detection_operators.cpp)。
    // ============================================================
    const QString nodeUuid = inputs.value("node_uuid").toString();
    const QString outPath = nodeUuid.isEmpty()
        ? QString("images/w7_color_segment_mask.png")   // 测试路径:保持不变
        : w7::artifactPath("color_segment", nodeUuid,
                           inputs.value("frame_index").toInt(-1));
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    if (!cv::imwrite(outPath.toStdString(), mask)) {
        output["error"] = QString("mask 写盘失败: %1").arg(outPath);
        return output;
    }

    output["mask_path"]  = outPath;
    output["pixel_count"] = static_cast<int>(cv::countNonZero(mask));
    return output;
}

QWidget* ColorSegment::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new ColorSegmentForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

ColorSegmentForm::ColorSegmentForm(QWidget* parent) : ParamForm(parent) {}

void ColorSegmentForm::buildUI() {
    m_hLow  = new QSpinBox;  m_hLow->setRange(0, 179);
    m_hLow->setValue(ColorSegmentDefaults::kHsvLowH);
    m_sLow  = new QSpinBox;  m_sLow->setRange(0, 255);
    m_sLow->setValue(ColorSegmentDefaults::kHsvLowS);
    m_vLow  = new QSpinBox;  m_vLow->setRange(0, 255);
    m_vLow->setValue(ColorSegmentDefaults::kHsvLowV);
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179);
    m_hHigh->setValue(ColorSegmentDefaults::kHsvHighH);
    m_sHigh = new QSpinBox;  m_sHigh->setRange(0, 255);
    m_sHigh->setValue(ColorSegmentDefaults::kHsvHighS);
    m_vHigh = new QSpinBox;  m_vHigh->setRange(0, 255);
    m_vHigh->setValue(ColorSegmentDefaults::kHsvHighV);

    m_layout->addRow(tr("H 下限"),  m_hLow);
    m_layout->addRow(tr("S 下限"),  m_sLow);
    m_layout->addRow(tr("V 下限"),  m_vLow);
    m_layout->addRow(tr("H 上限"),  m_hHigh);
    m_layout->addRow(tr("S 上限"),  m_sHigh);
    m_layout->addRow(tr("V 上限"),  m_vHigh);
}

void ColorSegmentForm::setParams(const QJsonObject& params) {
    // 和 PcbInspectForm::setParams 走同一个守卫,读法保持一致。
    // 原来这里只查了 `size() >= 3`,漏了元素类型:
    // QJsonValue::toInt() 对字符串返回 **0**,而 0 是合法阈值,
    // 于是手改过的 .sol.json 里 ["85","80","80"] 会安静地变成 (0,0,0)。
    //
    // 读不出来就保持界面默认值,不报错 —— run() 会明确报。
    int lo[3] = {0, 0, 0};
    int hi[3] = {0, 0, 0};
    QString err;
    if (readHsvTriple(params.value("hsv_low"), "hsv_low", lo, &err)) {
        m_hLow->setValue(lo[0]);
        m_sLow->setValue(lo[1]);
        m_vLow->setValue(lo[2]);
    }
    if (readHsvTriple(params.value("hsv_high"), "hsv_high", hi, &err)) {
        m_hHigh->setValue(hi[0]);
        m_sHigh->setValue(hi[1]);
        m_vHigh->setValue(hi[2]);
    }
}

QJsonObject ColorSegmentForm::collect() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{m_hLow->value(),  m_sLow->value(),  m_vLow->value()};
    p["hsv_high"] = QJsonArray{m_hHigh->value(), m_sHigh->value(), m_vHigh->value()};
    return p;
}

} // namespace w7
