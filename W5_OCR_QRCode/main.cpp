// ============================================================
// W5: OCR + 二维码
//   - 默认 / qr  → W5a QR Code 识别
//   - ocr        → W5b OCR 文字识别 (调用 Tesseract)
// 学习目标::
//   W5a: cv::QRCodeDetector 检测 + 解码
//   W5b: 调用 Tesseract OCR 读 PCB 丝印文字
// ============================================================

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>      // system()
#include <fstream>      // 读 OCR 输出文件

#ifdef _WIN32
#include <windows.h>
#endif

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

    cv::QRCodeDetector detector;
    std::vector<cv::Point2f> points;
    std::string decoded = detector.detectAndDecode(img, points);

    if (decoded.empty()) {
        std::cout << "  ❌ 没识别到二维码\n";
    } else {
        std::cout << "  ✅ 解码: " << decoded << "\n";
    }

    // 画框 + 显示
    cv::Mat marked = img.clone();
    if (!decoded.empty() && points.size() == 4) {
        for (int i = 0; i < 4; ++i) {
            cv::line(marked, points[i], points[(i+1) % 4],
                     cv::Scalar(0, 0, 255), 3);
        }
        cv::putText(marked, decoded,
                    cv::Point((int)points[0].x, (int)points[0].y - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(0, 0, 255), 2);
    }
    cv::imwrite("images/w5_qr_result.png", marked);

    cv::namedWindow("QR Code", cv::WINDOW_AUTOSIZE);
    cv::imshow("QR Code", marked);
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();
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

    // ---------- 第 1 步:OpenCV 预处理(灰度化 + 自适应阈值) ----------
    cv::Mat gray, thresh;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::adaptiveThreshold(gray, thresh, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY, 31, 10);

    std::string prePath = "images/ocr_preprocessed.png";
    cv::imwrite(prePath, thresh);
    std::cout << "  预处理图已保存: " << prePath << "\n";

    // ---------- 第 2 步:调用 Tesseract (用 CreateProcessW 避免空格问题) ----------
    const std::wstring tesseract = L"D:\\Program Files\\Tesseract-OCR\\tesseract.exe";
    const std::wstring args      = L"\"" + std::wstring(prePath.begin(), prePath.end())
                                  + L"\" \"images\\ocr_result\" -l eng --psm 6";

    std::wstring cmdLine = L"\"" + tesseract + L"\" " + args;
    std::wcout << L"  调用: " << cmdLine << std::endl << std::endl;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // CreateProcessW 要求命令行可写
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        NULL,                  // 应用名(NULL=从命令行解析)
        cmdBuf.data(),         // 命令行
        NULL, NULL, FALSE,     // 安全属性 + 句柄继承
        0, NULL, NULL,         // 创建标志 + 环境 + 工作目录
        &si, &pi);
    if (!ok) {
        std::cerr << "❌ CreateProcessW 失败,错误码 " << GetLastError() << "\n";
        return;
    }
    // 等 Tesseract 跑完
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ret = 0;
    GetExitCodeProcess(pi.hProcess, &ret);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (ret != 0) {
        std::cerr << "❌ Tesseract 调用失败,返回码 " << ret << "\n";
        return;
    }

    // ---------- 第 3 步:读 OCR 输出 ----------
    std::ifstream fin("images/ocr_result.txt");
    std::string line, allText;
    std::cout << "===== 识别结果 =====\n";
    while (std::getline(fin, line)) {
        std::cout << "  " << line << "\n";
        allText += line + "\n";
    }
    if (allText.empty()) {
        std::cout << "  ❌ 没识别到文字\n";
    } else {
        std::cout << "\n✅ OCR 完成,共 " << allText.size() << " 字符\n";
    }

    // ---------- 第 4 步:在原图上画框 + 显示 ----------
    cv::Mat marked = img.clone();
    cv::rectangle(marked, cv::Rect(0, 0, img.cols, img.rows),
                  cv::Scalar(0, 255, 0), 3);
    cv::putText(marked, "OCR:",
                cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.8,
                cv::Scalar(0, 255, 0), 2);

    cv::imwrite("images/w5_ocr_result.png", marked);
    cv::namedWindow("OCR Input", cv::WINDOW_AUTOSIZE);
    cv::imshow("OCR Input", marked);
    std::cout << "按任意键关闭窗口...\n";
    cv::waitKey(0);
    cv::destroyAllWindows();
}

// ============================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string mode = (argc > 1) ? argv[1] : "ocr";

    if (mode == "qr") {
        runQR();   // 传 qr 跑 QR 识别
    } else {
        runOCR();  // 默认 = OCR 文字识别
    }
    return 0;
}