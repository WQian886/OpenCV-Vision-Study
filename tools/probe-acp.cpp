// 探针:确认「路径的窄化/宽化」到底走的是哪条编码规则。
//
// ============================================================
// 怎么跑(不是 cmake 目标,得手动编)
// ============================================================
//
//   cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cl /nologo /EHsc /utf-8 /std:c++17 tools\probe-acp.cpp /Fe:%TEMP%\probe-acp.exe && %TEMP%\probe-acp.exe'
//
// ⚠ **/utf-8 不能省** —— 本文件里有中文字面量,
//   少了它 MSVC 按 ACP 读源文件,探针自己测的就不是它要测的东西了。
// ⚠ **/std:c++17 不能省** —— std::filesystem 要 C++17。
//   (这两个标志第一次都漏了,探针自己编不过 ——
//    「命令本身已在本机实测过」这句话,得真的跑一遍才算数。)
//
// ============================================================
// 为什么需要它
// ============================================================
//
//   第一次在真 CI runner(windows-2022)上跑,W6 的 w6_cli 测试抛了
//
//     "No mapping for the Unicode character exists in the target multi-byte code page"
//
//   (= Win32 错误 1113,ERROR_NO_UNICODE_TRANSLATION)。
//   **本机却一路绿。**
//
//   ⇒ 差别**不在代码**,在**机器的 ACP**。
//   这个探针把「ACP 是几」「两个方向各自会怎样」
//   都打出来,免得这一条只靠「我说是这样」。
//
// ============================================================
// ⚠ 两个方向**结果不一样**,别混成一句
// ============================================================
//
//   ① 宽 -> 窄(`path::string()`):MSVC 按 **ACP** 窄化,而且是
//       **严格**的 —— 字符在 ACP 里表示不出来就**抛 1113**。
//         ACP=936(本机 GBK):中文在 GBK 里 -> 转得出 -> **绿**
//         ACP=1252(runner):中文在 Latin-1 里表示不出来 -> **抛 1113**
//       ⇒ 这一条才是**「本机绿、runner 红」**。
//         runner 上炸的**就是它**(原代码里 `(g_w6 / u8p("根本没有这个目录")).string()`)。
//
//   ② 窄 -> 宽(`fs::path(窄串)`):也按 ACP,但转不了就**不报错**。
//         喂一段**真正的 UTF-8** 的窄串进去:
//         ACP=936:24 字节 -> 12 个汉字(错的)
//         ACP=1252:24 字节 -> 24 个 Latin-1 字符(还是错的)
//       ⇒ 这一条**每台机器都错**,只是错得不一样,而且**一声不吭**。
//         它是**类型**问题(拿文本当路径),不是机器差异。
//
//   混成一句的后果:以为 ② 才是机器差异,于是去查 ②,
//   而 runner 上真正炸掉的是 ① ——
//   **查错方向,改错地方,病根还在。**
//
//   (我自己就犯过这个错:第一版探针把 ② 写成"本机绿、runner 红",
//    还照着它改了 utf8_path.h 的注释 ——
//    是 runner 日志里那句 "target multi-byte code page"
//    和 HEAD 里第 273 行 `(g_w6 / u8p(...)).string()` 把它掰回来的。)
//
// ============================================================
// ★ 本机复现 runner 的失败(不用去 runner)
// ============================================================
//
//   规则是「字符在 ACP 里表示不出来就抛」。
//   本机 ACP=936(GBK)**有**中文,所以中文不炸;
//   但 GBK **没有** emoji(U+1F600) ——
//   拿 emoji 当路径,本机就抛**一模一样**的 1113、一模一样的消息。
//
//   ⇒ 这条**规则**在本机就能证成;机器只决定**哪个字符**踩雷。
//     runner 的 ACP=1252,中文踩雷;本机的 ACP=936,emoji 踩雷。
//
// ============================================================
// 本机实测输出(2026-10-26,中文 Windows, ACP=936)
// ⚠ 这段是**粘贴真实输出**的,不是手写的 —— 手写过两版,两版都跟实际对不上。
// ============================================================
//
//   GetACP() = 936 (GBK)
//   GetOEMCP() = 936
//
//     ① 窄 -> 宽:MultiByteToWideChar(ACP, MB_ERR_INVALID_CHARS) on 24 UTF-8 bytes:
//           ACP 936  : OK,   12 wide chars
//           ACP 1252  : OK,   24 wide chars
//           ACP 65001  : OK,    8 wide chars
//
//     ② 宽 -> 窄:path::string() 是**严格**的(转不了就抛):
//           ASCII 路径       : OK len=11
//           ASCII 路径       : u8string() OK len=11
//           中文路径(GBK有) : OK len=16
//           中文路径(GBK有) : u8string() OK len=24
//           emoji  (GBK没有) : THREW: No mapping for the Unicode character exists
//                                       in the target multi-byte code page.
//           emoji  (GBK没有) : u8string() OK len=10
//
//     (对照) WideCharToMultiByte(ACP, 0) —— 宽松版,不抛:
//           ACP 936  : OK,   16 bytes (宽松:转不了换 '?')
//           ACP 1252 : OK,    8 bytes (宽松:转不了换 '?')
//
//     ③ fs::path(窄的 UTF-8 串) 在本机上:
//           和原路径是同一个? NO
//           变成: 鏍规湰娌℃湁杩欎釜鐩綍
//           (原串 24 字节 -> 变成 36 字节)
//
//     ④ 试着把代码页改成 1252: _setmbcp(1252) -> 0
//           改完之后 path::string() 的行为:
//           中文路径(GBK有) : OK len=16
//           中文路径(GBK有) : u8string() OK len=24
//           emoji  (GBK没有) : THREW: No mapping for the Unicode character exists
//                                       in the target multi-byte code page.
//           emoji  (GBK没有) : u8string() OK len=10
//           => 一点没变。代码页是**机器**的事,进程里改不了。
//
//   读法:
//     · ①③ = 窄->宽:**每台机器都错**,而且不报错。
//     · ②   = 宽->窄:**本机绿、runner 红**的那一条。
//                 runner 的 ACP=1252 里没有中文 -> 中文踩雷;
//                 本机的 ACP=936 里有中文 -> 不踩,
//                 换个 GBK 也没有的 emoji,本机就踩同一个雷(同一条消息)。
//     · ④   = 这种机器事实**没法在进程里造出来**,只能去 runner 验。
//
//   注意 (A) 那两行:936 之所以"OK",是因为 UTF-8 字节**碰巧**是合法 GBK。
//   碰巧 = 换个词就不成立了,而**不成立的时候是静悄悄的**。
// ============================================================
#include <windows.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// ⚠ MSVC 的头文件里**没有**声明 _setmbcp(只有 CRT 里导出的那个符号),
//   所以这里自己声明一个。
extern "C" int __cdecl _setmbcp(int);

// 窄 -> 宽。fs::path(窄串) 走的就是这条路。
static void narrowToWide(unsigned cp, const char* bytes, int len) {
    wchar_t buf[256] = {};
    // ⚠ MB_ERR_INVALID_CHARS:转不了就**报错**,而不是替换成 '?'。
    const int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes, len,
                                       buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) {
        const DWORD err = GetLastError();
        std::cout << "      ACP " << cp << "  : FAIL, GetLastError=" << err
                  << "  ("
                  << (err == 1113 ? "ERROR_NO_UNICODE_TRANSLATION" : "?")
                  << ")\n";
        return;
    }
    std::cout << "      ACP " << cp << "  : OK,   " << n << " wide chars\n";
}

// 宽 -> 窄。path::string() 走的就是这条路。
// ⚠ 注意:path::string() 走的是 CRT 的**严格**转换,转不了就抛;
//   而这里带 flags=0 的调用是**宽松**的(替换成 '?')。
//   两者行为不一样,所以这一行**不能**用来预测 path::string()。
static void wideToNarrowLenient(unsigned cp, const wchar_t* w) {
    char buf[256] = {};
    const int n = WideCharToMultiByte(cp, 0, w, -1, buf, sizeof(buf),
                                      nullptr, nullptr);
    if (n == 0) {
        std::cout << "      ACP " << cp << "  : FAIL, GetLastError=" << GetLastError() << "\n";
        return;
    }
    std::cout << "      ACP " << cp << "  : OK,   " << (n - 1) << " bytes (宽松:转不了换 '?')\n";
}

// path::string():**严格**。转不了就抛 1113。
static void strictNarrow(const char* tag, const fs::path& p) {
    std::cout << "      " << tag << " : ";
    try {
        const std::string s = p.string();
        std::cout << "OK len=" << s.size() << "\n";
    } catch (const std::exception& e) {
        std::cout << "THREW: " << e.what() << "\n";
    }
    std::cout << "      " << tag << " : u8string() ";
    try {
        std::cout << "OK len=" << p.u8string().size() << "\n";
    } catch (const std::exception& e) {
        std::cout << "THREW: " << e.what() << "\n";
    }
}

int main() {
    // 1. 本机的 ACP 是几
    char acpName[16] = {};
    const DWORD acp = GetACP();
    if (acp == 936)        strcpy_s(acpName, "GBK");
    else if (acp == 1252)  strcpy_s(acpName, "1252 (Latin-1)");
    else if (acp == 65001) strcpy_s(acpName, "65001 (UTF-8)");
    else                   strcpy_s(acpName, "other");
    std::cout << "GetACP() = " << acp << " (" << acpName << ")\n";
    std::cout << "GetOEMCP() = " << GetOEMCP() << "\n";

    // 2. 窄 -> 宽:一段**真正的 UTF-8** 字节喂进去会怎样
    const fs::path p = fs::u8path("根本没有这个目录");
    const std::string utf8 = p.u8string();
    std::cout << "\n  ① 窄 -> 宽:MultiByteToWideChar(ACP, MB_ERR_INVALID_CHARS) on "
              << utf8.size() << " UTF-8 bytes:\n";
    narrowToWide(936, utf8.data(), static_cast<int>(utf8.size()));   // 中文 Windows
    narrowToWide(1252, utf8.data(), static_cast<int>(utf8.size()));  // 英文 Windows(GitHub runner)
    narrowToWide(65001, utf8.data(), static_cast<int>(utf8.size())); // UTF-8

    // 3. 宽 -> 窄:MSVC 窄化用的是 ACP,而且**严格**。
    //    本机 ACP=936 有中文 -> 绿;runner 的 1252 没中文 -> 抛 1113。
    //    emoji 在 GBK 里也没有 -> 本机就能把 runner 那个失败原样复现。
    std::cout << "\n  ② 宽 -> 窄:path::string() 是**严格**的(转不了就抛):\n";
    strictNarrow("ASCII 路径      ", fs::path(L"abc/def.txt"));
    strictNarrow("中文路径(GBK有)", fs::path(L"根本没有这个目录"));
    strictNarrow("emoji  (GBK没有)", fs::path(L"\U0001F600/x.txt"));

    // 4. 对照:宽松的宽->窄(带 flags=0)不抛,只是把字符换成 '?'。
    //    拿它去预测 path::string() 会**预测错**。
    std::cout << "\n  (对照) WideCharToMultiByte(ACP, 0) —— 宽松版,不抛:\n";
    wideToNarrowLenient(936, L"根本没有这个目录");
    wideToNarrowLenient(1252, L"根本没有这个目录");

    // 5. 窄 -> 宽:MSVC 按 ACP 解释那个窄串。
    //    所以"UTF-8 的窄串"在 ACP=936 上会变成 12 个汉字,
    //    在 ACP=1252 上会变成 24 个 Latin-1 字符 —— **两台机器都错**。
    const fs::path back(utf8);
    std::cout << "\n  ③ fs::path(窄的 UTF-8 串) 在本机上:\n";
    std::cout << "      和原路径是同一个? " << (back == p ? "YES" : "NO") << "\n";
    std::cout << "      变成: " << back.u8string() << "\n";
    std::cout << "      (原串 " << utf8.size() << " 字节 -> 变成 "
              << back.u8string().size() << " 字节)\n";

    // 6. 机器事实能不能在进程里改?
    //    改不了 —— 文件系统的窄化不跟着 CRT 的多字节代码页走。
    std::cout << "\n  ④ 试着把代码页改成 1252: ";
    const int rc = _setmbcp(1252);
    std::cout << "_setmbcp(1252) -> " << rc << "\n";
    std::cout << "      改完之后 path::string() 的行为:\n";
    strictNarrow("中文路径(GBK有)", fs::path(L"根本没有这个目录"));
    strictNarrow("emoji  (GBK没有)", fs::path(L"\U0001F600/x.txt"));
    std::cout << "      => 一点没变。代码页是**机器**的事,进程里改不了。\n";
    return 0;
}
