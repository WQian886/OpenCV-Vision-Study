// ============================================================
// present_pipeline —— 见 present_pipeline.h 的开头(为什么有这一层)
// ============================================================

#include "present_pipeline.h"

#include <opencv2/imgproc.hpp>

#include <regex>
#include <sstream>

namespace w5 {

// ============================================================
// 常量:全部只在这里定义一次
// ============================================================

// 画框的颜色和粗细 —— demo 和测试用的是同一份定义。
namespace {
constexpr int kMarkThickness = 3;    // 四边形 / 外框的线宽
constexpr int kTextThickness = 2;    // 字画的线宽
constexpr double kQrFontScale = 0.7; // 串号的字大小
constexpr double kOcrFontScale = 0.8;// "OCR:" 的字大小
constexpr int kQrTextDy = -10;       // 串号写在角点**上方**多少像素
constexpr int kOcrTextX = 10;        // "OCR:" 的落笔位置
constexpr int kOcrTextY = 30;
} // namespace

const char* const kSerialPattern = "PCB-\\d{4}-\\d{3}-SN:[A-Z0-9]+";

// ============================================================
// 串号
// ============================================================
Serial findSerial(const std::string& payload) {
    Serial out;

    // ⚠ **不区分大小写**,而且是**搜索**不是整串匹配 ——
    //   理由见 present_pipeline.h 开头「两边怎么用它」那一段:
    //   要和 CI 第 4 步那道门禁(PowerShell 的 -match)**行为一致**。
    static const std::regex re(kSerialPattern,
                               std::regex::ECMAScript | std::regex::icase);

    std::smatch m;
    if (std::regex_search(payload, m, re)) {
        out.found = true;
        out.text = m.str(0);   // 原样,不改成大写
        return out;
    }

    out.found = false;
    out.reason = "这一段里没有流水号(期望 PCB-<4位>-<3位>-SN:<字母数字>)";
    return out;
}

// ============================================================
// 标注
// ============================================================
cv::Mat markQr(const cv::Mat& img,
               const std::vector<cv::Point2f>& corners,
               const std::string& label) {
    if (img.empty()) return img;

    cv::Mat marked = img.clone();

    // 角点**不是 4 个**就不画 —— 不猜、不凑、不崩。
    //
    // ⚠ 画错顺序会得到一个**蝴蝶结**(对角连线),
    //   而它看上去也挺像回事 ⇒ 这条不是「防手滑」,是**真的会发生**的错。
    if (corners.size() != 4) return marked;

    const cv::Scalar kRed(0, 0, 255);
    for (int i = 0; i < 4; ++i) {
        cv::line(marked, corners[i], corners[(i + 1) % 4], kRed, kMarkThickness);
    }

    // 串号写在**左上角点**的上方。
    //
    // ⚠ 角点贴到图的上边时,y 会是负数 ⇒ OpenCV **把它裁掉**,不崩。
    //   这一条被测试钉住了(见 tests/ 里「角点贴边」那一条)。
    cv::putText(marked, label,
                cv::Point(static_cast<int>(corners[0].x),
                          static_cast<int>(corners[0].y) + kQrTextDy),
                cv::FONT_HERSHEY_SIMPLEX, kQrFontScale, kRed, kTextThickness);

    return marked;
}

cv::Mat markOcr(const cv::Mat& img) {
    if (img.empty()) return img;

    cv::Mat marked = img.clone();

    const cv::Scalar kGreen(0, 255, 0);
    cv::rectangle(marked, cv::Rect(0, 0, img.cols, img.rows),
                  kGreen, kMarkThickness);
    cv::putText(marked, "OCR:",
                cv::Point(kOcrTextX, kOcrTextY),
                cv::FONT_HERSHEY_SIMPLEX, kOcrFontScale, kGreen, kTextThickness);

    return marked;
}

// ============================================================
// 文字
// ============================================================
std::vector<std::string> splitTokens(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ' ')) {
        if (!token.empty()) out.push_back(token);
    }
    return out;
}

std::string qrDecodedLine(const std::string& payload) {
    return "  ✅ 解码: " + payload;
}

} // namespace w5
