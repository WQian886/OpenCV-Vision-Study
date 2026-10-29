#pragma once
// ============================================================
// W4 的可测逻辑层：模板匹配 + ORB 特征点
// ============================================================
//
// ## 为什么要抽出这一层
//
// 2026-10-29 之前，W4 的整条链写在 main() 里：
// 生成图 → 抠模板 → matchTemplate → minMaxLoc → 画红框 → imwrite → imshow。
//
// 那样写的后果不是「代码难看」，是**这一整层一条测试都写不了**：
// imshow/waitKey 要显示器，imwrite 要往磁盘写 —— 两个都不是无头环境能做的。
// 而 W4 在 2026-10-29 之前**一条自动化测试都没有**（欠账总账里的一条）。
//
// 照仓库里已有的先例做（W2 的 color_pipeline.h、W3 的 edge_pipeline.h、
// W6 的 w6_cli.h）：**把纯逻辑抽出来，main.cpp 调它，测试也调它** ——
// 一份实现，两个调用方。
//
// ## 这里暴露的是「语义」，不是「OpenCV 调用序列」
//
// 函数名说的是「算相似度图」「找最佳匹配」「找所有超过阈值的匹配」，
// 不是「先 matchTemplate 再 minMaxLoc」。
// 这样测试断的是**行为**，不是实现细节 ——
// 换一种求极值的写法不该让测试变红。
//
// ## ⚠ 常量只有这一份
//
// 模板在图上的位置、默认 method、NMS 半径、ORB 的关键点上限、
// Lowe ratio —— 全在 feature_pipeline.cpp 里，demo 和测试读的是**同一份**。
//
// 抄一份到测试里，就是本仓库栽过五遍的「抄副本」：
// 改了 demo 的门槛，测试还在按旧门槛断言，
// 于是测试「绿着」而 demo 已经变了 —— 绿灯什么也没证明。
//
// ## ⚠ 「合成 PCB 图的事实」**不在这个文件里**
//
// 它在 common/pcb_sim/pcb_sim.h —— W2 / W3 / W4 用的是同一份。
//
// 2026-10-29 实测：W4 原来的 main.cpp 里**内联抄了一份**生成器，
// 尺寸、8 个焊点的矩形、颜色、噪声强度**逐条与 pcb_sim 相同**，
// 而那行注释写着「W3 同款 8 个金色元件」——
// **注释说同款，代码是抄的。**  这里不接。
//
// ============================================================

#include <opencv2/opencv.hpp>

#include <vector>

namespace w4 {

// ------------------------------------------------------------
// 演示图与模板
// ------------------------------------------------------------

// 生成 W4 的模拟 PCB 图：640x480，8 个金色焊点 + 中央文字 + 噪声。
//
// 事实全部来自 common/pcb_sim/pcb_sim.h（W2 / W3 共用同一份）。
//
// seed 是**参数**，不是内部随机数 ——
// 2026-10-29 之前这个生成器用 cv::randn 却**没有设种子**，
// 同一份代码两次跑出来的是两张不同的图，
// 「这张图长这样」这句话根本没法被引用，测试也就无从写起。
cv::Mat generatePcbImage(int seed = 20261029);

// 模板在图上的位置：demo 从这里抠一块下来当模板
cv::Rect templateRect();

// 从大图上抠一块下来当模板（"用手机拍的元件" 的自包含版）
//
// ⚠ 裁剪范围必须**整个**在图内；有一半在外面就返回**空图** ——
// 悄悄裁成半个是更糟的失败：半个模板照样能匹配，分数还挺高。
cv::Mat cutTemplate(const cv::Mat& img, const cv::Rect& r);

// 一个**独立**的金块模板（不是从大图抠的）。
//
// 为什么需要它：模板**从大图里抠**时，它和大图的那一块**逐像素相同**，
// 匹配分数必然是 **1.0** —— 那个分数是**白送的**，不含任何信息
// （任何图和自己匹配都是 1.0）。
// 独立的模板才有「像不像」的含义。
cv::Mat makeSyntheticTemplate(int seed = 20261030);

// ORB 模板：图上那块唯一的文字（"W4 Template Feature Test"）。
//
// 为什么用文字而不是金块：8 个焊点**长得一模一样**，
// 拿金块当 ORB 模板，匹配结果在 8 个位置**并列** ——
// 而文字在整张图里**只出现一次**，位置是唯一的。
//
// ⚠ 这块必须**够高**：ORB 默认 edgeThreshold=19、patchSize=31，
// 模板比这矮就把特征全当边界扔了 ——
// 实测同样宽 380 的裁剪，高 60 时**一个关键点都没有**，高 80 时有 162 个。
cv::Rect orbTemplateRect();

// 8 个金色焊点的位置（W2 / W3 / W4 共用同一份事实，在 common/pcb_sim 里）
const std::vector<cv::Rect>& padRects();

// 把图绕中心旋转 angle 度（ORB 的抗旋转那一半用得到）
cv::Mat rotateAroundCenter(const cv::Mat& src, double angle);

// ------------------------------------------------------------
// 模板匹配
// ------------------------------------------------------------

// 默认的 method：TM_CCOEFF_NORMED（工业首选）
int defaultMethod();

// 这个 method 是「越大越像」还是「越小越像」。
//
// ⚠ 这一条是**真会被写错**的：TM_SQDIFF / TM_SQDIFF_NORMED
// 要取**最小值**，其余四种取**最大值**。
// 取反了不会报错 —— OpenCV 照样给你一张图、一个位置 ——
// 只是那个位置是**最不像**的地方。
bool higherIsBetter(int method);

// 相似度图：每个像素 = 模板放在那个位置的分数。
//
// method < 0 表示用 defaultMethod()。
cv::Mat scoreMap(const cv::Mat& img, const cv::Mat& tpl, int method = -1);

// 一次匹配
struct Match {
    double score = 0.0;   // 分数（越大越像 还是 越小越像 由 method 定）
    cv::Point loc;        // 模板**左上角**在大图里的位置
};

// 最好的一次匹配。
//
// method < 0 表示用 defaultMethod()。
// 这一层负责**按 method 决定取哪个极值** ——
// 「取极值」这件事本身是这一层的约定，不该散在每个调用方手里。
Match bestMatch(const cv::Mat& scores, int method = -1);

// 找出**所有**超过（低于）阈值的匹配，贪心 NMS 去掉挨着的重复。
//
// · higherIsBetter(method) 为真：保留 score >= threshold，按分数**从大到小**排；
//   否则：保留 score <= threshold，按分数**从小到大**排。
// · nmsRadius：两个匹配点之间至少隔这么多像素才算**两个**匹配。
//   （模板 80x80 时，挨着的两个位置其实是同一个目标。）
//
// 返回按「像的程度」排好序的列表。
std::vector<Match> findMatches(const cv::Mat& scores, double threshold,
                                int method = -1, int nmsRadius = 20);

// 把一次匹配画到图上：红色外接框 + 分数文字
cv::Mat drawMatch(const cv::Mat& bgr, const Match& m, const cv::Size& tplSize);

// ------------------------------------------------------------
// ORB 特征点
// ------------------------------------------------------------

// 默认的关键点上限（cv::ORB::create 的 nfeatures）
int defaultFeatures();

// 默认的 Lowe ratio（比率检验）：好匹配的距离 < ratio × 次好距离
double defaultRatio();

// 检测关键点 + 算描述子。
//
// 输入 BGR 或灰度都行，内部自己转灰度（ORB 只吃单通道）。
// keypoints 传 nullptr 就不要关键点。
// nfeatures <= 0 表示用 defaultFeatures()。
//
// ⚠ 描述子的**行数**是「这张图上找到的关键点数」，
// 它**小于等于** nfeatures，而且**天生依赖 nfeatures** ——
// 断言「找到 N 个关键点」是在断言一个**参数的产物**。
cv::Mat detectDescriptors(const cv::Mat& bgr, int nfeatures,
                          std::vector<cv::KeyPoint>* keypoints);

// 暴力匹配 + 比率检验，返回「好匹配」。
//
// ratio <= 0 表示用 defaultRatio()。
std::vector<cv::DMatch> matchDescriptors(const cv::Mat& descTemplate,
                                          const cv::Mat& descImage,
                                          double ratio = -1.0);

// 由好匹配算单应性矩阵：把**模板坐标**映射到**大图坐标**。
//
// 好匹配 < 4 对时返回**空矩阵**（算不出来就是算不出来，不给一个假的）。
cv::Mat estimateHomography(const std::vector<cv::KeyPoint>& kpsTemplate,
                           const std::vector<cv::KeyPoint>& kpsImage,
                           const std::vector<cv::DMatch>& good);

}  // namespace w4
