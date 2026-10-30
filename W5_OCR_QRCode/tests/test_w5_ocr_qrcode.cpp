// ============================================================
// W5（QR + OCR）的无头回归测试
// ============================================================

// ## 这个套件在补什么
//
// 2026-10-30 之前，W5 **一条自动化测试都没有** ——
// 它是欠账总账里「W3–W5 零测试缺口」那一条的**最后一笔**
// （W1/W2 是 2026-10-26，W3 是 2026-10-28，W4 是 2026-10-29）。
//
// 原因不是「没时间」。W5 的情况**和 W1–W4 不一样**，得说清楚：
//
//   · QR 解码、tesseract 调用 —— **已经在 common/qr_ocr/ 里了**，
//     而且那一层**已经有自己的套件**（36 条断言，注册在 W7 的 qr_ocr 下）。
//     ⇒ 这一套**不重复测**它。重复测就是「抄副本」的另一种写法。
//   · W5 **自己**剩下的是「**给人看**」那一层：
//     画四边形、写串号、把 OCR 文字一行一个地列出来。
//     它**从来没被测过** —— 而它恰好是 W5 这个 demo 的全部内容。
//
// 现在把那一层抽到 present_pipeline.{h,cpp}（main.cpp 和这个套件
// 调的是**同一份**），这个套件就是那一层的回归网。
//
// ## ⚠ 断言断的是**这一层自己的约定**，不是 OpenCV 的行为
//
// 像素那一部分，断的是**结构**而不是**像素数**：
// 「四边的中点是不是红的」「中心是不是红的」「远处的像素有没有被动过」。
// ⇒ 门槛**不随 OpenCV 版本漂** ——
// 断像素数的话，那只数字就变成「只有这台机器成立」的断言
// （那正是本仓库在 CI 第一次真跑时栽过的坑）。
//
// ## ⚠ 这个套件**不**依赖 tesseract
//
// common/qr_ocr 的套件里有一部分断言**需要** tesseract，
// 没装就整段**跳过** ⇒ 断言总数会**随机器变**
// （本机没装 tesseract 时，那一套少报若干条）。
// 而断言总数是**被文档门禁引用**的数 ⇒
// 一个随机器变的数，会在没装 tesseract 的机器上把门禁弄红。
// ⇒ 所以这一套**一条 tesseract 依赖的断言都没有**。
//
// ## 这个套件**不**做什么
//
// 不写任何文件、不开任何窗口 —— 产物一个字节都不落地，
// 所以 CI 的「源码树必须干净」那一步对它是空转。
//
// ============================================================

#include "present_pipeline.h"

#include "qr_ocr.h"     // common/qr_ocr —— 共享核心(解码)
#include "utf8_path.h"  // 路径 <-> UTF-8 文本(路径不能是窄串)

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
// 极简断言（与 W1 / W2 / W3 / W4 / W6 / W7 各套件同一套风格）
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static std::string g_case;

// 打印小数用**固定位数**，不截断。
//
// ⚠ W4 那轮栽过：屏幕上是 0.0309、文档里是 0.030943 ——
// 同一个测量，两个数 ⇒ 这套件一律 fmt(v, 6)。
static std::string fmt(double v, int digits = 6) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
}

static void beginCase(const std::string& name) {
    g_case = name;
    std::cout << "\n=== " << name << " ===" << std::endl;
}

static void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::cout << "  [PASS] " << what << std::endl;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << what << "  (case:" << g_case << ")" << std::endl;
    }
}

// ============================================================
// 找仓库根
//
// ⚠ 判据是 **common/ 存在**，不是「工程目录名存在」。
//   CI 的构建目录 build/ci/ 下面正好摆着一份 W5_OCR_QRCode
//   和一份 W6_PCB_DefectProject —— 只看工程目录名的话，
//   循环在那一层就停了，**把构建目录当成了仓库根**。
//   common/ 只有仓库根有，构建树里不可能有同名子目录。
//
// 这条判据是 common/qr_ocr 的套件里**已经验过**的那一条
// （common/qr_ocr/tests/test_qr_ocr.cpp 的 findRepoRoot）。
// 这里照抄，不另写一份 —— 判据多一份就多一处会漂的地方。
// ============================================================
static fs::path findRepoRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "common" / "qr_ocr" / "qr_ocr.h") &&
            fs::exists(p / "W5_OCR_QRCode") &&
            fs::exists(p / "W6_PCB_DefectProject")) {
            return p;
        }
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return fs::current_path();
}

static fs::path g_root;

// ============================================================
// 像素小工具
//
// 颜色是 BGR:红 = (0,0,255),绿 = (0,255,0)。
// ============================================================
static bool isRed(const cv::Vec3b& p) {
    return p[2] > 180 && p[1] < 70 && p[0] < 70;
}

static bool isGreen(const cv::Vec3b& p) {
    return p[1] > 180 && p[2] < 70 && p[0] < 70;
}

// (x,y) 附近 radius 个像素内，有没有红的 / 绿的
static bool hasRedNear(const cv::Mat& m, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= m.cols || py >= m.rows) continue;
            if (isRed(m.at<cv::Vec3b>(py, px))) return true;
        }
    }
    return false;
}

static bool hasGreenNear(const cv::Mat& m, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= m.cols || py >= m.rows) continue;
            if (isGreen(m.at<cv::Vec3b>(py, px))) return true;
        }
    }
    return false;
}

static int countRed(const cv::Mat& m) {
    int n = 0;
    for (int y = 0; y < m.rows; ++y) {
        for (int x = 0; x < m.cols; ++x) {
            if (isRed(m.at<cv::Vec3b>(y, x))) ++n;
        }
    }
    return n;
}

// 一块矩形里的红/绿像素个数
//
// ⚠ 为什么要有「数一块区域」而不是「附近有没有」:
//   问「附近有没有红的」会被**别的东西**满足 ——
//   比如「OCR: 那几个字是不是绿的」,框自己就在附近,
//   那条断言等于被框满足,什么也没证明。
//   ⇒ 只能问「**框里面那块**区域里有没有」。
static int countRedInRect(const cv::Mat& m, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(m.rows, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(m.cols, x1); ++x) {
            if (isRed(m.at<cv::Vec3b>(y, x))) ++n;
        }
    }
    return n;
}

static int countGreenInRect(const cv::Mat& m, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(m.rows, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(m.cols, x1); ++x) {
            if (isGreen(m.at<cv::Vec3b>(y, x))) ++n;
        }
    }
    return n;
}

// 某一行里 x ∈ [x0,x1) 的红像素**在哪些列** —— "158,159,160,161,162"
//
// 失败消息里带着这个,别人一看就知道**量到的是哪几个像素**,
// 不用再回头加打印重跑一遍。
static std::string redXsInRow(const cv::Mat& m, int y, int x0, int x1) {
    std::string s;
    for (int x = std::max(0, x0); x < std::min(m.cols, x1); ++x) {
        if (isRed(m.at<cv::Vec3b>(y, x))) {
            if (!s.empty()) s += ",";
            s += std::to_string(x);
        }
    }
    return s.empty() ? std::string("(没有)") : s;
}

// 两个图在 (x,y) 处一不一样
static bool samePixel(const cv::Mat& a, const cv::Mat& b, int x, int y) {
    if (x < 0 || y < 0 || x >= a.cols || y >= a.rows) return true;
    if (x >= b.cols || y >= b.rows) return true;
    const cv::Vec3b pa = a.at<cv::Vec3b>(y, x);
    const cv::Vec3b pb = b.at<cv::Vec3b>(y, x);
    return pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2];
}

// ============================================================
// 用例 1:串号的形状
//
// ⚠ 这一层是 **W5 自己的规则**，不是 OpenCV 的行为。
//   门槛全部是**精确**的 —— 因为「什么算流水号」是**定义**，
//   不是实现的巧合。
// ============================================================
static void test_serial_shape() {
    beginCase("串号:形状对不对");

    const std::string good = "PCB-2026-001-SN:A12345X";

    // --- 正常的 ---
    {
        const w5::Serial s = w5::findSerial(good);
        check(s.found, "认得出流水号");
        check(s.text == good, "认出来的那一段原样(实际 \"" + s.text + "\")");
    }

    // --- 流水号是 2 位(不是 3 位)---
    //
    // ⚠ 这一个就是 CI 第 4 步那道门禁的 C 变异用的那张图
    //   (tools/fixtures/qr_wrongshape.png)里的内容。
    {
        const w5::Serial s = w5::findSerial("PCB-2026-01-SN:A12345X");
        check(!s.found, "流水号只有 2 位 -> 不认(2 digits,实际认成了 \"" + s.text + "\")");
    }

    // --- 别的形状不对的 ---
    {
        check(!w5::findSerial("PCB-2026-001-SN:").found, "SN: 后面是空的 -> 不认");
        check(!w5::findSerial("PCB-2026-001-XX:A12345X").found, "SN: 写成了 XX -> 不认");
        check(!w5::findSerial("").found, "空字符串 -> 不认");
        check(!w5::findSerial("PCB-2026-001-SN-A12345X").found, "用的是 - 不是 : -> 不认");
    }

    // --- 不区分大小写(和 CI 那道门禁一致)---
    {
        const w5::Serial s = w5::findSerial("pcb-2026-001-sn:a12345x");
        check(s.found, "小写的也认(icase,和 CI 那边的 -match 一致)");
        check(s.text == "pcb-2026-001-sn:a12345x",
              "认出来的那一段**原样**,不改成大写(实际 \"" + s.text + "\")");
    }

    // --- 是「含有」,不是「整串就是」---
    {
        const std::string payload = "junk PCB-2026-001-SN:A12345X junk";
        const w5::Serial s = w5::findSerial(payload);
        check(s.found, "夹在别的字里也认(搜索,不是整串匹配)");
        check(s.text == "PCB-2026-001-SN:A12345X",
              "认出来的是**那一段**,不是整串(实际 \"" + s.text + "\")");
    }

    // --- 没认出来时,必须说清楚为什么 ---
    {
        const w5::Serial s = w5::findSerial("not a serial at all");
        check(!s.found, "认不出来");
        check(!s.reason.empty(), "认不出来时说清楚了为什么");
    }
}

// ============================================================
// 用例 2:形状的那份定义,**只有一处**
//
// 「PCB 序列号长什么样」这件事在仓库里有**两个地方**:
//   · present_pipeline.h 里的 kSerialPattern(产品这一边)
//   · tools/check-ci-steps.ps1 第 4 步里的那条正则(CI 门禁那一边)
//
// 这两个是**两个证人**,不是一份定义的拷贝 ——
// 但它们必须认**同一个形状** ⇒ 字符必须**逐字一样**。
//
// ⚠ 这里**不去**问程序「你觉得对不对」——
//   那是「尺子给自己打分」。这一条比的是**两份文字**。
// ============================================================
static void test_pattern_is_defined_once() {
    beginCase("串号的形状:两份定义逐字一样");

    const fs::path gate = g_root / "tools" / "check-ci-steps.ps1";
    check(fs::exists(gate),
          "找得到 CI 那道门禁的脚本(" + utf8path::text(gate) + ")");
    if (!fs::exists(gate)) return;

    std::ifstream in(gate, std::ios::binary);
    if (!in) {
        check(false, "门禁脚本打不开");
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // 从 "-match '...'" 里把那条正则**取出来** —— 不是拿我们的 pattern 去搜它,
    // 那样等于「拿答案去找答案」。
    const std::string marker = "-match '";
    const size_t at = text.find(marker);
    check(at != std::string::npos, "门禁脚本里找得到 -match '...'");
    if (at == std::string::npos) return;

    const size_t b = at + marker.size();
    const size_t e = text.find('\'', b);
    check(e != std::string::npos, "那条正则有结尾的引号");
    if (e == std::string::npos) return;

    const std::string gatePattern = text.substr(b, e - b);
    check(gatePattern == w5::kSerialPattern,
          "两边认的是同一个形状(门禁那边 \"" + gatePattern +
              "\",这边 \"" + std::string(w5::kSerialPattern) + "\")");
}

// ============================================================
// 用例 3:画四边形
//
// 角点顺序是 **左上 → 右上 → 右下 → 左下**。
// 顺序错了画出来是**蝴蝶结**(对角连线)——
// 而蝴蝶结看上去也挺像回事 ⇒ 这一条不是「防手滑」。
// ============================================================
static void test_mark_qr() {
    beginCase("QR:四边形画对了,没画错成蝴蝶结");

    const int W = 200, H = 160;
    const cv::Mat img(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

    // 一个轴对齐的四边形,留出边距(角点上方要写字)。
    const std::vector<cv::Point2f> corners = {
        cv::Point2f(40, 60), cv::Point2f(160, 60),
        cv::Point2f(160, 130), cv::Point2f(40, 130)};

    const cv::Mat marked = w5::markQr(img, corners, "SN-TEST");

    check(marked.size() == img.size(), "标注后的图和原图一样大");
    check(marked.type() == img.type(), "标注后的图和原图同类型");

    // --- 四条边的中点都得是红的 ---
    //
    // ⚠ 上下两条边看的是 **y**,左右两条边看的是 **x** ——
    //   混着写过一次(上边的 y 写成了 100,那是中心的 y),
    //   断言当场就红了 ⇒ 这不是「门槛太松」,是**写错了被抓住**。
    const int mx = 100, my = 95;    // 四边形的中心 (x, y)
    const int topY = 60, botY = 130;// 上边 / 下边的 y
    const int leftX = 40, rightX = 160;// 左边 / 右边的 x
    check(hasRedNear(marked, mx, topY, 3), "上边中点是红的");
    check(hasRedNear(marked, mx, botY, 3), "下边中点是红的");
    check(hasRedNear(marked, leftX, my, 3), "左边中点是红的");
    check(hasRedNear(marked, rightX, my, 3), "右边中点是红的");

    // --- 中心**不能**是红的 ---
    //
    // ⚠ 这条是「蝴蝶结」那条错的照妖镜:
    //   连错顺序的话,两条对角线正好从中心穿过去。
    check(!hasRedNear(marked, mx, my, 3), "中心不是红的(连的是四边,no diagonals)");

    // --- 角点上方写了字 ---
    check(hasRedNear(marked, 40, 50, 8), "串号写在左上角点的上方");

    // --- 线有多粗 ---
    //
    // ⚠ 只断「**不粗于**某条线」和「**不细于**某条线」,不断「正好 N 个像素」——
    //   正好 N 个是 OpenCV 的画法决定的,版本一换就可能差一个。
    //   这里断的是**区间**:细了(1 像素)抓得到,粗成一条带子也抓得到。
    //
    // ⚠ 量的是**右边那条竖线**,而且窗口**只有 1 行高** ——
    //   第一版量的是上边,窗口 (99..101, 50..70) 里**混进了串号文字**
    //   (文字底边正好在 y=50,x=99..100 落在 "SN-TEST" 的字身上)
    //   ⇒ 量出来 10 个像素,量的是「线 + 文字」,不是线。
    //   教训:量一个东西,窗口里**只能有那一个东西**。
    const int thick = countRedInRect(marked, 150, 95, 170, 96);
    check(thick >= 2 && thick <= 8,
          "右边那条线有 2..8 个像素厚(实际 " + std::to_string(thick) +
              " 个,在 x=" + redXsInRow(marked, 95, 150, 170) + ")");

    // --- 远处的像素没被动过 ---
    check(samePixel(img, marked, 5, 5), "左上角的原像素没被动过");
    check(samePixel(img, marked, W - 5, H - 5), "右下角的原像素没被动过");

    // --- 角点**不是 4 个**:什么都不画 ---
    {
        const std::vector<cv::Point2f> three = {corners[0], corners[1], corners[2]};
        const cv::Mat m2 = w5::markQr(img, three, "SN-TEST");
        check(countRed(m2) == 0, "角点只有 3 个 -> 一个红像素都不画(实际 " +
                                      std::to_string(countRed(m2)) + " 个)");
        check(samePixel(img, m2, 5, 5), "角点只有 3 个 -> 原图没被动过");
    }
    {
        const std::vector<cv::Point2f> none;
        const cv::Mat m2 = w5::markQr(img, none, "SN-TEST");
        check(countRed(m2) == 0, "没有角点 -> 一个红像素都不画");
    }

    // --- 空图不崩 ---
    {
        const cv::Mat empty;
        const cv::Mat m2 = w5::markQr(empty, corners, "SN-TEST");
        check(m2.empty(), "空图 -> 还是空图,不崩");
    }

    // --- 角点贴到图的上边:字被裁掉,但不崩 ---
    {
        const std::vector<cv::Point2f> top = {
            cv::Point2f(40, 2), cv::Point2f(160, 2),
            cv::Point2f(160, 130), cv::Point2f(40, 130)};
        const cv::Mat m2 = w5::markQr(img, top, "SN-TEST");
        check(m2.size() == img.size(), "角点贴着上边 -> 不崩,图还是那么大");
        check(hasRedNear(m2, 100, 2, 3), "角点贴着上边 -> 边还是画上了");
    }
}

// ============================================================
// 用例 4:画 OCR 的框
// ============================================================
static void test_mark_ocr() {
    beginCase("OCR:框画在边上,里面的原像素没被动过");

    const int W = 200, H = 160;
    const cv::Mat img(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

    const cv::Mat marked = w5::markOcr(img);
    check(marked.size() == img.size(), "标注后的图和原图一样大");

    check(hasGreenNear(marked, 100, 1, 3), "上边是绿的");
    check(hasGreenNear(marked, 100, H - 2, 3), "下边是绿的(OCR frame)");
    check(hasGreenNear(marked, 1, 80, 3), "左边是绿的");
    check(hasGreenNear(marked, W - 2, 80, 3), "右边是绿的");

    // 框线的粗细(区间,不是「正好 N 个」—— 见 QR 那条的解释)
    //
    // ⚠ 窗口**只有 1 列宽**,量出来的数**就是**线的厚度;
    //   窗口 2 列宽的话,数会翻倍 —— 门槛就得跟着那个倍数走,
    //   门槛和窗口必须对得上,不然量的是别的东西。
    const int frameThick = countGreenInRect(marked, 99, 0, 100, 8);
    check(frameThick >= 2 && frameThick <= 6,
          "框线有 2..6 个像素厚(实际 " + std::to_string(frameThick) + " 个)");

    // "OCR:" 写在 (10,30) 附近 —— 那是**框里面**,
    // 所以不能拿「框里面没动过」来断言它;
    // 也不能只问「附近有没有绿的」—— **框自己就在附近**,
    // 那条断言会被框满足,什么也没证明。
    // ⇒ 只数**框里面**那一块区域的绿像素。
    const int textGreen = countGreenInRect(marked, 5, 5, 140, 45);
    check(textGreen >= 50,
          "\"OCR:\" 那几个字是绿的(框里面那块区域 " +
              std::to_string(textGreen) + " 个绿像素,门槛 50)");

    // 框**里面**远处的像素没被动过
    check(samePixel(img, marked, 150, 140), "框里面远处的原像素没被动过");

    // --- 空图不崩 ---
    {
        const cv::Mat empty;
        const cv::Mat m2 = w5::markOcr(empty);
        check(m2.empty(), "空图 -> 还是空图,不崩");
    }
}

// ============================================================
// 用例 5:OCR 文字的分词
// ============================================================
static void test_split_tokens() {
    beginCase("OCR 文字:一行一个,空的那几个丢掉");

    {
        const std::vector<std::string> t = w5::splitTokens("R123 100R C456 10uF");
        check(t.size() == 4, "四个词(实际 " + std::to_string(t.size()) + " 个)");
        check(t.size() == 4 && t[0] == "R123", "第一个是 R123");
        check(t.size() == 4 && t[3] == "10uF", "最后一个是 10uF");
    }
    {
        const std::vector<std::string> t = w5::splitTokens("a  b");
        check(t.size() == 2, "中间两个空格 -> 还是两个词(实际 " +
                                 std::to_string(t.size()) + " 个)");
    }
    {
        check(w5::splitTokens("").empty(), "空字符串 -> 没有词");
        check(w5::splitTokens("   ").empty(), "只有空格 -> 没有词");
    }
    {
        const std::vector<std::string> t = w5::splitTokens("a");
        check(t.size() == 1 && t[0] == "a", "一个词就是它自己");
    }
    {
        // 换行**不**分 —— 这是 demo 原来的行为,照搬过来的。
        const std::vector<std::string> t = w5::splitTokens("a\nb c");
        check(t.size() == 2, "换行不当分隔符(实际 " + std::to_string(t.size()) + " 个)");
        check(t.size() == 2 && t[0] == "a\nb", "带换行的那一段原样保留");
    }
}

// ============================================================
// 用例 6:报告行
//
// ⚠ CI 第 4 步那道门禁是在**整个输出**里搜流水号的,
//   所以「串号原样出现在这一行里」是**被依赖的约定**。
// ============================================================
static void test_report_line() {
    beginCase("报告行:串号原样在里面(CI 门禁靠这个)");

    const std::string payload = "PCB-2026-001-SN:A12345X";
    const std::string line = w5::qrDecodedLine(payload);
    check(line.find(payload) != std::string::npos,
          "那一行里有串号(CI gate 靠这个,实际 \"" + line + "\")");
    check(line.find("PCB-2026-001-SN:A12345X") != std::string::npos,
          "门禁那条正则能在这行里搜到");
}

// ============================================================
// 用例 7:端到端 —— 真图、真解码、真规则
//
// 这一条把三样东西串起来:
//   仓库里**真**的那张 QR 图 → 共享核心**真**的解码 → W5 **自己**的规则。
// ============================================================
static void test_end_to_end() {
    beginCase("端到端:真图解出来的号,W5 自己的规则认不认");

    const fs::path img = g_root / "W5_OCR_QRCode" / "images" / "qrcode_test.png";
    if (!fs::exists(img)) {
        check(false, "找得到真图(" + utf8path::text(img) + ")");
        return;
    }
    const cv::Mat m = cv::imread(utf8path::text(img), cv::IMREAD_COLOR);
    check(!m.empty(), "读得到真图");
    if (m.empty()) return;

    const qroc::QrResult qr = qroc::readQr(m);
    check(qr.found, "真图上找得到 QR");
    if (!qr.found) return;

    // W5 自己的规则,认不认解出来的这一串
    const w5::Serial s = w5::findSerial(qr.text);
    check(s.found, "W5 的规则认这个号(实际 \"" + qr.text + "\")");

    // 而 CI 门禁用的那张**形状不对**的图,两边都该不认
    //
    // ⚠ 这张图是 CI 第 4 步的 C 变异用的那张
    //   (tools/fixtures/qr_wrongshape.png)——
    //   它的内容是「流水号只有 2 位」那一种。
    //   门禁那边**已经**用它证明过自己会红;
    //   这一条证明**产品这一边**也会不认 —— 两边认的是同一件事。
    {
        const fs::path bad = g_root / "tools" / "fixtures" / "qr_wrongshape.png";
        if (!fs::exists(bad)) {
            check(false, "找得到形状不对的那张图(" + utf8path::text(bad) + ")");
        } else {
            const cv::Mat bm = cv::imread(utf8path::text(bad), cv::IMREAD_COLOR);
            check(!bm.empty(), "读得到形状不对的那张图");
            if (!bm.empty()) {
                const qroc::QrResult bqr = qroc::readQr(bm);
                const w5::Serial bs = w5::findSerial(bqr.text);
                check(!bs.found, "形状不对的那张 -> W5 的规则**不认**(实际 \"" +
                                    bqr.text + "\")");
            }
        }
    }
}

int main() {
    g_root = findRepoRoot();

    std::cout << "=== W5 (QR + OCR) 回归测试 ===" << std::endl;
    std::cout << "仓库根: " << utf8path::text(g_root) << std::endl;

    test_serial_shape();
    test_pattern_is_defined_once();
    test_mark_qr();
    test_mark_ocr();
    test_split_tokens();
    test_report_line();
    test_end_to_end();

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: pass=" << g_pass << " fail=" << g_fail
              << "   RESULT=" << (g_fail == 0 ? "PASS" : "FAIL") << std::endl;
    return g_fail == 0 ? 0 : 1;
}
