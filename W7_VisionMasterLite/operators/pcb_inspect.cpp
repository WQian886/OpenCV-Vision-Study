#include "pcb_inspect.h"
#include "hsv_params.h"
#include "pcb_color_ref.h"
#include "operator_defaults.h"
#include "board_recipe.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFileInfo>
#include <QDir>
#include "artifact_path.h"
#include <QJsonArray>
#include <QStringList>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>

// 共享判定核心不带 Qt。`using namespace pcbcore;` 之后,
// 原来写的 `pcbcolor::estimateBoardBase(...)` 照常解析 ——
// 那边也是 pcbcore::pcbcolor。
using namespace pcbcore;

namespace w7 {

// ============================================================
// 板型真值 —— 现在从 recipes/*.json 读,不再写在源码里
//
// === 这里原来是什么,以及为什么拿掉 ===
//
// 原来是一张 SITES[6] 硬编码表 + 判定算子里七处 `if (label == "R2")`。
// **同一份板型真值抄了两遍,分居两个文件。**
//
// 实测代价(2026-10-09,把 W6 遗留的三种板型直接喂进来):
//   - 元件数恒等于 6,与图上实际有什么无关
//   - 三块**好板**全被判废(误报 3/3)
//   - 两块废板的输出与好板**逐字相同**(signal 板 0/2 有信号)
//
// 根因不是坐标写死 —— 坐标归一化过了,换分辨率是好的。
// 根因是**规则也写死**:换一块布局不同的板,坐标不对,
// 那七处 label== 更是每一条都匹配不上,一条规则都不跑。
//
// 现在坐标和规则都在同一份 JSON 里(recipes/*.json),
// 由 tools/export_recipes.py 从 W6 生成器导出。
// 换板子 = 换一份 JSON,**不改一行代码**。
//
// ⚠ JSON 是产物不是源。要改配方去改 W6 生成器再重跑导出脚本。
// ============================================================
namespace {
// 默认板型:自带的那张 demo 板。
const QString kDefaultRecipe = QString::fromUtf8(PcbInspectDefaults::kRecipePath);

// auto_contour 模式不读配方,聚类半径就没有 refW/refH 和本体尺寸可依,
// 只能退回改造前写死的真值。**这三个数是原值,不是新选的**:
// kRefW/kRefH 和 SITES[6] 里 U1 的 180x60。改动它们会直接改变
// test_auto_contour_scale_stable 钉死的 7/10/10/11。
constexpr int kFallbackRefW = 1000;
constexpr int kFallbackRefH =  700;
} // namespace

QJsonObject PcbInspect::defaultParams() const {
    QJsonObject p;
    p["mode"]     = QString::fromUtf8(PcbInspectDefaults::kMode);
    // === 金色范围沿用 W6 校准值,不要想当然改成 (10,80,80) ===
    // W6 笔记记录的坑:合成器的 GOLD 实际落在 H 85-115,
    // 用 W2 笔记里的常规金色范围会抓到 IC 的深蓝本体而不是金层。
    //
    // ⚠ V 下界不再绝对。原来的 hsv_low=[85,80,80] 里那个 80 是**绝对 V**,
    //   压暗到亮度 0.4 倍时金层像素的 V 掉到 78,被这道线整个切掉 ——
    //   实测 R1 ROI 的 gold_px 从 168 掉到 144(相对式在 0.4~1.6 全档
    //   稳在 168)。所以 V 下界改成"板面基色 x mask_v_mul"。
    //
    //   参数名跟着换,理由和 dark_thresh -> dark_min_pct 一样:
    //   **沿用旧名字的话,老存档里的 80 会被当成 80 倍板面色**,
    //   判据永远不成立,金层全空。读不到新名字就退回默认,宁可用默认
    //   也不能让人以为是自己配的数在起作用。
    p["hsv_h_low"]  = pcbcolor::goldBand().hLo;
    p["hsv_h_high"] = pcbcolor::goldBand().hHi;
    p["hsv_s_low"]  = pcbcolor::goldBand().sLo;
    p["mask_v_mul"] = pcbcolor::kMaskVMul;
    // 最小连通域:占全图面积 %,不是绝对像素(理由见 run() 里那段说明)。
    //
    // ⚠ 这里原来登记的是 min_area = 80(绝对像素),而 run() 读的是
    //   min_area_pct —— 登记的和读的不是同一个 key,于是:
    //     · min_area 成了**死旋钮**(面板上写着、调了不生效)
    //     · min_area_pct 又**从来没登记过**,它的默认值只活在
    //       run() 的兜底和面板里,写进流水线的 JSON 里压根没有
    //   两个毛病一起收:死 key 删掉,真正的旋钮补登记。
    p["min_area_pct"] = PcbInspectDefaults::kMinAreaPct;
    // 板型:换板子只改这一项(指向 recipes/ 下的另一份 JSON),
    // 不改任何代码。原来这里是隐含的 —— 只有一张写死的表。
    p["recipe"] = kDefaultRecipe;
    return p;
}

QJsonObject PcbInspect::run(const QJsonObject& params, const QJsonObject& inputs) {
    QJsonObject output;

    QString imgPath = inputs.value("image_path").toString();
    if (imgPath.isEmpty()) {
        output["error"] = "上游未提供 image_path(需要先接图像源算子)";
        return output;
    }
    cv::Mat img = cv::imread(imgPath.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        output["error"] = "imread failed: " + imgPath;
        return output;
    }

    // ============================================================
    // 板型配方在下面读(要等 mode 读出来才知道该不该读),
    // 这里先把类型声明好 —— 一张配方横跨整个 run(),
    // 后面 auto_contour 分支也用它的 ref_w/ref_h 算归一化基准。
    // ============================================================
    BoardRecipe recipe;

    // --- 金层提取 ---
    // 守卫见 operators/hsv_params.h。以前这里是裸的 lo[0].toInt(85),
    // QJsonArray::operator[] 不检查越界,hsv_low 传成空数组就 0xC0000005。
    //
    // 参数形状变了:原来是两个三元数组(绝对 H/S/V),现在是三个标量
    // + 一个倍数。**故意不去读旧的 hsv_low/hsv_high** —— 老存档里的
    // 80 会被当成 80 倍板面色,金层恒空,不如退回默认让人一眼看出来
    // 没生效。理由和 dark_thresh -> dark_min_pct 完全一样。
    double hLo = pcbcolor::goldBand().hLo;
    double hHi = pcbcolor::goldBand().hHi;
    double sLo = pcbcolor::goldBand().sLo;
    double vMul = pcbcolor::kMaskVMul;
    QString hsvErr;
    // ============================================================
    // 金层阈值是**必填旋钮**:缺了或形状不对,一律报错
    // ============================================================
    // 这里**不**补默认值 —— 和"可选旋钮"那套规则不一样,是故意的:
    //   · 可选旋钮(阈值、坐标、路径那些):缺了就退回
    //     operator_defaults.h 里声明的默认值 —— 那是用户没配,
    //     退回一个声明过的值是诚实的。
    //   · 必填旋钮(金层区间这两个):缺了就说明**没配置**,
    //     补一个默认值会让用户以为自己的配置生效了 —— 那正是
    //     hsv_params.h 开头警告的"配置看着对、结果全错"。
    //
    // 有测试钉着这件事:test_pcb_operators.cpp 的
    // "H 上界缺失 -> 报错 点名字段 hsv_h_high"。
    // ============================================================
    if (!readHsvScalar(params.value("hsv_h_low"),  "hsv_h_low",  &hLo,  &hsvErr) ||
        !readHsvScalar(params.value("hsv_h_high"), "hsv_h_high", &hHi,  &hsvErr) ||
        !readHsvScalar(params.value("hsv_s_low"),  "hsv_s_low",  &sLo,  &hsvErr) ||
        !readHsvScalar(params.value("mask_v_mul"),  "mask_v_mul", &vMul, &hsvErr)) {
        output["error"] = QString("金层阈值不合法: %1").arg(hsvErr);
        return output;
    }
    if (hLo < 0 || hHi > 179 || hLo >= hHi) {
        output["error"] = QString(
            "金层 H 区间不合法: [%1, %2]。H 的取值范围是 0~179(OpenCV 按度/2 存),"
            "而且下界必须小于上界。").arg(hLo, 0, 'f', 0).arg(hHi, 0, 'f', 0);
        return output;
    }
    if (sLo < 0 || sLo > 255) {
        output["error"] = QString(
            "金层 S 下界 %1 越界,取值范围是 0~255。").arg(sLo, 0, 'f', 0);
        return output;
    }
    if (vMul < 0.0 || vMul > 4.0) {
        output["error"] = QString(
            "金层 V 下界的倍数 %1 不合理,取值范围 0~4。"
            "V 下界 = 倍数 x 板面基色的 V(逐图估计),调太大金层会被切空、"
            "调成 0 则相当于不设下界。").arg(vMul, 0, 'f', 2);
        return output;
    }

    // 板面基色**逐图估计**。相对判据的右端也必须带 k 才叫相对 ——
    // 拿一张参考图的板色当常量写进配置,等式右边就没有 k 了。
    const cv::Vec3b boardBase = pcbcolor::estimateBoardBase(img);

    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    // 色带/掩膜/开运算都走 pcb_color_ref.h 的共用实现,
    // 与 pcb_defect_judge 同一份定义(实测这两处原本已经漂开了)。
    pcbcolor::HsvBand band{static_cast<int>(std::lround(hLo)),
                           static_cast<int>(std::lround(hHi)),
                           static_cast<int>(std::lround(sLo)),
                           vMul};
    cv::Mat goldMask = pcbcolor::opened(pcbcolor::bandMask(hsv, boardBase, band),
                                        pcbcolor::kernelScale(img));

    const QString mode = params.value("mode").toString(
                              QString::fromUtf8(PcbInspectDefaults::kMode));

    // ============================================================
    // 板型配方
    //
    // 真值模式才需要配方;auto_contour 不依赖它,所以放到 mode 分支里读。
    // 读失败**必须在这里停**:带着一份"凑合用"的站点表往下走,
    // 会输出一份看起来正常的元件清单,只是清单是错的 ——
    // 而下游判定无从知道这一点。
    // ============================================================
    if (mode != "auto_contour") {
        const QString recipePath =
            params.value("recipe").toString(kDefaultRecipe);
        QString reason;
        if (!loadRecipe(recipePath, &recipe, &reason)) {
            output["error"] = QString("板型配方加载失败:%1").arg(reason);
            return output;
        }
    }

    // min_area 从绝对像素改成占全图面积的比例。
    //
    // 实测(只改分辨率、不改亮度,auto_contour 元件数):
    //            0.5x  0.75x  1.0x  1.25x  1.5x  2.0x
    //   绝对 80px     6      9    14     18    18    18
    //   占全图 0.05%  6      7    10     10    10    10
    // 绝对值下同一块板在不同分辨率上被切成 6 个或 18 个元件。
    //
    // 顺带说明 mergeDist:原来写死 90.0f,同样是绝对像素,
    // 而且**它不是参数**,界面上根本调不到。图放大时同元件焊盘间距
    // 一起放大,90 这个阈值相对就变紧,聚类散开 —— 上表 0.5x 的 6 个
    // 就是过度聚类(全板糊成几团),2.0x 的 18 个是聚类不足。
    // 改成跟着元件尺寸走,量纲不再是像素。
    const double minAreaPct = params.value("min_area_pct").toDouble(PcbInspectDefaults::kMinAreaPct);
    const int minArea = static_cast<int>(
        std::lround(minAreaPct / 100.0 * img.cols * img.rows));
    // 聚类半径:取元件本体最大尺寸的一半。跟着图像尺寸缩放,
    // 和配方那边的归一化同一个道理。
    //
    // auto_contour 不依赖配方,那它按什么缩放?按基准图的分辨率
    // —— 没有配方可读时,用下面那两个兜底常数(改造前的原值)。
    const int refW = recipe.isEmpty() ? kFallbackRefW : recipe.refW;
    const int refH = recipe.isEmpty() ? kFallbackRefH : recipe.refH;
    int maxBodyW = 0, maxBodyH = 0;
    for (const auto& s : recipe.sites) {
        maxBodyW = std::max(maxBodyW, s.bodyW);
        maxBodyH = std::max(maxBodyH, s.bodyH);
    }
    // auto_contour 不读配方(见上面),所以这里拿不到站点尺寸,
    // 只能退回改造前那套写死的真值。这三个数就是原 SITES[6] / kRef* 的
    // 原值,**不是新挑的**:换成别的数,mergeDist 就不是当年那个 90.0,
    // test_auto_contour_scale_stable 的 6/10/10/10 会立刻变。
    //
    // ⚠ 这里踩过一次坑,记下来:第一版兜底写成 maxBodyW=100 且
    //   refW=img.cols,结果 mergeDist 恒等于 90 —— 半径不跟着图缩放了,
    //   0.5x 的小图被按大图的半径聚类,过度聚类,6 个元件糊成 5 个,
    //   正好复现该测试注释里记的 M16 失效模式。
    //   也就是说"参照分辨率"必须是**固定的基准图尺寸**,
    //   不能拿当前图自己的尺寸去归一化自己 —— 那样等于把尺度消掉。
    //
    // 改造前 0.5x 下 mergeDist = 0.5*180*0.5 = 45,1.0x = 90,2.0x = 180。
    // 真要让 auto_contour 跨分辨率稳定,得让它也读一份配方;那属于下一步。
    if (maxBodyW <= 0) maxBodyW = 180;
    if (maxBodyH <= 0) maxBodyH = 60;
    const double mergeDist = std::max(
        1.0, 0.5 * std::max(maxBodyW * img.cols / static_cast<double>(refW),
                            maxBodyH * img.rows / static_cast<double>(refH)));

    QJsonArray compsJson;
    cv::Mat marked = img.clone();

    if (mode == "auto_contour") {
        // --- 自动模式:从金层连通域发现元件 ---
        // 真实产线做法:不依赖板型真值,换板型也能用。
        // 代价:焊盘会被当成独立元件(一个 IC 有十几个焊盘),
        //      所以这里按"同一行/列的焊盘聚类"合并成元件框。
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(goldMask, contours, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);

        struct Blob { cv::Rect box; int area; cv::Point2f center; };
        std::vector<Blob> blobs;
        for (const auto& c : contours) {
            cv::Rect r = cv::boundingRect(c);
            int a = (int)cv::contourArea(c);
            if (a < minArea) continue;
            blobs.push_back({r, a, cv::Point2f((float)r.x + r.width / 2.0f,
                                               (float)r.y + r.height / 2.0f)});
        }

        // 聚类:中心距离 < 阈值的焊盘归为一个元件
        // 阈值取元件最大尺寸的一半(已按图像尺寸归一化,见上面 mergeDist 处)
        const float mergeDistF = static_cast<float>(mergeDist);
        std::vector<bool> used(blobs.size(), false);
        int idx = 0;
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (used[i]) continue;
            cv::Rect merged = blobs[i].box;
            used[i] = true;
            for (size_t j = i + 1; j < blobs.size(); ++j) {
                if (used[j]) continue;
                if (cv::norm(blobs[i].center - blobs[j].center) < mergeDistF) {
                    merged |= blobs[j].box;
                    used[j] = true;
                }
            }
            QJsonObject cj;
            cj["label"]  = QString("C%1").arg(++idx);   // 自动模式无真值标签
            cj["kind"]   = "AUTO";
            cj["x"]      = merged.x;
            cj["y"]      = merged.y;
            cj["w"]      = merged.width;
            cj["h"]      = merged.height;
            cj["area"]   = merged.width * merged.height;
            compsJson.append(cj);

            cv::rectangle(marked, merged, cv::Scalar(0, 200, 255), 2);
            cv::putText(marked, cj["label"].toString().toStdString(),
                        cv::Point(merged.x, std::max(12, merged.y - 4)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 200, 255), 1);
        }
    } else {
        // --- 真值模式:按配方构造 ROI(W6 已验证路径) ---
        //
        // 站点几何、body_pct、gold_pct 的算法**全部在共享核心里**
        // (common/pcb_core)—— W6 命令行也要用同一份。
        // 这里只负责把核心的结果搬回 QJson。
        //
        // ⚠ 2026-10-20:原来这段是本文件里自己的实现,和核心那份是
        //   同一套算法的两份拷贝。两边改一处忘一处,定位和判定就会
        //   对不上 —— 而两边都在动的时候,没人会怀疑它们本来应该一样。
        const pcbcore::InspectResult ir =
            pcbcore::inspect(img, toCoreRecipe(recipe), band);
        // 核心算出来的金层掩膜 / 板面基色 / 标注图,和本文件自己算的
        // 是同一份定义(共用 pcb_color_ref.h)。以核心那份为准。
        //
        // boardBase 不赋值:上面那个 const 变量就是
        // pcbcolor::estimateBoardBase(img),而核心算的是**同一个函数、
        // 同一张图**,值必然相同 —— 赋一次反而是"两处各自算一遍"。
        goldMask  = ir.goldMask;
        marked    = ir.marked;

        for (const auto& s : ir.sites) {
            QJsonObject cj;
            cj["label"] = QString::fromUtf8(s.label.c_str());
            cj["kind"]  = QString::fromUtf8(s.kind.c_str());
            cj["x"]     = s.roi.x;
            cj["y"]     = s.roi.y;
            cj["w"]     = s.roi.width;
            cj["h"]     = s.roi.height;
            // 判定规则随元件一起下发。
            //
            // 以前规则写死在判定算子里(`if (label == "R2")`),
            // 于是换块板那七处一条都匹配不上。现在由配方决定,
            // 判定算子只认这个名字 —— 换板子不改代码。
            QJsonArray checks;
            for (const auto& c : s.checks) {
                checks.append(QString::fromUtf8(c.c_str()));
            }
            cj["checks"] = checks;
            // 极性条应该在哪一侧("left"/"right")。**必须往下游传**。
            //
            // ⚠ 2026-10-20 真踩过:这个字段漏在这里,后果是
            //   判定算子(从 components JSON 重建站点)拿不到基准 ->
            //   整条 polarity 规则静悄悄地变成"没查"(skipped)。
            //   界面上看不出区别:缺陷列表少了一条,报告还是 PASS,
            //   好板和"没查极性"的坏板长得一模一样。
            //   判据本身(core)一直是对的,坏的是这一跳的边界。
            //
            //   空串 = 这个元件没有极性条,那就**不发这个字段**
            //   (不是发空串)—— 判定那边分得清"没写"和"写了但是空"。
            if (!s.polaritySide.empty())
                cj["polarity_side"] = QString::fromUtf8(s.polaritySide.c_str());
            // 端子(焊盘)位置。**必须往下游传** —— 短路判据量的是
            // "两个端子之间的绝缘缝里有没有金属",拿不到端子位置就
            // 判不了,整条规则会静悄悄地变成"没查"(skipped)。
            //
            // ⚠ 和 polarity_side 是同一类坑:字段漏在这一跳,
            //   核心那边只能跳过,跳过的条目混在报告里没人看,
            //   好板照样 PASS —— 而真正短路的废板也 PASS。
            //   (登记端子的是生成器,配方带下来,这里原样搬运。)
            if (!s.pads.empty()) {
                QJsonArray pa;
                for (const auto& p : s.pads) {
                    QJsonArray one;
                    one.append(p.x);
                    one.append(p.y);
                    one.append(p.r);
                    pa.append(one);
                }
                cj["pads"] = pa;
            }
            // 偏移容差按封装给(配方里可省,省了用算子默认)。
            // 写死成一个数等于"所有元件一样灵":小电阻挪 20px 可能
            // 还没出框,QFP64 挪 20px 就是大事。
            if (s.hasShiftTol) cj["shift_tol_pct"] = s.shiftTolPct;
            // ROI 内的金像素**占比**。⚠ 这个字段**曾经是死的**:
            // 写了但全仓库没有任何消费者(2026-10-09 实测 grep),
            // 而它恰好是唯一能暴露"定位算子与判定算子的金层掩膜已经
            // 漂开"的字段 —— 判据那边早改成相对 V 下界了,这边的绝对
            // V>=80 没动,56/56 端到端测试照样全绿,因为没人读这个数。
            //
            // 现在给的是**占 ROI 面积的比例**,和下游所有阈值同一量纲;
            // 像素数留着只会被误当成能跨分辨率用的数。
            cj["gold_pct"] = s.goldPct;
            // 框里"非板面基色"像素占比。缺件判据(彩色本体)读的就是它。
            cj["body_pct"] = s.bodyPct;
            compsJson.append(cj);
        }

        // 站点被丢掉的要记账。以前是"越界就 continue",于是
        // 上游少给几个元件,下游照样报 PASS,没人知道少算了。
        QStringList skipped;
        for (const auto& s : ir.skippedSites) skipped << QString::fromUtf8(s.c_str());
        // 本体检测:每个站点的框里到底有没有东西。见下面那道闸。
        const int sitesWithBody = ir.sitesWithBody;

        if (!skipped.isEmpty()) {
            output["warning"] = QString(
                "配方里的 %1 个站点(%2)在这张 %3x%4 的图上算不出有效 ROI,"
                "已经跳过 —— 下游只能判定剩下的 %5 个元件,"
                "不要把这次的结论当成整板都查过了。")
                .arg(skipped.size()).arg(skipped.join(","))
                .arg(img.cols).arg(img.rows).arg(compsJson.size());
        }

        // ============================================================
        // 配方-图像匹配门禁
        //
        // 这不是"优化",是**堵一个静默失效**:配方和图配错时,
        // 原来会输出一份结构完整的判定结果,而且看起来完全正常。
        // 用户没有任何线索知道自己检的是另一块板。
        //
        // 宁可拒绝判定。理由和前面 auto_contour 的守卫一样:
        // "查不了"要说出来,"查了一半说 PASS"才是产线事故。
        //
        // 阈值取"多数站点都有本体"(过半)。全空 = 肯定配错;
        // 一两个站点空 = 那个元件真被拆了,那是缺件缺陷,归判定算子管。
        // ============================================================
        if (!compsJson.isEmpty() && sitesWithBody * 2 < compsJson.size()) {
            output["error"] = QString(
                "配方 \"%1\"(%2 个站点)跟这张 %3x%4 的图对不上:"
                "%5 个站点里只有 %6 个能找到元件本体(阈值:过半)。"
                "很可能是**选错板型了** —— 换一张配方再试。"
                "这次没有判定结果,不要当成合格。")
                .arg(recipe.name).arg(compsJson.size())
                .arg(img.cols).arg(img.rows)
                .arg(compsJson.size()).arg(sitesWithBody);
            return output;
        }
        output["recipe"] = recipe.name;
    }

    // 路径一律走 artifactPath —— 固定名会让帧之间互相覆盖,
    // 理由见 artifact_path.h 的说明。
    QString markedPath = w7::artifactPath(
        "pcb_inspect",
        inputs.value("node_uuid").toString(),
        inputs.value("frame_index").toInt(-1));
    QDir().mkpath(QFileInfo(markedPath).absolutePath());
    cv::imwrite(markedPath.toStdString(), marked);

    output["marked_path"] = markedPath;
    output["components"] = compsJson;
    output["count"]       = compsJson.size();
    output["mode"]        = mode;
    // 原图路径往下传:判定算子还要 imread 同一张图,
    // 不能让它依赖工具栏兜底(工具栏改了就对不上了)
    output["image_path"]  = imgPath;
    return output;
}

QWidget* PcbInspect::createParamWidget(QWidget* parent, const QJsonObject& params) {
    auto* form = new PcbInspectForm(parent);
    form->initialize();
    return form;
}

// ============================================================
// PcbInspectForm
// ============================================================
PcbInspectForm::PcbInspectForm(QWidget* parent) : ParamForm(parent) {}

void PcbInspectForm::buildUI() {
    m_mode = new QComboBox;
    m_mode->addItem(tr("真值坐标(按板型配方)"), "known_site");
    m_mode->addItem(tr("自动轮廓(无配方,不判定)"), "auto_contour");

    // 板型配方下拉框。列的是 recipes/ 下真实存在的文件 ——
    // 列一个不存在的板型,比不列更糟:用户选了它,
    // 拿到的会是"读不到配方文件",而不是"这块板不支持"。
    m_recipe = new QComboBox;
    const QStringList recipes = listRecipes();
    if (recipes.isEmpty()) {
        // recipes/ 读不到时不静默给空下拉框:那会让界面上
        // "板型"这一栏什么都没有,用户不知道该填什么。
        m_recipe->addItem(tr("(未找到 recipes/ 目录)"), QString());
        m_recipe->setEditable(true);
    } else {
        for (const QString& p : recipes) {
            // 显示板名而不是路径,免得一行太长看不清;
            // 但 currentData 存完整相对路径,那是 run() 要用的。
            const QString shown = QFileInfo(p).completeBaseName();
            m_recipe->addItem(shown, p);
        }
        m_recipe->setEditable(true);   // 允许直接输路径加载自定义配方
    }

    m_hLow  = new QSpinBox;  m_hLow->setRange(0, 179);  m_hLow->setValue(85);
    m_sLow  = new QSpinBox;  m_sLow->setRange(0, 255);  m_sLow->setValue(80);
    m_hHigh = new QSpinBox;  m_hHigh->setRange(0, 179); m_hHigh->setValue(115);
    // V 下界不再是绝对值,改成"板面基色 x 多少倍",用双精度小数的框。
    m_vMul = new QDoubleSpinBox; m_vMul->setRange(0.0, 4.0);
    m_vMul->setDecimals(2); m_vMul->setSingleStep(0.05); m_vMul->setValue(0.8);
    m_minAreaPct = new QDoubleSpinBox; m_minAreaPct->setRange(0.0, 10.0);
    m_minAreaPct->setDecimals(3); m_minAreaPct->setSingleStep(0.001);
    m_minAreaPct->setValue(PcbInspectDefaults::kMinAreaPct);

    m_layout->addRow(tr("定位模式"),   m_mode);
    m_layout->addRow(tr("板型配方"),   m_recipe);
    m_layout->addRow(tr("金层 H 下限"), m_hLow);
    m_layout->addRow(tr("金层 S 下限"), m_sLow);
    m_layout->addRow(tr("金层 H 上限"), m_hHigh);
    m_layout->addRow(tr("金层 V 下界 = 板面色 x"), m_vMul);
    m_layout->addRow(tr("最小连通域 占全图 %"), m_minAreaPct);
}

void PcbInspectForm::setParams(const QJsonObject& params) {
    const QString mode = params.value("mode").toString(
                              QString::fromUtf8(PcbInspectDefaults::kMode));
    const int idx = m_mode->findData(mode);
    if (idx >= 0) m_mode->setCurrentIndex(idx);

    // 配方:列表里有的按路径选中,没有的(自定义路径)直接填进去。
    // 找不到就保持下拉框当前值,不报错 —— 和下面几个阈值一样的规矩:
    // 界面上替 run() 再吵一遍没用,run() 才是真正决定成败的地方。
    const QString recipe = params.value("recipe").toString(kDefaultRecipe);
    const int ridx = m_recipe->findData(recipe);
    if (ridx >= 0) m_recipe->setCurrentIndex(ridx);
    else if (!recipe.isEmpty()) m_recipe->setCurrentText(recipe);

    // 读不出来就保持界面上的默认值,**不在这里报错**:
    // 界面控件自己 collect() 出来的永远是合法数字,只有手改 .sol.json
    // 才可能拿到怪东西 —— 那时 run() 会明确报错(见 run() 里的守卫),
    // 界面不必替它再吵一遍。
    double d = 0;
    QString err;
    if (readHsvScalar(params.value("hsv_h_low"),  "hsv_h_low",  &d, &err)) m_hLow->setValue(int(d));
    if (readHsvScalar(params.value("hsv_s_low"),  "hsv_s_low",  &d, &err)) m_sLow->setValue(int(d));
    if (readHsvScalar(params.value("hsv_h_high"), "hsv_h_high", &d, &err)) m_hHigh->setValue(int(d));
    if (readHsvScalar(params.value("mask_v_mul"),  "mask_v_mul", &d, &err)) m_vMul->setValue(d);
    if (readHsvScalar(params.value("min_area_pct"), "min_area_pct", &d, &err))
        m_minAreaPct->setValue(d);
}

QJsonObject PcbInspectForm::collect() const {
    QJsonObject p;
    p["mode"]        = m_mode->currentData().toString();
    p["recipe"]      = m_recipe->currentData().toString().isEmpty()
                     ? m_recipe->currentText() : m_recipe->currentData().toString();
    p["hsv_h_low"]   = m_hLow->value();
    p["hsv_s_low"]   = m_sLow->value();
    p["hsv_h_high"]  = m_hHigh->value();
    p["mask_v_mul"]  = m_vMul->value();
    p["min_area_pct"] = m_minAreaPct->value();
    return p;
}

} // namespace w7
