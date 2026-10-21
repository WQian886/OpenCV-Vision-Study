#pragma once
// ============================================================
// operator_defaults.h — W7 全部算子旋钮默认值的**唯一一份**
// ============================================================
//
// ------------------------------------------------------------
// 为什么要有这个文件
// ------------------------------------------------------------
// 每个算子的默认值以前抄在**三个地方**:
//
//   1. defaultParams()              —— 写进流水线 JSON 的那份
//   2. run() 里的 toDouble(0.9)     —— 读不到这个 key 时的兜底
//   3. 参数面板 buildUI() 的 setValue(0.9) —— 界面上初值那份
//
// 三份都得改,漏一份就出两种病:
//
//   · 漏改 (2):参数 JSON 缺这个 key 时,算子按**另一个数**在跑,
//     而面板上写的是第三个数 —— 用户调的值和算子用的值不是一回事。
//   · 漏改 (3):面板上写着 A,实际跑的是 B ——
//     界面上摆一个"调了不生效"的旋钮 = 骗人。
//
// 这不是假想敌。2026-10-21 用脚本扫过全部 16 个算子,
// **三处副本已经漂了**:
//
//   blob_count.min_area       声明 20   兜底 50    ← 漂了
//   count_check.expected_min  声明 1    兜底 0     ← 漂了
//   count_check.expected_max  声明 100  兜底 999   ← 漂了
//
// 还有两处更隐蔽的:
//
//   pcb_inspect.min_area      声明 80,但 run() 读的是 min_area_pct ——
//                             而 min_area_pct **压根没在 defaultParams() 里登记**,
//                             于是这个旋钮的默认值只活在 run() 的兜底和面板里。
//   dl_infer.class_names      声明了 COCO 类名表,run() 里**没有兜底** ——
//                             缺 key 时类名列表是空的,检测框上标不出类别。
//
// 所以本文件不是"重构洁癖",是那几处漂移的收口。
//
// ------------------------------------------------------------
// 怎么用
// ------------------------------------------------------------
//   defaultParams(): p["min_circularity"] = CircleFitDefaults::kMinCircularity;
//   run():          params.value("min_circularity").toDouble(CircleFitDefaults::kMinCircularity)
//   面板 buildUI(): m_minCircularity->setValue(CircleFitDefaults::kMinCircularity);
//
// 三个地方引用同一个常量 —— 不是"记得一起改",是**没有第二份可改**。
//
// 这条不变式有断言盯着:tests/test_operator_knobs.cpp 的
// "默认值必须来自唯一一份"那条用例。
//
// ------------------------------------------------------------
// 边界:这份清单只管**算子旋钮的默认值**
// ------------------------------------------------------------
// 判定阈值(pcb_defect_judge 的 16 个旋钮)不走这里 ——
// 它们走 common/pcb_core/pcb_core.h 的 pcbcore::JudgeParams,
// 因为 W6 命令行和 W7 算子要共用同一份判据。
// 两份清单职责不重叠,别混。
//
// 金层 HSV 区间也不走这里 —— 它走 common/pcb_core/pcb_color_ref.h
// (pcbcolor::goldBand() / pcbcolor::kMaskVMul),因为 W6 的共享核心也要用它。
//
// ⚠ 路径别写错:这个文件**不在** operators/ 底下,在 common/pcb_core/。
//   注释里写错路径,下一个人会以为 operators/ 下有一份,
//   然后在 operators/ 里"再抄一份" —— 这就是本项目栽过五遍的坑。
//
//   注意:面板 buildUI() 的初值也必须取自 goldBand(),
//   不能自己写 setValue(85) —— 那又是一份手抄副本。
//   旋钮审计的用例 3e 盯着这件事.
//
// ------------------------------------------------------------
// 为什么这个文件要 include OpenCV
// ------------------------------------------------------------
//
// template_match 的"匹配方法"默认值是 OpenCV 的枚举
// cv::TM_CCOEFF_NORMED。写死 5 的话没人知道 5 是什么,
// 写 cv::TM_CCOEFF_NORMED 才是能读懂的 —— 所以这里 include
// opencv2/imgproc.hpp,让那个枚举在本文件里有名字。
//
// ⚠ 这个 include 只为了这一个常量。别把 OpenCV 的东西
//   往这个文件里搬 —— 它是"旋钮默认值清单",不是通用头文件。
// ============================================================

#include <opencv2/imgproc.hpp>

namespace w7 {

// ============================================================
// color_segment — HSV 分割
//
// ⚠ 这六个数以前抄在两处:defaultParams() 里的两个 QJsonArray,
//   和面板 buildUI() 里六个 setValue()。改一处忘一处,分割区间
//   就和面板上显示的对不上。
// ============================================================
struct ColorSegmentDefaults {
    static constexpr int kHsvLowH  = 10;
    static constexpr int kHsvLowS  = 80;
    static constexpr int kHsvLowV  = 80;
    static constexpr int kHsvHighH = 40;
    static constexpr int kHsvHighS = 255;
    static constexpr int kHsvHighV = 255;
};

// ============================================================
// blob_count — 连通域计数
//
// ⚠ 兜底曾经是 50,和这里声明的 20 不是一回事(已收口)。
// ============================================================
struct BlobCountDefaults {
    static constexpr int kMinArea = 20;
};

// ============================================================
// count_check — 数量判定
//
// ⚠ expected_min 的兜底曾是 0、expected_max 的兜底曾是 999 ——
//   两个都和声明值不一致(已收口)。
// ============================================================
struct CountCheckDefaults {
    static constexpr int kExpectedMin = 1;
    static constexpr int kExpectedMax = 100;
};

// ============================================================
// template_match — 模板匹配
//
// ⚠ kMethod 以前直接写 cv::TM_CCOEFF_NORMED,在**三处**各写一遍:
//   defaultParams() 的声明、run() 的兜底、setParams() 的兜底。
//   看着是同一个枚举,但改默认值要改三处,漏一处就变成
//   "面板上摆的方法"和"算子跑的方法"不是一回事 ——
//   那正是本项目栽过五遍的那个坑的第六次化身。
//
// ⚠ 同一个枚举在 template_match.cpp 里还会出现:
//   run() 的**合法范围**检查(method < TM_SQDIFF || method > TM_CCOEFF_NORMED)。
//   那是**范围边界**,不是默认值 —— 两者不是一回事,不要混:
//   把默认方法改成 TM_SQDIFF 不会改掉合法范围。
// ============================================================
struct TemplateMatchDefaults {
    static constexpr double      kThreshold   = 0.9;
    static constexpr const char* kTemplatePath = "templates/R.png";
    static constexpr int         kMethod      = cv::TM_CCOEFF_NORMED;
};

// ============================================================
// image_source_dir — 文件夹图像源
// ============================================================
struct ImageSourceDirDefaults {
    static constexpr const char* kDirPath = "images";
    static constexpr const char* kPattern = "*.png";
};

// ============================================================
// image_source_video — 视频图像源
// ============================================================
struct ImageSourceVideoDefaults {
    static constexpr const char* kVideoPath = "images/sample.avi";
    static constexpr int kMaxFrames = 30;   // 视频可能有几千帧,全跑一遍很久
    static constexpr int kFrameStep = 1;    // 隔几帧取一帧
};

// ============================================================
// image_source_camera — 工业相机图像源
//
// ⚠ 本算子的旋钮只在装了海康 MVS SDK 时才真的生效(编译期决定),
//   运行时缺失时面板会置灰并写明原因 —— 理由见 dl_infer.h。
// ============================================================
struct ImageSourceCameraDefaults {
    static constexpr const char* kCameraIp  = "192.168.1.10";
    static constexpr double      kExposureMs = 10.0;   // ms
    static constexpr int         kTriggerMode = 0;     // 0=连续, 1=软触发
};

// ============================================================
// dl_infer — ONNX 推理
//
// ⚠ 同样只在装了 ONNX Runtime 时生效(编译期决定)。
// ============================================================
struct DlInferDefaults {
    static constexpr const char* kModelPath  = "models/yolov8n.onnx";
    static constexpr int    kInputW        = 640;
    static constexpr int    kInputH        = 640;
    static constexpr double kConfThreshold = 0.25;
    static constexpr double kNmsIou        = 0.45;
    static constexpr const char* kClassNames =
        "person,bicycle,car,motorcycle,airplane,bus";
    static constexpr const char* kOutputLayout = "yolov8";  // yolov5=(1,N,5+nc)
};

// ============================================================
// line_fit — 直线测量
// ============================================================
struct LineFitDefaults {
    static constexpr int kX1 = 100, kY1 = 100;
    static constexpr int kX2 = 300, kY2 = 200;
};

// ============================================================
// circle_fit — 圆形测量
//
// ⚠ min_circularity:1.0 是完美圆,越小越"像圆"。
//   0.7 是经验值 —— 低于这个值的多边形/方块会被滤掉。
// ============================================================
struct CircleFitDefaults {
    static constexpr int    kMinArea        = 100;
    static constexpr double kMinCircularity = 0.70;
};

// ============================================================
// caliper_dist — 卡尺距离
// ============================================================
struct CaliperDistDefaults {
    static constexpr int kAx = 100, kAy = 100;   // 基准线起点
    static constexpr int kBx = 300, kBy = 100;   // 基准线终点
    static constexpr int kCx = 200, kCy = 160;   // 被测点
};

// ============================================================
// point_point_dist — 两点距离
// ============================================================
struct PointPointDistDefaults {
    static constexpr int kX1 = 100, kY1 = 100;
    static constexpr int kX2 = 200, kY2 = 200;
};

// ============================================================
// point_line_dist — 点到线距离
// ============================================================
struct PointLineDistDefaults {
    static constexpr int kAx = 0,   kAy = 0;     // 基准线起点
    static constexpr int kBx = 100, kBy = 0;     // 基准线终点
    static constexpr int kCx = 50,  kCy = 30;    // 被测点
};

// ============================================================
// angle — 两线夹角
// ============================================================
struct AngleDefaults {
    static constexpr int kX1 = 0,   kY1 = 100;   // 线 1 起点
    static constexpr int kX2 = 200, kY2 = 100;   // 线 1 终点(水平)
    static constexpr int kX3 = 100, kY3 = 0;     // 线 2 起点
    static constexpr int kX4 = 100, kY4 = 200;   // 线 2 终点(垂直)
};

// ============================================================
// pcb_inspect — PCB 元件定位
//
// ⚠ 这里的 min_area_pct 是**唯一**的最小连通域旋钮。
//   以前 defaultParams() 里登记的是 min_area = 80(绝对像素),
//   而 run() 读的是 min_area_pct(占全图 %)—— 登记的和读的不是同一个 key,
//   于是 min_area 成了**死旋钮**,min_area_pct 又**从没登记过**。
//   死旋钮已经删掉(界面上摆一个调了不生效的旋钮 = 骗人)。
//
//   min_area_pct 这个名字不是随便起的:它是从"绝对像素"改成
//   "占全图面积比例"时一起换的,理由见 pcb_inspect.cpp 里的注释。
// ============================================================
struct PcbInspectDefaults {
    static constexpr const char* kMode       = "known_site";
    static constexpr double      kMinAreaPct = 0.05;   // 占全图面积 %,见上面
    static constexpr const char* kRecipePath = "recipes/demo.json";
};

} // namespace w7
