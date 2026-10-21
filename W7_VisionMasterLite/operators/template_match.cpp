#include "template_match.h"
#include "operator_defaults.h"
#include "param_read.h"
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <algorithm>   // std::max({...}) 需要 initializer_list 重载

namespace w7 {

QJsonObject TemplateMatch::defaultParams() const {
    QJsonObject p;
    p["template_path"] = QString::fromUtf8(TemplateMatchDefaults::kTemplatePath);
    p["method"]        = TemplateMatchDefaults::kMethod;
    p["threshold"]     = TemplateMatchDefaults::kThreshold;
    return p;
}

QJsonObject TemplateMatch::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    QString tplPath = inputs.value("template_path").toString();
    if (tplPath.isEmpty()) {
        // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
        // 兜底值来自 defaultParams() —— 声明值只有一份。
        QString err;
        if (!readTextKnob(params, defaultParams(), "template_path", &tplPath, &err)) {
            output["error"] = err;
            return output;
        }
    }

    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    cv::Mat tpl = cv::imread(tplPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty() || tpl.empty()) {
        output["error"] = "imread failed (img or template)";
        return output;
    }

    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    int method = 0;
    double threshold = 0.0;
    {
        QString err;
        if (!readIntKnob(params, defaultParams(), "method", &method, &err) ||
            !readDoubleKnob(params, defaultParams(), "threshold", &threshold, &err)) {
            output["error"] = err;
            return output;
        }
    }

    // ============================================================
    // 两道必须在 cv::matchTemplate 之前的闸
    //
    // matchTemplate 内部用 CV_Assert 守住"模板不能比原图大"。
    //
    // 注意:**Release 构建下 CV_Assert 是空的**(NDEBUG 把 assert 关掉了),
    // 所以去掉这道闸并不会像文档说的那样抛 cv::Exception 崩掉程序。
    // 实测本机(OpenCV 4.x + Release):它安静地返回了一个
    //     score = 0.040665   position = (40, 31)
    // 而原图只有 40x40,合法 x 上限是 10 —— 它拿着越界的位置照常
    // 返回一个结果,不报错、不崩、画布状态点是绿的。
    //
    // 所以这道闸的收益不是"防崩溃",是把一次静默的越界结果换成一句报错。
    // 崩溃反而是好的失败方式:你立刻知道了。
    // 第二道闸(合法 method)同理:越界的方法号会让 matchTemplate 的
    // switch 落到没有实现的分支,结果一样是静默出错。
    // ============================================================
    // 注意 OpenCV 这六个枚举是**按家族顺序**排的,不是按相似度排的:
    //   0 TM_SQDIFF  1 TM_SQDIFF_NORMED  2 TM_CCORR
    //   3 TM_CCORR_NORMED  4 TM_CCOEFF  5 TM_CCOEFF_NORMED
    // 所以上下界是 TM_SQDIFF 和 TM_CCOEFF_NORMED —— 拿
    // TM_SQDIFF_NORMED 当上界会把 3/4/5 全判成非法。
    // (这条是写测试时先撞红的:第一版上界写成了 TM_SQDIFF_NORMED。)
if (method < cv::TM_SQDIFF || method > cv::TM_CCOEFF_NORMED) {
        output["error"] =
            QString("匹配方法 %1 不是合法值(合法范围 %2~%3,依次是 "
                    "TM_SQDIFF / TM_SQDIFF_NORMED / TM_CCORR / "
                    "TM_CCORR_NORMED / TM_CCOEFF / TM_CCOEFF_NORMED)")
                .arg(method).arg(int(cv::TM_SQDIFF)).arg(int(cv::TM_CCOEFF_NORMED));
        return output;
    }
    if (tpl.cols > img.cols || tpl.rows > img.rows) {
        output["error"] =
            QString("模板 %1x%2 比原图 %3x%4 大。cv::matchTemplate 里有 "
                    "CV_Assert(image.size() >= templ.size()),直接调会抛 "
                    "cv::Exception 把程序打崩,所以这里先拦下来。")
                .arg(tpl.cols).arg(tpl.rows).arg(img.cols).arg(img.rows);
        return output;
    }

    // ============================================================
    // CCOEFF 系列要求模板**自身**有方差。
    //
    // 这里本来写的是"跑完看 score 是不是 NaN" —— 实测(OpenCV 4.x,本机)
    // 发现我猜错了:一整块纯色模板不但没给 NaN,还给的是
    //     score = 1.000000  pass = true
    // 也就是**恒判合格**。板子再差也能过 —— 对一个检测算子来说,
    // 恒判合格比恒判不合格危险得多(不合格会有人去查,合格没人查)。
    //
    // 而且 OpenCV 这里到底返回 NaN、1.0 还是别的,依版本和实现而异
    // (0/0 的处理路径不同)。所以不能靠"检查返回值"来发现这个问题 ——
    // 必须在调用之前自己算一遍模板的方差。
    // ============================================================
    if (method == cv::TM_CCOEFF || method == cv::TM_CCOEFF_NORMED) {
        cv::Scalar mean, stddev;
        cv::meanStdDev(tpl, mean, stddev);
        const double maxStd = std::max({stddev[0], stddev[1], stddev[2]});
        if (maxStd < 1.0) {
            output["error"] =
                QString("模板各通道标准差最大只有 %1(几乎是一整块纯色),"
                        "而 %2 要求模板自身有明暗变化。这种模板匹配不出任何"
                        "东西 —— 换个位置也照样\"匹配\",恒判合格。"
                        "换一张有图案/有纹理的模板,或改用 TM_CCORR_NORMED。")
                    .arg(maxStd, 0, 'f', 3)
                    .arg(method == cv::TM_CCOEFF_NORMED
                             ? QStringLiteral("TM_CCOEFF_NORMED")
                             : QStringLiteral("TM_CCOEFF"));
            return output;
        }
    }

    cv::Mat result;
    cv::matchTemplate(img, tpl, result, method);
    cv::Point minLoc, maxLoc;
    double minVal, maxVal;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    // ============================================================
    // 相似度必须归一成"1 = 最像",不管下拉框选的是哪一种方法
    //
    // 三种下拉项的原始值方向不一样:
    //   TM_CCOEFF_NORMED / TM_CCORR_NORMED : 越大越像,完美时 ≈ 1
    //   TM_SQDIFF_NORMED                  : **越小越像**,完美时 ≈ 0
    //
    // 原来的代码对第三种取 -minVal,得到的是一个**非正数**
    // (完美匹配时正好是 -0),再拿去和 [0,1] 区间的 threshold(默认 0.9)比:
    //     -0 >= 0.9  →  恒为 false
    // 于是 pass 永远是 false、score 永远是负数,整条流水线一直判 NG ——
    // 而画布状态点是绿的,日志一切正常。
    // 产线上这就是"模板明明在图里,却一直判 NG"。
    //
    // 归一成 1 - minVal 之后,threshold 对三种方法是同一个含义,
    // 这也正是"从一个下拉框里选方法"的用户默认预期。
    // ============================================================
    const bool useMax = (method == cv::TM_CCOEFF_NORMED || method == cv::TM_CCOEFF
                     || method == cv::TM_CCORR_NORMED || method == cv::TM_CCORR);
    const double bestScore = useMax ? maxVal : 1.0 - minVal;
    const cv::Point bestLoc = useMax ? maxLoc : minLoc;

    // 兜底:上面按方差拦过了,但不同 OpenCV 版本在退化输入上的返回值
    // 并不一致(NaN / 1.0 / 其他),所以这里再挡一次非有限值 ——
    // 判据是"这个数没法参与比较",而不是猜 OpenCV 会给什么。
    if (std::isnan(bestScore) || std::isinf(bestScore)) {
        output["error"] =
            QString("匹配结果是 %1 —— 这个匹配方法要求模板有明暗变化"
                    "(标准差不能为 0),而当前模板是一整块纯色。"
                    "换一张有图案/有纹理的模板,或改用 TM_CCORR_NORMED。")
                .arg(std::isnan(bestScore) ? QStringLiteral("NaN")
                                           : QStringLiteral("无穷大"));
        return output;
    }

    output["position_x"] = bestLoc.x;
    output["position_y"] = bestLoc.y;
    output["score"]      = bestScore;
    output["pass"]       = (bestScore >= threshold);
    output["template_w"] = tpl.cols;
    output["template_h"] = tpl.rows;
    return output;
}

QWidget* TemplateMatch::createParamWidget(QWidget* parent, const QJsonObject& params) {
    Q_UNUSED(params);
    auto* form = new TemplateMatchForm(parent);
    form->initialize();   // S3-bug 修复:必须显式 init
    return form;
}

TemplateMatchForm::TemplateMatchForm(QWidget* parent) : ParamForm(parent) {}

void TemplateMatchForm::buildUI() {
    m_tplPath = new QLineEdit(QString::fromUtf8(TemplateMatchDefaults::kTemplatePath));

    m_method = new QComboBox;
    m_method->addItem("TM_CCOEFF_NORMED (推荐)", cv::TM_CCOEFF_NORMED);
    m_method->addItem("TM_CCORR_NORMED",          cv::TM_CCORR_NORMED);
    m_method->addItem("TM_SQDIFF_NORMED",         cv::TM_SQDIFF_NORMED);

    // 面板初值 = **声明的默认值**,不是"列表第一项"。
    // ⚠ 以前靠"TM_CCOEFF_NORMED 恰好排在第一位"来当默认值 ——
    //   那是**巧合**,不是声明:哪天把声明的默认值改了、
    //   或者把列表顺序调了,面板上摆的方法和算子跑的方法
    //   就不是一回事了(界面上摆一个"调了不生效"的旋钮 = 骗人)。
    //   旋钮审计的用例 3e 盯着 setValue,但 combo 的初值它看不见,
    //   这条靠写代码本身保证。
    for (int i = 0; i < m_method->count(); ++i) {
        if (m_method->itemData(i).toInt() == TemplateMatchDefaults::kMethod) {
            m_method->setCurrentIndex(i);
            break;
        }
    }

    m_threshold = new QDoubleSpinBox;
    m_threshold->setRange(0.0, 1.0);
    m_threshold->setSingleStep(0.05);
    m_threshold->setValue(TemplateMatchDefaults::kThreshold);

    m_layout->addRow(tr("模板路径"), m_tplPath);
    m_layout->addRow(tr("匹配方法"), m_method);
    m_layout->addRow(tr("相似度阈值"), m_threshold);
}

void TemplateMatchForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**。
    // ⚠ template_path 以前是 toString() 不带参数 —— 那是**空串**,
    //   不是声明值("templates/R.png"):存盘里路径存成了数字时,
    //   面板显示空串,算子却按声明路径跑 —— 界面上摆的和实际跑的
    //   不是一回事。
    const QJsonObject d = TemplateMatch().defaultParams();
    if (params.contains("template_path"))
        m_tplPath->setText(knobTextForWidget(params.value("template_path"), d, "template_path"));
    if (params.contains("method")) {
        int method = params.value("method").toInt(TemplateMatchDefaults::kMethod);
        for (int i = 0; i < m_method->count(); ++i) {
            if (m_method->itemData(i).toInt() == method) {
                m_method->setCurrentIndex(i);
                break;
            }
        }
    }
    // 兜底用**声明的默认值**(toDouble() 看着"没写兜底",其实是隐式退成 0)。
    if (params.contains("threshold"))
        m_threshold->setValue(params.value("threshold").toDouble(TemplateMatchDefaults::kThreshold));
}

QJsonObject TemplateMatchForm::collect() const {
    QJsonObject p;
    p["template_path"] = m_tplPath->text();
    p["method"]        = m_method->currentData().toInt();
    p["threshold"]     = m_threshold->value();
    return p;
}

} // namespace w7
