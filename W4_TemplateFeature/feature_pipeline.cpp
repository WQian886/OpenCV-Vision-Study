#include "feature_pipeline.h"

#include "pcb_sim.h"

#include <algorithm>
#include <cmath>

namespace w4 {
namespace {

// ------------------------------------------------------------
// 常量 —— 全部只在这里定义一次
// ------------------------------------------------------------

// 默认 method：归一化相关系数（工业首选）
const int kDefaultMethod = cv::TM_CCOEFF_NORMED;

// 模板在图上的位置（左上角那一块的起点）
const cv::Rect kTemplateRect(50, 100, 80, 80);

// ORB 模板：图上那块唯一的文字。
// 8 个焊点长得一模一样，文字只出现一次 —— 只有唯一的区域才配做 ORB 模板。
//
// ⚠ 高度 80 是**量出来的下限附近**：ORB 默认 edgeThreshold=19、patchSize=31，
// 同样宽 380 的裁剪，高 60 时一个关键点都没有，高 70 时 109 个，高 80 时 162 个。
const cv::Rect kOrbTemplateRect(90, 200, 380, 80);

// ORB 默认关键点上限
const int kDefaultFeatures = 500;

// Lowe ratio 默认值
const double kDefaultRatio = 0.75;

// 合成模板的噪声强度（比图上的 σ 小一点 —— 它是"另一张照片"）
const double kTemplateSigma = 15.0;

}  // namespace

int defaultMethod() { return kDefaultMethod; }
int defaultFeatures() { return kDefaultFeatures; }
double defaultRatio() { return kDefaultRatio; }
cv::Rect templateRect() { return kTemplateRect; }
cv::Rect orbTemplateRect() { return kOrbTemplateRect; }
const std::vector<cv::Rect>& padRects() { return pcbsim::padGrid(); }

// ------------------------------------------------------------
// 图与模板
// ------------------------------------------------------------

cv::Mat generatePcbImage(int seed) {
    // 底色、焊点颜色、焊点位置、噪声强度、图的尺寸 —— 全在 pcb_sim 里。
    // W2 / W3 / W4 从**同一份**取。
    cv::Mat img(pcbsim::imageSize(), CV_8UC3, pcbsim::backgroundColor());

    for (const auto& p : pcbsim::padGrid()) {
        cv::rectangle(img, p, pcbsim::padColor(), -1);
    }

    cv::putText(img, "W4 Template Feature Test", cv::Point(100, 240),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, pcbsim::textColor(), 2);

    // 噪声（种子是参数，副作用见 common/pcb_sim/pcb_sim.h）
    return pcbsim::addNoise(img, seed);
}

cv::Mat cutTemplate(const cv::Mat& img, const cv::Rect& r) {
    // ⚠ 裁剪范围必须**整个**在图内。
    //
    // 有一半在外面时，这里返回**空图**，而不是悄悄裁成能用的那一半 ——
    // 因为「半个模板」照样能匹配，而且分数还挺高：
    // 匹配的是**半个目标**，而报告出去的是「找到了，0.98 分」。
    // 那比「抠不到」糟得多。
    const cv::Rect bounds(0, 0, img.cols, img.rows);
    if ((r & bounds) != r) return cv::Mat();
    if (r.width <= 0 || r.height <= 0) return cv::Mat();
    return img(r).clone();
}

cv::Mat makeSyntheticTemplate(int seed) {
    // 一个"独立拍的金块"：颜色和焊点一样，但**不是**从大图上抠的，
    // 噪声也是**另一份**随机数。
    //
    // ⇒ 拿它去匹配，相似度**不会是 1.0** —— 那个分数才有信息。
    cv::Mat tpl(80, 80, CV_8UC3, pcbsim::padColor());
    cv::theRNG() = cv::RNG(seed);
    cv::Mat noise(tpl.size(), CV_16SC3);
    cv::randn(noise, cv::Scalar::all(0), cv::Scalar::all(kTemplateSigma));
    cv::Mat out;
    cv::add(tpl, noise, out, cv::noArray(), CV_8UC3);
    return out;
}

cv::Mat rotateAroundCenter(const cv::Mat& src, double angle) {
    cv::Point2f center(src.cols / 2.0f, src.rows / 2.0f);
    cv::Mat rot = cv::getRotationMatrix2D(center, angle, 1.0);
    cv::Mat out;
    cv::warpAffine(src, out, rot, src.size());
    return out;
}

// ------------------------------------------------------------
// 模板匹配
// ------------------------------------------------------------

bool higherIsBetter(int method) {
    // 平方差类：**越小**越像；其余四类：**越大**越像。
    return !(method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED);
}

cv::Mat scoreMap(const cv::Mat& img, const cv::Mat& tpl, int method) {
    const int m = (method >= 0) ? method : kDefaultMethod;
    cv::Mat result;
    cv::matchTemplate(img, tpl, result, m);
    return result;
}

Match bestMatch(const cv::Mat& scores, int method) {
    const int m = (method >= 0) ? method : kDefaultMethod;
    double minVal = 0.0, maxVal = 0.0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(scores, &minVal, &maxVal, &minLoc, &maxLoc);

    Match r;
    if (higherIsBetter(m)) {
        r.score = maxVal;
        r.loc = maxLoc;
    } else {
        r.score = minVal;
        r.loc = minLoc;
    }
    return r;
}

std::vector<Match> findMatches(const cv::Mat& scores, double threshold,
                                int method, int nmsRadius) {
    const int m = (method >= 0) ? method : kDefaultMethod;
    const bool higher = higherIsBetter(m);

    // 1. 先把过线的像素收出来（按「像的程度」从好到差排）
    std::vector<Match> candidates;
    for (int y = 0; y < scores.rows; ++y) {
        const float* row = scores.ptr<float>(y);
        for (int x = 0; x < scores.cols; ++x) {
            const double v = row[x];
            const bool hit = higher ? (v >= threshold) : (v <= threshold);
            if (hit) {
                Match mm;
                mm.score = v;
                mm.loc = cv::Point(x, y);
                candidates.push_back(mm);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [higher](const Match& a, const Match& b) {
                  return higher ? (a.score > b.score) : (a.score < b.score);
              });

    // 2. 贪心 NMS：挨着的算**同一个**目标，只留最好的那个
    std::vector<Match> kept;
    for (const auto& c : candidates) {
        bool tooClose = false;
        for (const auto& k : kept) {
            const double dx = static_cast<double>(c.loc.x - k.loc.x);
            const double dy = static_cast<double>(c.loc.y - k.loc.y);
            if (dx * dx + dy * dy <= static_cast<double>(nmsRadius) * nmsRadius) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) kept.push_back(c);
    }
    return kept;
}

cv::Mat drawMatch(const cv::Mat& bgr, const Match& m, const cv::Size& tplSize) {
    cv::Mat marked = bgr.clone();
    const cv::Rect box(m.loc.x, m.loc.y, tplSize.width, tplSize.height);
    cv::rectangle(marked, box, cv::Scalar(0, 0, 255), 3);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", m.score);
    cv::putText(marked, buf, cv::Point(m.loc.x, m.loc.y - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 1);
    return marked;
}

// ------------------------------------------------------------
// ORB
// ------------------------------------------------------------

cv::Mat detectDescriptors(const cv::Mat& bgr, int nfeatures,
                          std::vector<cv::KeyPoint>* keypoints) {
    const int n = (nfeatures > 0) ? nfeatures : kDefaultFeatures;

    cv::Mat gray;
    if (bgr.channels() == 1) {
        gray = bgr;
    } else {
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Ptr<cv::ORB> orb = cv::ORB::create(n);
    std::vector<cv::KeyPoint> kps;
    cv::Mat desc;
    orb->detectAndCompute(gray, cv::noArray(), kps, desc);

    if (keypoints) *keypoints = kps;
    return desc;
}

std::vector<cv::DMatch> matchDescriptors(const cv::Mat& descTemplate,
                                          const cv::Mat& descImage,
                                          double ratio) {
    const double r = (ratio > 0.0) ? ratio : kDefaultRatio;
    if (descTemplate.empty() || descImage.empty()) return std::vector<cv::DMatch>();

    cv::BFMatcher matcher(cv::NORM_HAMMING);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(descTemplate, descImage, knn, 2);

    std::vector<cv::DMatch> good;
    for (const auto& pair : knn) {
        if (pair.size() < 2) continue;
        // Lowe ratio：好匹配的距离要明显小于次好匹配的距离
        if (pair[0].distance < r * pair[1].distance) good.push_back(pair[0]);
    }
    return good;
}

cv::Mat estimateHomography(const std::vector<cv::KeyPoint>& kpsTemplate,
                           const std::vector<cv::KeyPoint>& kpsImage,
                           const std::vector<cv::DMatch>& good) {
    // 4 对才刚好够算一个 3x3 的单应性矩阵。
    // 不够就**返回空的** —— 给一个假的矩阵比不给更糟。
    if (good.size() < 4) return cv::Mat();

    std::vector<cv::Point2f> src, dst;
    for (const auto& m : good) {
        if (m.queryIdx < 0 || m.queryIdx >= static_cast<int>(kpsTemplate.size()))
            continue;
        if (m.trainIdx < 0 || m.trainIdx >= static_cast<int>(kpsImage.size()))
            continue;
        src.push_back(kpsTemplate[m.queryIdx].pt);
        dst.push_back(kpsImage[m.trainIdx].pt);
    }
    if (src.size() < 4) return cv::Mat();

    return cv::findHomography(src, dst, cv::RANSAC, 3.0);
}

}  // namespace w4
