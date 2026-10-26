# 窄串扛不了路径

## 核心

> **路径是宽字符的活。窄串扛路径，编码就变成了「猜」。**
>
> 而「猜错」在这台机器上**常常看起来是对的** ——
> 本机 ACP=936（GBK）恰好有中文，
> 于是「窄串当路径」在本机**一路绿灯**，
> 换台机器（ACP=1252）就**炸**。

⇒ 判据不是「这个字符串是不是路径」，
而是「**这个字符串的编码由谁定**」。
答不上来 ⇒ 它就不该是 `std::string`。

## 现象（2026-10-26，CI 第一次在真 runner 上跑）

run `40293214498`，W6 的 `w6_cli` 套件：

```
[FAIL] 测试抛出异常:No mapping for the Unicode character exists in the target multi-byte code page.
       (case: 配方目录:能找到,且不会因为一个坏目录就整个废掉)
SUMMARY: pass=20 fail=1   RESULT=FAIL
```

同一份代码，本机 `tools/ci.ps1` **全绿**。

炸的那一行（HEAD 第 273 行）：

```cpp
const std::string d = w6::findRecipeDir((g_w6 / u8p("根本没有这个目录")).string());
```

## 根因：MSVC 按 **ACP** 解释窄串

| 方向 | 写法 | MSVC 怎么做 | 失败形态 |
|---|---|---|---|
| 窄 → 宽 | `fs::path(窄串)` | 按 **ACP** 解释那段字节 | **不报错**。936：24 字节 UTF-8 → 12 个汉字；1252：→ 24 个 Latin-1 字符。**两台机器都不是原来那条路径** |
| 宽 → 窄 | `path::string()` | 按 **ACP** 窄化，**严格** | **抛 1113**（`ERROR_NO_UNICODE_TRANSLATION`）—— 字符在 ACP 里表示不出来就抛 |

> ⚠ 这两条**方向相反、结果不同**，别混成一句。
> 混了就会**查错方向**：去查窄→宽，而 runner 上炸的是宽→窄。
> （我第一版就混了，还照着错的版本改了注释 —— 见下面的「教训·推论三」。）

## 为什么本机是绿的

ACP=936（GBK）**有**中文 ⇒ 中文在 ACP 里表示得出来 ⇒ `.string()` 不抛。

runner 的 ACP=1252（Latin-1）**没有**中文 ⇒ 抛 1113。

> **同一行代码、同一份数据，两个结果。**
> 差别不在代码，在**这台机器的 ACP**。

### 而这条规则，本机就能证成

规则是「字符在 ACP 里表示不出来就抛」。
本机 ACP=936 **有**中文 ⇒ 中文不踩雷；
但 GBK **没有** emoji（U+1F600）⇒ 拿 emoji 当路径，本机就抛**一模一样**的消息：

```
ASCII 路径       : OK len=11
中文路径(GBK有) : OK len=16
emoji  (GBK没有) : THREW: No mapping for the Unicode character exists in the target multi-byte code page.
```

⇒ **机器只决定哪个字符踩雷，规则是同一条。**
（可复现的探针：`tools/probe-acp.cpp`。）

⚠ 而且这种机器事实**进程里改不动**：
`_setmbcp(1252)` 试过了，`path::string()` 的行为**一点没变**。
⇒ 「本机模拟一下 runner」这条路是**堵死的**；
能做的只有「**换触发条件，把规则逼出来**」。

## 修法：类型 + 编译器

### ① 路径一律 `std::filesystem::path`

它内部是宽字符，**不过任何代码页**。
「这个 `std::string` 是一条路径」这种句子，写出来就是错的。

### ② 文本 ↔ 路径的转换，全仓库**只留一处**

`common/utf8_path/utf8_path.h`：

- `text(path)` → **UTF-8** 文本（写进 CSV、跟 manifest.csv 比文件名）
- `fromUtf8(str)` → 路径（UTF-8 文本 → 路径）

> 为什么必须是 UTF-8 而不是「机器本地的那一套」：
> 仓库里的 `manifest.csv` 是 UTF-8，
> 文件名要和表里的内容**按同一套编码**比，
> 否则比不出来 = 查不到板型 = 这块板被**静默跳过**。
> 在报告里，被跳过的板和合格的板**长得一模一样**。

### ③ argv 是 **ACP**，直接 `fs::path(argv[i])`

操作系统给 `main` 的就是 ACP 窄串 ——
**不要**再套 `fromUtf8()`，那是**把对的搞坏**。

> 这也是「路径用 `fs::path`、文本用 `std::string`」最实在的好处：
> argv 那一处**不需要任何转换**，也就没有转换错的机会。

### ④ 窄串守卫：让编译器记，别让人记

光把 API 改成 `fs::path` **不够** ——
`std::filesystem::path` 有个**隐式**构造函数 `path(const std::string&)`，
它按 ACP 解释那段窄串。于是 `f("D:/中文/x.exe")` **照常编译**，路径已经不是那条路径了。

⇒ 在真身旁边加一个**故意编译不过**的模板重载，**只把路径参数**换成模板参数：

```cpp
bool runBoard(const fs::path& imgPath, const fs::path& recipeDir,
              BoardResult& out, std::string& errOut);   // 真身

// 守卫
template <class S1, class S2,
          std::enable_if_t<!utf8path::IsPathArg<S1>::value ||
                           !utf8path::IsPathArg<S2>::value, int> = 0>
bool runBoard(const S1& imgPath, const S2& recipeDir,
              BoardResult& out, std::string& errOut) {
    static_assert(utf8path::IsPathArg<S1>::value &&
                  utf8path::IsPathArg<S2>::value, UTF8PATH_ONLY_PATH_MSG);
    return false;
}
```

窄串落到守卫上 → **编译不过**，而且错的就是**那一行**。

> ⚠ **只把路径参数**换成模板参数，**不能**整个函数变参：
> `std::string` 在同一个函数里可能**两种身份**都有 ——
> `runBoard` 的路径参数是路径，而它的 `errOut` 是**文本**。
> 变参模板分不清「这个 `std::string` 是路径」还是「这个 `std::string` 是文本」，
> 只好一律拦 —— 那会把合法调用也拦掉。

### ⑤ 守卫要**咬一口**才算数

守卫是「看着对」还是「真的拦得住」，得**验**：

1. 故意把一处改回裸窄串 → build 报
   `static assertion failed: path parameter takes std::filesystem::path only`
2. 再改回来。

> 没咬过一口的守卫，和注释一样：**没人验证过，它可能根本没生效**。

## 局限（边界必须写清）

- 这个守卫**只管函数参数**。
  **结构体成员赋值**不走函数调用，编译器拦不住 ——
  那一类目前只有两处（`OcrOptions::workDir` / `tesseractPath`），
  都是**纯 ASCII**（ASCII 在 UTF-8 和所有 Windows ACP 里表示同样的字符，所以现在是安全的），
  但**没有机器守着**。
  **重开条件**：谁要让那两个成员装**非 ASCII** 路径时，
  先把守卫扩到成员上（比如换成一个只认 `fs::path` 的小包装类型）。
- W7 的算子还有约 30 处 `cv::imread(QString::toStdString(), …)` ——
  OpenCV 内部按 ANSI 打开文件，中文路径读不出来而且**不报错**。
  那一类是**已知限制**，不是本次修的。

## 教训

推论一：

> **窄串的编码不由类型定，由「它从哪来」定。**
>
> 命令行 argv 是 ACP；UTF-8 文件读出来的是 UTF-8；
> `QString::toStdString()` 是 UTF-8；`getenv` 是 ACP。
>
> ⇒ 它们**全是 `std::string`**，而它们的编码**各不相同**。
> ⇒ 「这个 `std::string` 是什么编码」答不上来时，**它就不该是 `std::string`**。

推论二：

> **「本机绿」和「仓库绿」是两个问题，而窄串这种坑**总是**答第二个时翻车。**
>
> 因为本机的 ACP 恰好**有**中文 —— 错的那条路径**看起来是对的**。
>
> ⇒ 凡是**机器事实**参与的判断（代码页、PATH、装没装），
> 本机的绿**不算数**。只能去那台机器上问。

推论三：

> **一个方向的失败，不能推广到另一个方向。**
>
> 我把「窄→宽」和「宽→窄」混成一句，
> 照着错的版本改了注释、改了 API 形状，
> 还差点把**病根留在原地**。
>
> ⇒ 两个方向**各测一次**，得出两条**各自**的结论。
> 混成一句的那一刻，**两条都不可信了**。

推论四：

> **注释里的数字没人验证过，它可能是推演出来的。**
>
> 我那版探针的注释是推的，跑起来跟写的**两处对不上**：
> `WC_ERR_INVALID_CHARS` 在 936/1252 上是 **1004** 不是 1113；
> `std::cout << const wchar_t*` 打的是**指针地址**不是字符串。
>
> ⇒ 写完就跑一遍，**把真实输出粘回去**。

## 检查清单

- [ ] 这个参数是**路径**吗？是的话，它的类型是 `fs::path` 吗？
- [ ] 这个 `std::string` 是**文本**吗？它的编码**由谁定**，说得清吗？
- [ ] 从 `argv` 来的路径，是直接 `fs::path(argv[i])` 吗（**没**多套一层 `fromUtf8`）？
- [ ] 有没有把窄串**悄悄**转成了 `fs::path`（隐式构造，编译器不拦）？
- [ ] 要写进 CSV / 跟 manifest.csv 比的路径，走的是 `utf8path::text()` 吗？
- [ ] 新加的守卫，**咬过一口**吗？

配套：`at-the-runtime-dll-lookup-is-a-machine-fact.md`（运行期按名字找 DLL 是机器事实）、
`at-an-empty-dir-is-not-in-git.md`（空目录不在 git 里）、
`at-silent-fail-shape.md`（静默失败的形状）、
`at-the-verdict-must-be-checkable.md`（结论必须可查）。

参见 `notes/notes_CI_FirstRunnerRun_2026-10.md` §五（这一笔的完整记录）、
`common/utf8_path/utf8_path.h`（转换的唯一定义 + 守卫）、
`tools/probe-acp.cpp`（可复现的探针）。
