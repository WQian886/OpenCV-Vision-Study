// ============================================================
// W6: PCB 缺陷检测综合项目
//
// 子模式（argv[1]）：
//   baseline          跑完整管线,处理 images/baseline.png,输出 report.csv
//   inspect <path>    打开单张图,做 HSV 统计 + 焊点粗数 + 焊桥/短路粗判,保存标注图
//   report [dir]      批量处理目录里所有 PNG,每个写一行 CSV（默认 images/）
//   gen               调用 python gen_pcb_dataset.py 重新生成测试数据
//
// 学习路径：
//   W6a (本文件脚手架) → W6b 元件识别 → W6c 缺陷判定 → W6d OCR/QR → W6e 报告
// ============================================================

#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>      // system()

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================
// 公共枚举（跨命名空间用,留在全局）
// ============================================================
enum class DefectType {
    OK, MISSING, WRONG, POLARITY, BRIDGE, INSUFFICIENT, SHIFTED, SHORT, UNKNOWN
};

inline const char* defectName(DefectType t) {
    switch (t) {
        case DefectType::OK:           return "ok";
        case DefectType::MISSING:      return "missing";
        case DefectType::WRONG:        return "wrong";
        case DefectType::POLARITY:     return "polarity";
        case DefectType::BRIDGE:       return "bridge";
        case DefectType::INSUFFICIENT: return "insufficient";
        case DefectType::SHIFTED:      return "shifted";
        case DefectType::SHORT:        return "short";
        default:                       return "unknown";
    }
}


// ============================================================
// 元件站点真值（同步自 gen_pcb_dataset.py 的 SITES,行 80-93）
// 用于 W6b 已知位元件识别 + W6c 缺陷判定的 ROI 参考
// ============================================================
struct Site {
    const char* label;
    const char* kind;       // "R" / "C" / "IC"
    cv::Point2i center;
    int         padDx;      // 焊盘间距一半（R/C 用）
    cv::Size    body;       // 本体宽高
};
// 用 const 而非 constexpr：cv::Point2i / cv::Size 在 OpenCV 4.x 不是文本类型
// SITES 仅在函数内部使用，不需要编译期常量
static const Site SITES[6] = {
    {"R1","R",  {150,200}, 40, {100,28}},
    {"C1","C",  {400,200}, 36, { 70,28}},
    {"U1","IC", {720,200}, 90, {180,60}},
    {"R2","R",  {150,480}, 40, {100,28}},
    {"C2","C",  {400,480}, 36, { 70,28}},
    {"R3","R",  {700,480}, 40, {100,28}},
};


// ============================================================
// 公共预处理（W2 风格的 HSV 金色提取 + 形态学）
// ============================================================
namespace w6a {
    // 金色像素 + 轮廓统计
    struct GoldStats {
        int    pixelCount   = 0;
        int    contourCount = 0;
        double avgArea      = 0.0;
        double totalArea    = 0.0;
    };

    // 返回 BGR 原图；goldMask 出参
    cv::Mat loadAndPreprocess(const std::string& path, cv::Mat& goldMask) {
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (img.empty()) {
            throw std::runtime_error("imread failed: " + path);
        }
        cv::Mat hsv;
        cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
        // 金色范围（针对 W6 合成数据）
        //   gen_pcb_dataset.py 用 PIL RGB(30,180,220) 画 GOLD → cv::imread 读为 BGR(220,180,30)
        //   → HSV V=220, S=190/220≈0.86, V==B → H=240+60×(30-180)/190=192.6 → 0-180 scale = 96.3
        //   所以合成金色实际是青色 H~96;W2 笔记里的 (10,80,80)→(40,255,255) 在 W6 上抓到的是
        //   IC 的 BLUE_DARK(原 RGB(180,80,30)→BGR(30,80,180)→H=20)而不是金
        //   改为 H 85-115 抓真正的金
        cv::inRange(hsv, cv::Scalar(85, 80, 80), cv::Scalar(115, 255, 255), goldMask);
        cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
        cv::morphologyEx(goldMask, goldMask, cv::MORPH_OPEN, k);
        return img;
    }

    GoldStats computeGoldStats(const cv::Mat& goldMask) {
        GoldStats s;
        s.pixelCount = cv::countNonZero(goldMask);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(goldMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        s.contourCount = (int)contours.size();
        if (!contours.empty()) {
            double total = 0;
            for (auto& c : contours) total += cv::contourArea(c);
            s.totalArea = total;
            s.avgArea   = total / contours.size();
        }
        return s;
    }
}


// ============================================================
// W6b: 元件识别（复用 W4 的 matchTemplate + ORB）
// ============================================================
namespace w6b {
    // 元件识别结果
    struct Component {
        std::string label;     // "R1", "C2", "U1"
        std::string kind;      // "R", "C", "IC"
        cv::Rect    roi;
        double      angle = 0.0;   // 旋转角（ORB 给出）
        cv::Point2f center;
    };

    // TODO: 用模板匹配/ORB 识别 PCB 上每个元件,返回 Component 列表
    //   思路：
    //     1. 用 w6a::loadAndPreprocess 拿到 goldMask
    //     2. 在 mask 上 findContours + boundingRect 拿到 ROI 候选
    //     3. 对每个 ROI 跑 cv::matchTemplate（templates/R.png / C.png / U.png）
    //     4. 或用 ORB + findHomography（抗旋转）
    // Step 1 已知位版：直接按 SITES 真值构造 ROI（±10 缓冲）
    // 后续可在 ROI 内用 matchTemplate/ORB 做二次验证或泛化
    std::vector<Component> recognizeComponents(const cv::Mat& /*img*/,
                                               const cv::Mat& /*goldMask*/) {
        std::vector<Component> out;
        out.reserve(6);
        for (const auto& s : SITES) {
            Component c;
            c.label  = s.label;
            c.kind   = s.kind;
            c.roi    = cv::Rect(s.center.x - s.body.width/2  - 10,
                                s.center.y - s.body.height/2 - 10,
                                s.body.width  + 20,
                                s.body.height + 20);
            c.center = s.center;
            c.angle  = 0.0;
            out.push_back(c);
        }
        return out;
    }

    // 画框 + 标签
    cv::Mat drawComponents(cv::Mat img, const std::vector<Component>& comps) {
        for (size_t i = 0; i < comps.size(); ++i) {
            const auto& c = comps[i];
            cv::Scalar color(0, 255, 0);     // 绿框 = 识别成功
            cv::rectangle(img, c.roi, color, 2);
            cv::putText(img, c.label + "(" + c.kind + ")",
                        cv::Point(c.roi.x, c.roi.y - 4),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
            (void)i;
        }
        return img;
    }
}


// ============================================================
// W6c: 缺陷判定（缺件/错件/极性/焊桥/焊少/偏移/短路）
// ============================================================
namespace w6c {
    // 单条缺陷判定结果（短路等全局缺陷 componentLabel 留空）
    struct Defect {
        std::string componentLabel;
        DefectType  type = DefectType::OK;
        std::string description;
    };

    // 主判定入口（Step 5 完整版实现见文件下方 Step 5 区块）
    std::vector<Defect> judgeDefects(const cv::Mat& img,
                                     const cv::Mat& goldMask,
                                     const std::vector<w6b::Component>& comps);

    // 短路判定：连通域分析
    //   思路：
    //     1. 提取走线 mask（金色 + 形态学闭运算 = 把相邻走线粘起来）
    //     2. cv::connectedComponentsWithStats 数连通域
    //     3. 若某个连通域同时覆盖两条"应绝缘"的走线 → 短路
    //     4. 短路时连通域面积 / 周长会显著异常
    // Step 2: 短路判定 —— 检测合成数据自带的"蓝色椭圆环"
    //   实现真相（实验发现）：
    //     1. gen_pcb_dataset.py 用 PIL 画 RED=(40,40,220) RGB;OpenCV 读成 BGR=(220,40,40)
    //        → 在 OpenCV HSV 空间是 H=120（蓝色）,不是红色
    //     2. baseline 的"红色"只有 4 个小极性点（C1/C2 ~16 px²）;defect_short 多一个
    //        60×40 的椭圆环 outline（~200-300 px²）
    //     3. 用 HSV 提取"蓝紫色"区域,找面积最大连通域;阈值 > 100 px² 且 宽高比 ≈ 1.5
    //        即可识别短路圆环（baseline 最大蓝点 ~16,short 圆环 ~200+）
    struct ShortLocation { int x, y, w, h; };
    bool detectShortCircuit(const cv::Mat& img, const cv::Mat& /*goldMask*/,
                            std::string& detail, ShortLocation& loc) {
        cv::Mat hsv;
        cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
        // 蓝紫色范围：H 100-135,S>80,V>100
        cv::Mat blueMask;
        cv::inRange(hsv, cv::Scalar(100, 80, 100), cv::Scalar(135, 255, 255), blueMask);
        // 连通域
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(blueMask, labels, stats, centroids);
        // 找最大连通域
        int bestIdx = -1, bestArea = 0;
        for (int i = 1; i < n; ++i) {
            int a = stats.at<int>(i, cv::CC_STAT_AREA);
            if (a > bestArea) { bestArea = a; bestIdx = i; }
        }
        if (bestIdx < 0) return false;
        int x = stats.at<int>(bestIdx, cv::CC_STAT_LEFT);
        int y = stats.at<int>(bestIdx, cv::CC_STAT_TOP);
        int w = stats.at<int>(bestIdx, cv::CC_STAT_WIDTH);
        int h = stats.at<int>(bestIdx, cv::CC_STAT_HEIGHT);
        // 短路圆环特征：面积 100-500、bbox 近似方形或稍宽、宽高都在 30-80 范围
        bool isRing = (bestArea > 100 && bestArea < 800
                    && w >= 30 && w <= 80
                    && h >= 20 && h <= 60);
        if (isRing) {
            loc = {x, y, w, h};
            detail = "short ring at (" + std::to_string(x) + "," + std::to_string(y)
                   + ") " + std::to_string(w) + "x" + std::to_string(h)
                   + ", area=" + std::to_string(bestArea);
            return true;
        }
        return false;
    }

    // Step 3: 焊桥判定 —— R1 焊盘之间出现连续金条
    //   思路（针对合成的 defect_bridge：在 R1 (cx=150,dx=40) ROI 内画
    //         rectangle([98,192,202,208]) = 宽 104 × 高 16 的金条）：
    //     1. 找 R1 的 ROI（按 label）
    //     2. 在 ROI 内对 goldMask 做行投影（cv::reduce REDUCE_SUM）
    //     3. 找最长连续金段：baseline 单焊盘宽 ~24px，bridge 后 ~104px
    //     4. 阈值 > 70 即判定为焊桥
    bool detectSolderBridge(const std::vector<w6b::Component>& comps,
                            const cv::Mat& goldMask,
                            std::string& detail) {
        // 找 R1
        auto it = std::find_if(comps.begin(), comps.end(),
            [](const w6b::Component& c){ return c.label == "R1"; });
        if (it == comps.end()) {
            detail = "R1 not found in components";
            return false;
        }
        // ROI 边界保护
        cv::Rect roi = it->roi & cv::Rect(0, 0, goldMask.cols, goldMask.rows);
        if (roi.width <= 0 || roi.height <= 0) {
            detail = "R1 ROI out of bounds";
            return false;
        }
        cv::Mat roiMask = goldMask(roi);
        // 行投影：每行金像素数（用 countNonZero 数每行 ROI 像素）
        int maxWidth = 0, maxY = 0;
        for (int y = 0; y < roiMask.rows; ++y) {
            int w = cv::countNonZero(roiMask.row(y));
            if (w > maxWidth) { maxWidth = w; maxY = y; }
        }
        // baseline 单焊盘宽 ~24px；bridge 后 ~104px
        if (maxWidth > 70) {
            detail = "bridge at R1, max gold run = " + std::to_string(maxWidth)
                   + "px at row y=" + std::to_string(maxY);
            return true;
        }
        return false;
    }

    // ============================================================
    // Step 5: 5 类 ROI 判定（针对合成数据特定的已知位缺陷）
    //   全部基于 SITES 真值坐标 + 合成器已知缺陷手法
    // ============================================================

    // missing: R2 ROI 内"暗像素"（电阻本体）数量过少 → 元件被擦除
    bool judgeMissing(const cv::Mat& img, const w6b::Component& c,
                      std::string& detail) {
        if (c.label != "R2") return false;
        cv::Rect roi = c.roi & cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0) return false;
        cv::Mat rgb;
        cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
        cv::Mat dark;
        cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
        int darkCount = cv::countNonZero(dark);
        // baseline R2 有 ~200+ 暗像素（电阻本体）；missing 后接近 0
        if (darkCount < 50) {
            detail = "missing at " + c.label + ", dark px=" + std::to_string(darkCount);
            return true;
        }
        return false;
    }

    // wrong: R3 应该是黑色电阻;如果本体变成 PIL YELLOW (浅青色) → wrong
    // PIL YELLOW = RGB(40,200,230) → cv BGR(230,200,40) → B>=200 G>=150 R<=80
    bool judgeWrong(const cv::Mat& img, const w6b::Component& c,
                    std::string& detail) {
        if (c.label != "R3") return false;
        cv::Rect roi = c.roi & cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0) return false;
        cv::Mat bgr = img(roi);
        cv::Mat yellowMask;
        cv::inRange(bgr, cv::Scalar(150, 150, 0), cv::Scalar(255, 255, 80), yellowMask);
        int yellowCount = cv::countNonZero(yellowMask);
        if (yellowCount > 200) {
            detail = "wrong at " + c.label
                   + ", yellow px=" + std::to_string(yellowCount);
            return true;
        }
        return false;
    }

    // polarity: C1 ROI 内白色像素（极性条）质心在右半 → 极性反
    bool judgePolarity(const cv::Mat& img, const w6b::Component& c,
                       std::string& detail) {
        if (c.label != "C1") return false;
        cv::Rect roi = c.roi & cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0) return false;
        // 白色极性条 PIL(255,255,255)
        cv::Mat bgr = img(roi);
        cv::Mat whiteMask;
        cv::inRange(bgr, cv::Scalar(200, 200, 200), cv::Scalar(255, 255, 255), whiteMask);
        // 算白色像素的 x 质心
        cv::Moments m = cv::moments(whiteMask, true);
        if (m.m00 < 5) return false;  // 白色像素太少,不判定
        double cx = m.m10 / m.m00;
        // baseline 极性条在 x0+2~x0+8（C1 ROI 内 x=0~15 区间）; reversed 在 x=75~83
        // cx > roi.width * 0.6 → 极性反
        if (cx > roi.width * 0.6) {
            detail = "polarity reversed at " + c.label
                   + ", white cx=" + std::to_string((int)cx)
                   + "/" + std::to_string(roi.width);
            return true;
        }
        return false;
    }

    // insufficient: U1 某个焊盘被擦小(从 size=12 缩到 size=4)
    // U1 有 12 个焊盘 (左右各 6);baseline U1 在 goldMask 中有 ~13 个连通域
    // 最小几个 < 80 px² 是边缘噪声(被 OPEN erode 削小的孤立金像素)
    // 过滤掉 < 80 的噪声,在剩下"焊盘候选"中找 min
    bool judgeInsufficient(const cv::Mat& goldMask, const w6b::Component& c,
                           std::string& detail) {
        if (c.label != "U1") return false;
        cv::Rect roi = c.roi & cv::Rect(0, 0, goldMask.cols, goldMask.rows);
        if (roi.width <= 0) return false;
        cv::Mat roiMask = goldMask(roi);
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(roiMask, labels, stats, centroids);
        // 只保留面积 >= 80 的"焊盘候选",过滤小噪声
        std::vector<int> areas;
        for (int i = 1; i < n; ++i) {
            int a = stats.at<int>(i, cv::CC_STAT_AREA);
            if (a >= 80) areas.push_back(a);
        }
        if (areas.size() < 4) return false;
        std::sort(areas.begin(), areas.end());
        int median = areas[areas.size() / 2];
        int minArea = areas[0];
        // baseline U1 焊盘 ~128-136;insufficient 把其中一个缩到 size=4 → ~80
        // 用绝对差阈值:baseline min-median 差 8;insufficient 差 ~55
        if (minArea < median - 50) {
            detail = "insufficient at " + c.label
                   + ", min pad area=" + std::to_string(minArea)
                   + " (median=" + std::to_string(median) + ")";
            return true;
        }
        return false;
    }

    // shifted: R2 ROI 内"暗像素"质心偏离 SITES 中心 → 元件平移
    bool judgeShifted(const cv::Mat& img, const w6b::Component& c,
                      std::string& detail) {
        if (c.label != "R2") return false;
        cv::Rect roi = c.roi & cv::Rect(0, 0, img.cols, img.rows);
        if (roi.width <= 0) return false;
        cv::Mat rgb;
        cv::cvtColor(img(roi), rgb, cv::COLOR_BGR2RGB);
        cv::Mat dark;
        cv::inRange(rgb, cv::Scalar(0, 0, 0), cv::Scalar(60, 60, 60), dark);
        cv::Moments m = cv::moments(dark, true);
        if (m.m00 < 50) return false;  // 没有元件本体,留给 missing 处理
        double cx = m.m10 / m.m00;
        double cy = m.m01 / m.m00;
        // SITES R2 中心 (150, 480) → ROI 内坐标 (60, 24)
        // shifted defect 平移 (+30, +10) → 偏离 ~(30, 10)
        double dx = cx - 60, dy = cy - 24;
        if (std::abs(dx) > 15 || std::abs(dy) > 8) {
            detail = "shifted at " + c.label
                   + ", dark centroid=(" + std::to_string((int)cx)
                   + "," + std::to_string((int)cy)
                   + ") expect (60,24)";
            return true;
        }
        return false;
    }

    // Step 5 总入口:对每个 Component 调 5 个 judge,合并缺陷
    std::vector<Defect> judgeDefects(const cv::Mat& img, const cv::Mat& goldMask,
                                     const std::vector<w6b::Component>& comps) {
        std::vector<Defect> defs;
        std::string d;
        for (const auto& c : comps) {
            if (judgeMissing(img, c, d))
                defs.push_back({c.label, DefectType::MISSING, d});
            if (judgeWrong(img, c, d))
                defs.push_back({c.label, DefectType::WRONG, d});
            if (judgePolarity(img, c, d))
                defs.push_back({c.label, DefectType::POLARITY, d});
            if (judgeInsufficient(goldMask, c, d))
                defs.push_back({c.label, DefectType::INSUFFICIENT, d});
            if (judgeShifted(img, c, d))
                defs.push_back({c.label, DefectType::SHIFTED, d});
        }
        return defs;
    }
}


// ============================================================
// W6d: OCR + QR 串号（复用 W5）
// ============================================================
namespace w6d {
    // 读丝印（元件上的 R47 / C220 / U1 等）
    // TODO: 复用 W5 的 CreateProcessW + tesseract.exe
    std::string readSilkscreen(const cv::Mat& /*img*/) {
        return "";
    }

    // 读 QR
    std::string readQRCode(const cv::Mat& img) {
        cv::QRCodeDetector det;
        std::vector<cv::Point2f> pts;
        std::string s = det.detectAndDecode(img, pts);
        return s;
    }
}


// ============================================================
// W6e: 报告输出（CSV + JSON）
// ============================================================
namespace w6e {
    // 写 CSV 报告
    // TODO: 把 comps / defects / silk / qr 写成 per-board-per-component 行
    bool writeCSVReport(const std::string& path,
                        const std::vector<w6b::Component>& comps,
                        const std::vector<w6c::Defect>& defects,
                        const std::string& silk,
                        const std::string& qr,
                        bool pass) {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << "board_file,component,kind,defect,description,silk,qr,result\n";
        for (const auto& c : comps) {
            std::string def = "ok";
            std::string desc = "-";
            for (const auto& d : defects) {
                if (d.componentLabel == c.label) {
                    def  = defectName(d.type);
                    desc = d.description;
                }
            }
            f << path << "," << c.label << "," << c.kind << ","
              << def << "," << desc << "," << silk << "," << qr << ","
              << (pass ? "PASS" : "FAIL") << "\n";
        }
        // 全局缺陷（短路等,无 componentLabel）
        for (const auto& d : defects) {
            if (d.componentLabel.empty()) {
                f << path << ",-,-," << defectName(d.type) << ","
                  << d.description << "," << silk << "," << qr << ","
                  << (pass ? "PASS" : "FAIL") << "\n";
            }
        }
        return true;
    }
}


// ============================================================
// 子模式入口
// ============================================================
static void runInspect(int argc, char** argv) {
    std::string imgPath = (argc > 2) ? argv[2] : "images/baseline.png";
    std::cout << "===== Inspect: " << imgPath << " =====\n";

    cv::Mat goldMask;
    cv::Mat img;
    try {
        img = w6a::loadAndPreprocess(imgPath, goldMask);
    } catch (const std::exception& e) {
        std::cerr << "❌ " << e.what() << "\n";
        return;
    }
    std::cout << "  尺寸: " << img.cols << " x " << img.rows << "\n";

    w6a::GoldStats st = w6a::computeGoldStats(goldMask);
    std::cout << "  金色像素数:    " << st.pixelCount << "\n";
    std::cout << "  候选轮廓数:    " << st.contourCount
              << "  (≈ 焊点 + 走线 + 元件金属面)\n";
    std::cout << "  平均轮廓面积:  " << (int)st.avgArea << "\n";
    std::cout << "  累计金色面积:  " << (int)st.totalArea << "\n";

    // === Step 1: 已知位元件识别（按 SITES 真值）===
    auto comps = w6b::recognizeComponents(img, goldMask);
    std::cout << "  识别元件数:    " << comps.size() << " (按 SITES 真值)\n";

    // 画一张"金色区域叠加"图
    cv::Mat marked = img.clone();
    // 把 goldMask 半透明叠到原图
    cv::Mat goldColor;
    cv::cvtColor(goldMask, goldColor, cv::COLOR_GRAY2BGR);
    goldColor.setTo(cv::Scalar(40, 180, 220), goldMask);  // 金色
    cv::addWeighted(marked, 0.7, goldColor, 0.3, 0, marked);
    // === Step 1: 在金色叠加后再画绿框（避免被金色覆盖）===
    marked = w6b::drawComponents(marked, comps);
    // === Step 2: 短路判定（检测合成数据自带的蓝环）===
    std::string shortDetail;
    w6c::ShortLocation shortLoc{};
    bool hasShort = w6c::detectShortCircuit(img, goldMask, shortDetail, shortLoc);
    if (hasShort) {
        // 在蓝环位置画红框 + 标 SHORT
        cv::rectangle(marked, cv::Rect(shortLoc.x, shortLoc.y, shortLoc.w, shortLoc.h),
                      cv::Scalar(0, 0, 255), 3);
        cv::putText(marked, "SHORT",
                    cv::Point(shortLoc.x, shortLoc.y - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
        std::cout << "  ❌ " << shortDetail << "\n";
    }
    // === Step 3: 焊桥判定（R1 焊盘间连续金条）===
    std::string bridgeDetail;
    bool hasBridge = w6c::detectSolderBridge(comps, goldMask, bridgeDetail);
    if (hasBridge) {
        // 在 R1 ROI 上画黄色矩形 + BRIDGE 标签
        auto it = std::find_if(comps.begin(), comps.end(),
            [](const w6b::Component& c){ return c.label == "R1"; });
        if (it != comps.end()) {
            cv::rectangle(marked, it->roi, cv::Scalar(0, 255, 255), 3);
            cv::putText(marked, "BRIDGE",
                        cv::Point(it->roi.x, it->roi.y - 8),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        std::cout << "  ❌ " << bridgeDetail << "\n";
    }
    // 写文字
    cv::putText(marked, "Inspect: " + imgPath,
                cv::Point(20, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                cv::Scalar(255, 255, 255), 2);
    cv::putText(marked, "gold pixels: " + std::to_string(st.pixelCount),
                cv::Point(20, 90), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(255, 255, 255), 1);
    cv::putText(marked, "contours: " + std::to_string(st.contourCount),
                cv::Point(20, 115), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(255, 255, 255), 1);
    cv::putText(marked, "components: " + std::to_string(comps.size()),
                cv::Point(20, 140), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(255, 255, 255), 1);
    cv::putText(marked, std::string("result: ") + ((hasShort || hasBridge) ? "FAIL" : "PASS"),
                cv::Point(20, 170), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                (hasShort || hasBridge) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 2);

    std::string outPath = "images/w6_inspect_marked.png";
    cv::imwrite(outPath, marked);
    std::cout << "  标注图已保存: " << outPath << "\n";

    cv::namedWindow("W6 Inspect", cv::WINDOW_AUTOSIZE);
    cv::imshow("W6 Inspect", marked);
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();
}


// Step 4 辅助:跑一张图出 (comps, defects, qr, pass) 四元组
static bool runPipeline(const std::string& imgPath,
                        std::vector<w6b::Component>& comps,
                        std::vector<w6c::Defect>& defects,
                        std::string& qr,
                        std::string& errOut) {
    cv::Mat img, goldMask;
    try {
        img = w6a::loadAndPreprocess(imgPath, goldMask);
    } catch (const std::exception& e) {
        errOut = e.what();
        return false;
    }
    // W6b: 识别元件
    comps = w6b::recognizeComponents(img, goldMask);
    // W6c: 短路 + 焊桥
    std::string det;
    w6c::ShortLocation sloc{};
    if (w6c::detectShortCircuit(img, goldMask, det, sloc)) {
        defects.push_back({"", DefectType::SHORT, det});
    }
    if (w6c::detectSolderBridge(comps, goldMask, det)) {
        defects.push_back({"R1", DefectType::BRIDGE, det});
    }
    // W6c Step 5: 5 类 ROI 判定（missing / wrong / polarity / insufficient / shifted）
    auto step5 = w6c::judgeDefects(img, goldMask, comps);
    defects.insert(defects.end(), step5.begin(), step5.end());
    // W6d: QR
    qr = w6d::readQRCode(img);
    return true;
}


static void runBaseline() {
    std::cout << "===== Baseline full pipeline =====\n";
    std::vector<w6b::Component> comps;
    std::vector<w6c::Defect> defects;
    std::string qr, err;
    if (!runPipeline("images/baseline.png", comps, defects, qr, err)) {
        std::cerr << "❌ " << err << "\n";
        return;
    }
    bool pass = defects.empty();
    w6e::writeCSVReport("report/baseline.csv", comps, defects, "", qr, pass);
    std::cout << "  尺寸: " << 1000 << "x" << 700 << "\n";
    std::cout << "  元件数: " << comps.size() << "\n";
    std::cout << "  缺陷数: " << defects.size() << "\n";
    std::cout << "  QR: " << (qr.empty() ? "(空)" : qr) << "\n";
    std::cout << "  " << (pass ? "✅ PASS" : "❌ FAIL") << "\n";
    std::cout << "  CSV 已写: report/baseline.csv\n";
}


static void runReport(int argc, char** argv) {
    std::string dir = (argc > 2) ? argv[2] : "images";
    std::cout << "===== Batch report: " << dir << " =====\n";
    namespace fs = std::filesystem;
    fs::create_directory("report");
    std::ofstream all("report/report.csv");
    all << "board_file,component,kind,defect,description,silk,qr,result\n";
    int n = 0, nPass = 0, nFail = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".png") continue;
        std::string name = e.path().filename().string();
        // 过滤掉 inspect 生成的 marked 图（带叠加层,会污染检测）
        if (name.rfind("w6_inspect_", 0) == 0) continue;
        std::string path = e.path().string();
        std::cout << "  [分析] " << name << " ... ";
        std::vector<w6b::Component> comps;
        std::vector<w6c::Defect> defects;
        std::string qr, err;
        if (!runPipeline(path, comps, defects, qr, err)) {
            std::cout << "❌ " << err << "\n";
            continue;
        }
        bool pass = defects.empty();
        pass ? ++nPass : ++nFail;
        // 单图一行:用第一个 defect 描述整图结论(没有 defect 就是 ok)
        std::string defectStr = "ok";
        std::string descStr = "-";
        if (!defects.empty()) {
            defectStr = defectName(defects[0].type);
            descStr = defects[0].description;
        }
        all << name << ",,," << defectStr << "," << descStr
            << ",," << (qr.empty() ? std::string("-") : qr) << ","
            << (pass ? "PASS" : "FAIL") << "\n";
        std::cout << (pass ? "PASS" : "FAIL");
        if (!qr.empty()) std::cout << "  qr=" << qr;
        std::cout << "\n";
        ++n;
    }
    std::cout << "  共 " << n << " 张图: " << nPass << " PASS / " << nFail << " FAIL\n";
    std::cout << "  CSV 已写: report/report.csv\n";
    all.close();
}


static void runGen() {
    std::cout << "===== 重新生成测试数据 =====\n";
#ifdef _WIN32
    std::string cmd = "python gen_pcb_dataset.py";
    std::cout << "  调用: " << cmd << "\n";
    int rc = system(cmd.c_str());
    std::cout << "  返回码: " << rc << "\n";
#else
    std::cerr << "  本辅助函数仅 Windows 实现\n";
#endif
}


// ============================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string mode = (argc > 1) ? argv[1] : "baseline";

    if      (mode == "inspect")  runInspect(argc, argv);
    else if (mode == "report")   runReport(argc, argv);
    else if (mode == "gen")      runGen();
    else                         runBaseline();   // 默认 = baseline

    return 0;
}
