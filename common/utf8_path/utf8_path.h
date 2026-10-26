#pragma once
// ============================================================
// 路径 <-> UTF-8 文本 —— 全仓库**唯一**的一处定义
//
// === 为什么要单开一个共享核心 ===
//
// 本仓库栽过的最大一种坑是"抄副本"(同一件事写两遍,两份慢慢漂开)。
// 路径的编码转换正好是新的一次机会踩这个坑:
//   · W6 的 w6_cli.h 里有一份 pathText()
//   · common/qr_ocr/qr_ocr.cpp 里有一份 wideToUtf8() / utf8ToWide()
//   · W7 的算子里还有 QString::toStdString() 满天飞
// 三处各写各的,于是"路径到底按什么编码解释"这个问题**没人答得上来**。
//
// 所以规则写在这里,只有这一处:
//
//   1. **路径一律用 std::filesystem::path**。它内部是宽字符,
//      Windows 上不经过任何代码页转换 —— 编码问题**根本不存在**。
//      凡是"这个 std::string 是一条路径",那个 string 就是错的类型。
//
//   2. 路径要变成**给人看的文本**(写进 CSV、拼进错误消息、
//      跟 manifest.csv 里的文件名比对),走 text() —— 出来是 UTF-8。
//
//   3. UTF-8 文本要变成**路径**,走 fromUtf8()。
//
// === ⚠ 为什么不能用 path::string() ===
//
//   Windows 上 path::string() 把宽字符按 **ANSI 代码页(ACP)** 窄化,
//   而且是**严格**的:字符在 ACP 里表示不出来就**抛异常**
//   (Win32 1113 ERROR_NO_UNICODE_TRANSLATION:
//    "No mapping for the Unicode character exists in the target multi-byte code page")。
//
//   本机 ACP=936(GBK)**有**中文 -> 转得出 -> 绿;
//   GitHub runner 的 ACP=1252 **没有**中文 -> **抛**。
//   **同一份代码,一台绿一台红**。差别不在代码,在**这台机器的 ACP**。
//   第一次真跑 CI 就是在这一步翻车的(见
//   notes/notes_CI_FirstRunnerRun_2026-10.md 的第四个坑)。
//
//   这条规则**本机就能证成**:GBK 里没有 emoji(U+1F600),
//   拿 emoji 当路径,本机的 path::string() 抛**一模一样的 1113**。
//   机器只决定**哪个字符**踩雷,规则本身是同一条。
//   (可复现的探针:tools/probe-acp.cpp)
//
//   ⚠ **反方向是另一回事,别混成一句**:
//
//   窄串建路径(`fs::path(窄串)`)也按 ACP 解释那段窄串,
//   但转不了**不报错** —— 936 上 24 字节 UTF-8 变成 12 个汉字,
//   1252 上变成 24 个 Latin-1 字符。
//   ⇒ 那一条**每台机器都错**,只是错得不一样,而且一声不吭。
//     它是**类型**问题(拿文本当路径),不是机器差异。
//   (我自己第一版就是把这两条混成一句的,还照着错的版本改了注释 ——
//    是 runner 日志里那句 "target multi-byte code page" 把它掰回来的。)
//
// === 那 argv 怎么办 ===
//
//   argv 里的路径是 **ACP** 的(操作系统就是这么给 main 的),
//   所以 argv → 路径要用 `fs::path(argv[i])`(它按 ACP 解释,正好对)。
//   **不要**把 argv 当 UTF-8 再套 fromUtf8() —— 那是把对的搞坏。
//
//   这也是"路径用 fs::path、文本用 std::string"这条规则最实在的好处:
//   argv 那一处**不需要任何转换**,也就没有转换错的机会。
// ============================================================

#include <filesystem>
#include <string>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace utf8path {

// 路径 -> UTF-8 文本(给人看 / 写进 CSV / 跟 manifest.csv 比)
//
// 用 generic_(正斜杠)而不是原生的:路径要写进 CSV,
// 正斜杠跨平台一致,反斜杠在 CSV 里还得再转义一遍。
//
// u8string() 在 C++20 返回 std::u8string,这里统一转回 std::string,
// 免得哪天升了 C++ 标准就编不过。
inline std::string text(const std::filesystem::path& p) {
    try {
#if __cplusplus >= 202002L
        const std::u8string s = p.generic_u8string();
        return std::string(s.begin(), s.end());
#else
        return p.generic_u8string();
#endif
    } catch (const std::exception&) {
        // 路径里有连 UTF-8 都表示不了的字符(非法代理对之类)。
        // 退一步用 ACP 窄串 —— 这种情况它多半也转不了,
        // 那就让它**继续抛**:崩在这里比骗上面强。
        // 静默换个字符串的话,上层会拿它当路径去开文件,
        // 开不到还以为文件坏了 —— 那是本项目最恨的那种静默失效。
        return p.generic_string();
    }
}

// UTF-8 文本 -> 路径
//
// ⚠ 只有**确定是 UTF-8** 的文本才能走这里。
//   argv 不是 UTF-8(是 ACP),命令行参数请直接 fs::path(argv[i])。
inline std::filesystem::path fromUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::filesystem::path();
    // MB_ERR_INVALID_CHARS:非法 UTF-8 直接失败,而不是悄悄替换成 '?'
    // (悄悄替换 = 路径变成另一个路径,开不到却看不出为什么。)
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) {
        // 不是合法 UTF-8。按 ACP 兜底(至少不会崩),
        // 但这已经说明调用方把编码搞错了 —— 别指望它是对的。
        return std::filesystem::path(s);
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.data(), static_cast<int>(s.size()), &w[0], n);
    return std::filesystem::path(w);
#else
    return std::filesystem::u8path(s);
#endif
}

// 这个类型是"路径参数"吗?
//
// 判据:**就是** std::filesystem::path(去掉引用和 const)。
//   · fs::path / const fs::path&  -> 是
//   · std::string / const char*   -> 不是(它们是窄串)
//   · cv::Mat / bool / 指针        -> 不是
template <class T>
struct IsPathArg : std::is_same<std::decay_t<T>, std::filesystem::path> {};

} // namespace utf8path

// ============================================================
// 「路径参数不许收窄串」—— 交给**编译器**守
// ============================================================
//
// ⚠ 为什么光把 API 改成 fs::path 还**不够**:
//
//   std::filesystem::path 有个**隐式**构造函数 path(const std::string&)
//   —— 它按 **ACP**(ANSI 代码页)解释那段窄串。
//   于是 `f("D:/中文/x.exe")` **照常编译**,而路径已经不是那条路径了:
//   本机 ACP=936(GBK)把 24 字节 UTF-8 读成 12 个汉字,
//   runner 的 ACP=1252 读成 24 个 Latin-1 字符 ——
//   **两台机器都不对**,而且都不报错。
//   编译器**一声不响** —— 路径烂了,程序照跑,
//   报错发生在几百行之后,而且报的是「文件找不到」。
//
//   2026-10-26 门禁就逮到这一条:
//
//     [FAIL] 失败原因里写明了路径
//             (实际 "... D:/缁濆涓嶅瓨鍦ㄧ殑璺緞/tesseract.exe ...")
//
//   同一个仓库里另一处用对了(utf8path::fromUtf8),断言就过。
//   ⇒ **同一个类型,两种编码,只差一个转换。**
//   ⇒ 而"这次是谁审的"这种靠人记住的事,迟早会忘 —— 必须让编译器记。
//
// 怎么守:
//
//   在真身旁边加一个**故意编译不过**的模板重载,
//   里面**只把路径参数**换成模板参数(别的参数照抄):
//
//     // 真身
//     bool runBoard(const fs::path& imgPath, const fs::path& recipeDir,
//                   BoardResult& out, std::string& errOut);
//
//     // 守卫
//     template <class S1, class S2, ...>
//     bool runBoard(const S1& imgPath, const S2& recipeDir,
//                   BoardResult& out, std::string& errOut) { static_assert(...); }
//
//   窄串落到守卫上 -> **编译不过**,而且错的就是**那一行**;
//   fs::path 落到真身 -> 一点不受影响。
//
// ⚠ 为什么只把**路径参数**换成模板参数,而不是整个函数变参:
//
//   因为 `std::string` 在同一个函数里可能**两种身份**都有:
//   runBoard 的路径参数是路径,而它的 errOut 是**文本**。
//   变参模板分不清"这个 std::string 是路径"还是"这个 std::string 是文本",
//   只好一律拦 —— 那会把合法调用也拦掉。
//   所以守卫必须**逐个参数**地写。
//   (C++ 不能给函数模板做「部分特化」,没法用一个模板套住所有函数;
//    所以这个模式要在每个要守的函数旁边写一遍 ——
//    模式本身只有这一处定义:上面的 IsPathArg。)
//
// 为什么是「编译不过」而不是「加个检查」:
//
//   这条坑的坏处恰恰是**它不报错**,所以没有「检查得出来的时机」。
//   只能让它**编不过**。
//   (跟本项目那条「判据是依赖的,就交给编译器」是同一条。)
//
// 局限(**边界必须写清**):
//
//   这个守卫只管**函数参数**。结构体成员赋值不走函数调用,
//   编译器拦不住 —— 那一类目前只有两处
//   (OcrOptions::workDir / tesseractPath),都是**纯 ASCII**
//   (ASCII 在 UTF-8 和所有 Windows ACP 里表示同样的字符,所以现在是安全的),
//   但**没有机器守着**。
//   重开条件:谁要让那两个成员装**非 ASCII** 路径时,
//   先把这个守卫扩到成员上(比如换成一个只认 fs::path 的小包装类型)。
// ============================================================

// 守卫的报错文案。
//
// ⚠ 用 **ASCII**:编译器的输出会经过控制台代码页,
//   中文在那里可能变成乱码 —— 而那正是本文件要治的病。
#define UTF8PATH_ONLY_PATH_MSG                                                  \
    "path parameter takes std::filesystem::path only. "                          \
    "A narrow string would be decoded as the ANSI code page (ACP): "           \
    "silently wrong on every machine, this one included. "                      \
    "Use utf8path::fromUtf8() for UTF-8 text, "                                 \
    "or fs::path(argv[i]) for ACP command-line arguments."
