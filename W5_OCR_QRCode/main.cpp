// ============================================================
// W5: OCR + 二维码
//   - 默认 / ocr  → W5b OCR 文字识别 (调用 Tesseract)
//   - qr          → W5a QR Code 识别
//
// 学习目标::
//   W5a: cv::QRCodeDetector 检测 + 解码
//   W5b: 调用 Tesseract OCR 读 PCB 丝印文字
//
// ============================================================
// ⚠ 2026-10-20:实现搬到了 common/qr_ocr/,W5 / W6 / W7 **共用一份**
//
// 这个文件原来自己写了一份 QR 解码 + 一份 tesseract 调用。
// W6 那边抄了一份(修了宽字符转换的坑),W7 的 qr_decode 算子又抄了一份 QR。
// **三次**拷贝,而且从来没有被放在一起测过 ——
// 所以"W6 的 --psm 11 和 W5 的 --psm 6 哪个对"这个问题没人答得上来:
// 它们是两份拷贝,不是一份的两个参数。
//
// 现在:
//   qroc::readQr(img)             -> QR 结果(内容 + 四个角点)
//   qroc::readText(img, options)  -> 文字结果(ok / text / error)
//
// psm 现在是一个**参数**,W5 想用 6 就传 6,W6 想用 11 就传 11。
//
// ⚠ 这个文件保留的是**教学演示**(画框、显示、打印每一步),
//   算法本身在 common/qr_ocr/ 里。
//   保留演示而不是删掉,是因为"预处理长什么样"本身就是这周要学的东西 ——
//   但**定义**只能有一份。
// ============================================================

#include "qr_ocr.h"
#include "present_pipeline.h"

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// 是否显示窗口。CI / 无显示器环境要关掉 ——
// cv::imshow 在没有窗口系统时会抛异常。
static bool g_showWindow = true;

// ============================================================
// W5a QR 识别
// ============================================================
static void runQR() {
    const std::string imgPath = "images/qrcode_test.png";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cerr << "❌ 找不到 " << imgPath << "\n";
        std::cerr << "  请先跑 python gen_qr.py 生成测试二维码\n";
        return;
    }
    std::cout << "===== QR 识别 =====\n";
    std::cout << "  尺寸: " << img.cols << " x " << img.rows << "\n";

    const qroc::QrResult qr = qroc::readQr(img);

    if (!qr.found) {
        std::cout << "  ❌ 没识别到二维码\n";
    } else {
        std::cout << w5::qrDecodedLine(qr.text) << "\n";

        // W5 **自己**的规则:这个号长得对不对。
        //
        // ⚠ 这一行不是装饰 ——
        //   CI 第 4 步那道门禁搜的是「输出里有没有**一个像样的**流水号」。
        //   产品这边**先按规则认一遍**,门禁那边再**独立地**用同一个形状
        //   搜一遍 ⇒ 两个**互相不认识的证人**,认的是同一件事。
        //   (如果产品只是把解出来的原文往上一打,门禁那句正则就成了
        //    「原文碰巧长得像」—— 那它什么也没证明。)
        const w5::Serial serial = w5::findSerial(qr.text);
        if (serial.found) {
            std::cout << "  🔍 流水号: " << serial.text << "\n";
        } else {
            std::cout << "  ❌ 没认出流水号: " << serial.reason << "\n";
        }
    }

    // 画框 + 显示
    //
    // ⚠ 画框和写串号这两段现在都在 present_pipeline 里 ——
    //   demo 和测试调的是**同一份**实现(见 present_pipeline.h 的开头)。
    //   以前这两段只写在 main() 里,测试根本够不着它们。
    const cv::Mat marked = w5::markQr(img, qr.corners, qr.text);
    cv::imwrite("images/w5_qr_result.png", marked);

    if (g_showWindow) {
        cv::namedWindow("QR Code", cv::WINDOW_AUTOSIZE);
        cv::imshow("QR Code", marked);
        std::cout << "按任意键关闭窗口...\n";
        cv::waitKey(0);
        cv::destroyAllWindows();
    }
}

// ============================================================
// W5b OCR (调用 Tesseract 命令行)
// ============================================================
static void runOCR() {
    const std::string imgPath = "images/ocr_test.png";
    cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cerr << "❌ 找不到 " << imgPath << "\n";
        std::cerr << "  请先跑 python gen_ocr_test.py 生成测试图\n";
        return;
    }

    std::cout << "===== OCR 文字识别 =====\n";
    std::cout << "  图像: " << imgPath << "\n";
    std::cout << "  尺寸: " << img.cols << " x " << img.rows << "\n";

    // ---------- 第 1 步:OpenCV 预处理(灰度化 + 放大 + 自适应阈值) ----------
    //
    // 预处理在 common/qr_ocr/ 里做完了,这里拿到结果做教学展示。
    //
    // --psm 6 = "uniform block"(整块文字)。W5 的测试图是三行整整齐齐的字,
    // 用 6 合适。**板丝印是散落的**,那边要用 11 —— 那只是一个参数的区别。
    qroc::OcrOptions opt;
    opt.psm = 6;
    // 中间产物放 out/,**不能放 images/** ——
    // images/ 是"待检图"的目录,往里丢中间产物,下一次扫描会把自己的
    // 中间产物当成待检样本扫一遍(而且它没有 manifest 登记,
    // 会被报成"跳过",看着像少检了一块板)。
    // ⚠ 写成 fs::path(...),不是裸窄串 ——
    //   成员赋值不走函数调用,编译器拦不住窄串(见 utf8_path.h 的「局限」),
    //   所以这一处得**自己**写清楚:这是路径,不是文本。
    //   (纯 ASCII 所以现在两种写法结果一样;但规矩得是规矩。)
    opt.workDir = std::filesystem::path("out/ocr");
    opt.stem = "w5_ocr";

    const qroc::OcrResult r = qroc::readText(img, opt);

    // 第 2 步:调 tesseract(命令行为共享核心拼的,这里只展示)
    std::cout << "  调用: " << r.commandLine << "\n\n";

    if (!r.ok) {
        // ⚠ 失败必须**说出来**。
        //   以前这里是 "❌ CreateProcessW 失败" / "❌ Tesseract 调用失败",
        //   但 tesseract **没装**这种情况以前根本没处理 ——
        //   CreateProcessW 失败会报出来,可是"文件不存在"和"别的错"
        //   在旧代码里是同一条路径。
        //   现在共享核心把每一步的原因都写成了人话,这里直接显示。
        std::cerr << "  ❌ OCR 没跑成:" << r.error << "\n";
        return;
    }

    // 预处理图(教学用:看看送进 tesseract 的到底长什么样)
    if (!r.preprocessed.empty()) {
        cv::imwrite("out/ocr/w5_ocr_preprocessed.png", r.preprocessed);
        std::cout << "  预处理图已保存: out/ocr/w5_ocr_preprocessed.png\n";
    }

    // 第 3 步:读 OCR 输出
    std::cout << "===== 识别结果 =====\n";
    const std::string allText = r.text;
    // 分词在 present_pipeline 里 —— demo 和测试同一份(见该文件开头)。
    for (const std::string& token : w5::splitTokens(allText)) {
        std::cout << "  " << token << "\n";
    }
    if (allText.empty()) {
        std::cout << "  ❌ 没识别到文字\n";
    } else {
        std::cout << "\n✅ OCR 完成,共 " << allText.size() << " 字符\n";
    }

    // 第 4 步:在原图上画框 + 显示
    // ⚠ 画框和写 "OCR:" 现在都在 present_pipeline 里 ——
    //   demo 和测试同一份(见该文件开头)。
    const cv::Mat marked = w5::markOcr(img);
    cv::imwrite("images/w5_ocr_result.png", marked);
    if (g_showWindow) {
        cv::namedWindow("OCR Input", cv::WINDOW_AUTOSIZE);
        cv::imshow("OCR Input", marked);
        std::cout << "按任意键关闭窗口...\n";
        cv::waitKey(0);
        cv::destroyAllWindows();
    }
}

// ============================================================
// int main
// ============================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string mode = "ocr";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-gui") {
            // 无显示器 / CI 环境:cv::imshow 会抛异常。
            g_showWindow = false;
        } else {
            mode = a;
        }
    }

    try {
        if (mode == "qr") {
            runQR();   // 传 qr 跑 QR 识别
        } else {
            runOCR();  // 默认 = OCR 文字识别
        }
    } catch (const cv::Exception& e) {
        std::cerr << "❌ OpenCV 异常: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
