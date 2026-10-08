#include "count_check.h"
#include <QSpinBox>

namespace w7 {

QJsonObject CountCheck::defaultParams() const {
    QJsonObject p;
    p["expected_min"] = 1;
    p["expected_max"] = 100;
    return p;
}

QJsonObject CountCheck::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    int actual      = inputs.value("count").toInt();
    int expectedMin = params.value("expected_min").toInt(0);
    int expectedMax = params.value("expected_max").toInt(999);

    // ============================================================
    // 阈值填反了(min > max)必须报错,不能默默判 FAIL
    //
    // 判定式是 (actual >= min) && (actual <= max)。min=100、max=50 时
    // 没有任何 actual 能同时满足,于是**每一帧都 FAIL** —— 整批判废,
    // 但日志里写的是"数量 60 不在 [100, 50] 内",这句话自己就自相矛盾,
    // 用户根本看不出是自己的阈值填反了。
    //
    // 参数面板上两个 QSpinBox 都是 0~99999 且互相之间没有任何约束,
    // 所以这个状态是能填出来的,不是理论问题。
    //
    // 为什么报错而不是"自动交换两个值":交换看起来更友好,但那意味着
    // 填错配置会把本该判废的产品放行(判废方向反了)。
    // 填错配置时保守(直接报错、整批不判合格)才是对的。
    // ============================================================
    if (expectedMin > expectedMax) {
        output["error"] =
            QString("数量判定阈值填反了:最小值 %1 > 最大值 %2,没有任何数量能落在"
                   "这个区间里。请把最小值改小或把最大值改大。")
                .arg(expectedMin).arg(expectedMax);
        output["expected_min"] = expectedMin;
        output["expected_max"] = expectedMax;
        // result 故意**不给**:调用方看到没有 result 就知道这一步没跑成,
        // 而不是看到一个 FAIL 去当成真实判定结果。给了 PASS 之外的任何
        // 判定值都是撒谎。
        return output;
    }

    // 上游没给 count 时,inputs.value("count").toInt() 会静默给 0。
    // 而默认区间是 [0,999] —— 于是"什么都没检测到"会被判成 PASS。
    // 对一个判定算子来说这是最危险的默认值:漏检被判合格。
    // 所以这里必须区分"真的数出来 0 个"和"上游压根没给数"。
    if (!inputs.contains("count")) {
        output["error"] = QString("上游没有提供 count —— 这一步要接在"
                                  "Blob 计数之类会输出 count 的算子后面。"
                                  "(缺它会默认当成 0,而 0 可能落在合格区间里)");
        output["expected_min"] = expectedMin;
        output["expected_max"] = expectedMax;
        return output;
    }

    bool pass = (actual >= expectedMin) && (actual <= expectedMax);
    output["result"]       = pass ? "PASS" : "FAIL";
    output["actual"]       = actual;
    output["expected_min"] = expectedMin;
    output["expected_max"] = expectedMax;
    output["description"]  = pass
        ? QString("数量 %1 在 [%2, %3] 内").arg(actual).arg(expectedMin).arg(expectedMax)
        : QString("数量 %1 不在 [%2, %3] 内").arg(actual).arg(expectedMin).arg(expectedMax);
    return output;
}

QWidget* CountCheck::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new CountCheckForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

CountCheckForm::CountCheckForm(QWidget* parent) : ParamForm(parent) {}

void CountCheckForm::buildUI() {
    m_min = new QSpinBox;
    m_min->setRange(0, 99999);
    m_min->setValue(1);

    m_max = new QSpinBox;
    m_max->setRange(0, 99999);
    m_max->setValue(100);

    m_layout->addRow(tr("期望最小值"), m_min);
    m_layout->addRow(tr("期望最大值"), m_max);
}

void CountCheckForm::setParams(const QJsonObject& params) {
    if (params.contains("expected_min")) m_min->setValue(params["expected_min"].toInt());
    if (params.contains("expected_max")) m_max->setValue(params["expected_max"].toInt());
}

QJsonObject CountCheckForm::collect() const {
    QJsonObject p;
    p["expected_min"] = m_min->value();
    p["expected_max"] = m_max->value();
    return p;
}

} // namespace w7
