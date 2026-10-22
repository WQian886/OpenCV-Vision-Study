#include "qr_ocr.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
// QRCodeDetector 在 objdetect 里,不在 core 里 ——
// 少写这一行的话是"cv 不是成员"的语法错误,和"没链 objdetect"长得不像。
#include <opencv2/objdetect.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace qroc {
namespace {

// ============================================================
// 宽字符 <-> UTF-8
//
// ⚠ 不能用 std::wstring(s.begin(), s.end()) 那种逐元素转换:
//   wchar_t -> char 逐元素窄化,在非 ASCII 上会**毁掉**。
//   路径里可能有中文,必须走 WinAPI。
// ============================================================

#ifdef _WIN32
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}
#else
std::string wideToUtf8(const std::wstring&) { return std::string(); }
std::wstring utf8ToWide(const std::string&) { return std::wstring(); }
#endif

// 去掉行尾的 \r 和空格
void trimLineEnd(std::string& line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                             line.back() == '\t')) {
        line.pop_back();
    }
}

} // namespace

// ============================================================
// QR
// ============================================================
QrResult readQr(const cv::Mat& img) {
    QrResult out;
    if (img.empty()) return out;

    cv::QRCodeDetector det;
    std::vector<cv::Point2f> pts;
    // ⚠ detectAndDecode 对空图 / 非图会抛异常吗?实测不会,
    //   但 img.empty() 已经在上面挡住了。
    out.text = det.detectAndDecode(img, pts);
    out.found = !out.text.empty();
    if (out.found) out.corners = pts;
    return out;
}

// ============================================================
// tesseract 定位
// ============================================================
std::string defaultTesseractPath() {
#ifdef _WIN32
    // ⚠ 路径不能写死成**某一台机器**的事实。
    //
    //   以前这里写的是 D:\Program Files\Tesseract-OCR\tesseract.exe ——
    //   那是这台机器的装法。而 chocolatey 装的是
    //   C:\Program Files\Tesseract-OCR(它的 chocolateyinstall.ps1
    //   里 Install-ChocolateyPath 用的就是 %ProgramFiles%)。
    //
    //   写死的后果不是"多试一个地方":在别的机器上,
    //   "默认位置"指向一个压根不存在的文件,
    //   错误消息还会把人引到那个不存在的目录去。
    //   (CI 上就是靠 PATH 兜底才找到的 —— 那是运气,不是设计。)
    //
    //   现在按标准位置逐个找,找到真的那个就返回它;
    //   一个都没有,才返回标准位置 —— 错误消息总得有个地方可说。
    const char* kCandidates[] = {
        "C:\\Program Files\\Tesseract-OCR\\tesseract.exe",
        "D:\\Program Files\\Tesseract-OCR\\tesseract.exe",
    };
    std::error_code ec;
    for (const char* c : kCandidates) {
        if (fs::exists(fs::path(c), ec) && !ec) return std::string(c);
    }
    return std::string(kCandidates[0]);
#else
    return std::string();
#endif
}

std::string findTesseract(const std::string& explicitPath) {
    std::error_code ec;

    // 显式指定:存在就用,不存在就**不换**。
    //
    // ⚠ 为什么不"找不到就退回去找默认位置":
    //   调用方明确说了"用这个",我们偷偷换一个 = 拿另一个程序去跑,
    //   跑出来的结果调用方以为是它指定的那个。静默换人最阴险。
    if (!explicitPath.empty()) {
        if (fs::exists(fs::path(explicitPath), ec) && !ec) return explicitPath;
        return std::string();
    }

    // 默认安装位置
    const std::string def = defaultTesseractPath();
    if (!def.empty() && fs::exists(fs::path(def), ec) && !ec) return def;

    // PATH 里找
#ifdef _WIN32
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv) {
        std::stringstream ss(pathEnv);
        std::string dir;
        while (std::getline(ss, dir, ';')) {
            if (dir.empty()) continue;
            fs::path cand = fs::path(dir) / "tesseract.exe";
            if (fs::exists(cand, ec) && !ec) return cand.string();
        }
    }
#endif
    return std::string();
}

// ============================================================
// OCR
// ============================================================
OcrResult readText(const cv::Mat& img, const OcrOptions& opt) {
    OcrResult out;
    if (img.empty()) {
        out.error = "图像是空的,没读";
        return out;
    }

    // --- 1. 预处理:灰度 + **放大** + 自适应阈值 ---
    //
    // 放大是必须的:板上的丝印是 14~18px 的字(FONT_S / FONT_M),
    // 直接扔给 tesseract 基本读不出来 —— 之前整板 OCR 出来是一堆乱码。
    // 放大 3 倍之后字高到 42~54px,才进得了 tesseract 的射程。
    const double kScale = opt.scale > 0.0 ? opt.scale : 3.0;
    cv::Mat gray, big, thresh;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, big, cv::Size(), kScale, kScale, cv::INTER_CUBIC);
    cv::adaptiveThreshold(big, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY, 31, 10);
    out.preprocessed = thresh;

    // --- 2. 中间产物目录 ---
    //
    // 绝不能放在待检图目录里:那目录是批量报告的输入,
    // 往里丢中间产物,下一次扫描会把自己的中间产物当成待检板扫一遍。
    fs::path workDir;
    std::error_code ec;
    if (!opt.workDir.empty()) {
        workDir = fs::path(opt.workDir);
    } else {
        workDir = fs::temp_directory_path(ec) / "qroc_ocr";
        if (ec) workDir = fs::path(".");
    }
    fs::create_directories(workDir, ec);
    if (ec) {
        out.error = "中间产物目录建不出来: " + workDir.string() +
                    "(" + ec.message() + ")";
        return out;
    }

    const std::string stem = opt.stem.empty() ? std::string("ocr") : opt.stem;
    const fs::path prePath  = workDir / (stem + ".png");
    const fs::path outBase  = workDir / stem;
    const fs::path outTxt   = workDir / (stem + ".txt");

    if (!cv::imwrite(prePath.string(), thresh)) {
        out.error = "预处理图写不出去: " + prePath.string();
        return out;
    }

    // --- 3. 找 tesseract ---
    const std::string exe = findTesseract(opt.tesseractPath);
    if (exe.empty()) {
        if (!opt.tesseractPath.empty()) {
            out.error = "指定的 tesseract 路径不存在: " + opt.tesseractPath +
                        " —— 不会悄悄换一个来跑";
        } else {
            out.error = "没找到 tesseract(试过 " + defaultTesseractPath() +
                        " 和 PATH 里的 tesseract.exe)—— 装 Tesseract-OCR 才能读文字";
        }
        return out;
    }
    out.usedTesseract = exe;

#ifdef _WIN32
    // --- 4. 调 tesseract ---
    //
    // 用 CreateProcessW 而不是 system():tesseract 的路径里有空格,
    // system() 会把它切成两半。
    const std::wstring wExe    = utf8ToWide(exe);
    const std::wstring wPre    = utf8ToWide(prePath.string());
    const std::wstring wBase   = utf8ToWide(outBase.string());
    const std::wstring wLang   = utf8ToWide(opt.lang);
    const std::wstring cmdLine = L"\"" + wExe + L"\" \"" + wPre + L"\" \"" +
                                 wBase + L"\" -l " + wLang + L" --psm " +
                                 std::to_wstring(opt.psm);
    out.commandLine = wideToUtf8(cmdLine);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // CreateProcessW 要求命令行可写
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL,
                         &si, &pi)) {
        out.error = "CreateProcessW 失败,错误码 " +
                    std::to_string(static_cast<long long>(GetLastError()));
        return out;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ret = 1;
    GetExitCodeProcess(pi.hProcess, &ret);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (ret != 0) {
        out.error = "tesseract 返回码 " + std::to_string(static_cast<long long>(ret));
        return out;
    }

    // --- 5. 读结果 ---
    std::ifstream f(outTxt);
    if (!f) {
        out.error = "tesseract 跑完了但没产出 " + outTxt.string();
        return out;
    }
    std::string all, line;
    while (std::getline(f, line)) {
        trimLineEnd(line);
        if (line.empty()) continue;
        if (!all.empty()) all += " ";
        all += line;
    }
    f.close();

    out.text = all;
    out.ok = true;
    return out;
#else
    out.error = "文字 OCR 目前只有 Windows 实现(用 CreateProcessW 调 tesseract)";
    return out;
#endif
}

} // namespace qroc
