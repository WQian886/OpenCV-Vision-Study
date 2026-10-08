#include "color_segment.h"
#include <QSpinBox>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>

namespace w7 {

// ============================================================
// 从 params 里取出一个 HSV 三元组
//
// 为什么要单独抽出来、而不是直接 lo[0].toInt():
// QJsonArray::operator[] **不检查越界**。数组空的时候 lo[0] 拿到的是
// 一个指向空容器的 QJsonValueRef,再 toInt() 就是解引用空指针 ——
// 实测直接 0xC0000005 崩掉(2026-10-08)。
//
// 这个坑本来是已知的:同一个文件的 ColorSegmentForm::setParams 里
// 就写了 `if (lo.size() >= 3)`。只是 run() 这一处漏了。
//
// 另一个决定:形状不对时报 error,**不补默认值**。
// 补默认值看起来更"健壮",但那会让用户以为自己的阈值生效了,
// 实际跑的是默认阈值 —— 属于"配置看着对、结果全错",
// 比直接报错难查一个数量级。
// ============================================================
// QJsonValue::typeName() 是 Qt 5 的 API,Qt 6 里没了,所以自己映射一张表。
// 报错里写"是字符串"比写"isString"有用 —— 用户看的是自己写下的东西。
static QString jsonTypeName(QJsonValue::Type t) {
    switch (t) {
        case QJsonValue::Null:      return QStringLiteral("null");
        case QJsonValue::Bool:      return QStringLiteral("布尔值");
        case QJsonValue::Double:    return QStringLiteral("数字");
        case QJsonValue::String:    return QStringLiteral("字符串");
        case QJsonValue::Array:     return QStringLiteral("数组");
        case QJsonValue::Object:    return QStringLiteral("对象");
        case QJsonValue::Undefined: return QStringLiteral("未定义");
    }
    return QStringLiteral("未知类型");
}

static bool readHsvTriple(const QJsonValue& v, const QString& key,
                          int out[3], QString* err) {
    if (!v.isArray()) {
        *err = QString("%1 不是数组(实际是%2)").arg(key, jsonTypeName(v.type()));
        return false;
    }
    const QJsonArray a = v.toArray();
    if (a.size() < 3) {
        *err = QString("%1 只有 %2 个元素,至少要 3 个(H/S/V)").arg(key).arg(a.size());
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!a[i].isDouble()) {          // const 数组的 operator[] 返回 QJsonValue,不越界
            *err = QString("%1 第 %2 个不是数字(实际是%3)")
                     .arg(key).arg(i + 1).arg(jsonTypeName(a[i].type()));
            return false;
        }
        out[i] = a[i].toInt();
    }
    return true;
}

QJsonObject ColorSegment::defaultParams() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{10, 80, 80};
    p["hsv_high"] = QJsonArray{40, 255, 255};
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
    // mask 落盘的文件名必须**每个节点实例唯一**
    //
    // Run 引擎只传字符串路径,所以算子之间靠"写文件 → 下游 imread"传数据。
    // 文件名写死成 w7_color_segment_mask.png 的话,一条流水线里放两个
    // 颜色分割(抓红 + 抓蓝,很常见)时,第二个会覆盖第一个的文件,
    // 而下游 blob_count 拿着同一个路径读回来,读到的是**第二个的 mask**。
    //
    // 结果:不崩、不报错、数出来的数就是错的。产线上这种错最难查 ——
    // 因为每一步都"成功"了。
    //
    // node_uuid 由 Run 引擎注入(见 MainWindow 里 inputs["node_uuid"] 的赋值)。
    // 缺它时退回旧的文件名,保证直接调 run() 的老路径不受影响。
    // ============================================================
    const QString nodeUuid = inputs.value("node_uuid").toString();
    const QString outPath = nodeUuid.isEmpty()
        ? QString("images/w7_color_segment_mask.png")
        : QString("images/w7_seg_%1_f%2.png")
              .arg(nodeUuid.left(8))
              .arg(inputs.value("frame_index").toInt(0), 4, 10, QChar('0'));
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
    m_hLow  = new QSpinBox;  m_hLow->setRange(0, 179);   m_hLow->setValue(10);
    m_sLow  = new QSpinBox;  m_sLow->setRange(0, 255);   m_sLow->setValue(80);
    m_vLow  = new QSpinBox;  m_vLow->setRange(0, 255);   m_vLow->setValue(80);
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179);  m_hHigh->setValue(40);
    m_sHigh = new QSpinBox;  m_sHigh->setRange(0, 255);  m_sHigh->setValue(255);
    m_vHigh = new QSpinBox;  m_vHigh->setRange(0, 255);  m_vHigh->setValue(255);

    m_layout->addRow(tr("H 下限"),  m_hLow);
    m_layout->addRow(tr("S 下限"),  m_sLow);
    m_layout->addRow(tr("V 下限"),  m_vLow);
    m_layout->addRow(tr("H 上限"),  m_hHigh);
    m_layout->addRow(tr("S 上限"),  m_sHigh);
    m_layout->addRow(tr("V 上限"),  m_vHigh);
}

void ColorSegmentForm::setParams(const QJsonObject& params) {
    QJsonArray lo = params.value("hsv_low").toArray();
    QJsonArray hi = params.value("hsv_high").toArray();
    if (lo.size() >= 3) {
        m_hLow->setValue(lo[0].toInt());
        m_sLow->setValue(lo[1].toInt());
        m_vLow->setValue(lo[2].toInt());
    }
    if (hi.size() >= 3) {
        m_hHigh->setValue(hi[0].toInt());
        m_sHigh->setValue(hi[1].toInt());
        m_vHigh->setValue(hi[2].toInt());
    }
}

QJsonObject ColorSegmentForm::collect() const {
    QJsonObject p;
    p["hsv_low"]  = QJsonArray{m_hLow->value(),  m_sLow->value(),  m_vLow->value()};
    p["hsv_high"] = QJsonArray{m_hHigh->value(), m_sHigh->value(), m_vHigh->value()};
    return p;
}

} // namespace w7
