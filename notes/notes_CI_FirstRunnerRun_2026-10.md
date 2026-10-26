# CI 第一次在真 runner 上跑（2026-10-26）：欠账 9 的重开条件兑现

> 这一章记的是**欠账 9**（`notes/notes_W7_Knob_Audit_2026-10.md` §十）兑现的那一刻。
> 它的重开条件原文是「**仓库有 remote 的那一天**」。
> 今天 `gh` 已登录、网也通 → 仓库建了 → 这条路开了。
>
> 但**这一章真正记的不是「CI 跑起来了」**，
> 是**第一次真跑就挖出来的四件事** ——
> 前面三件都**不是代码写错了**，是「**只有这台机器成立**」的东西；
> 第四件**是**代码本身有歧义（拿窄串当路径），
> 而它**在这台机器上永远是绿的**。
>
> 四件里有**三件本地永远看不见**；
> 第四件本机门禁逮到过**同一类**的东西，但症状不一样
> （本机是路径乱掉，runner 上是直接抛异常）。

---

## 一、先说清楚：这件事的证据等级

| 说法 | 等级 | 依据 |
|---|---|---|
| 仓库有 remote，CI 在 `windows-2022` 上**启动过** | ✅ 实测 | run `40288478649` |
| 依赖准备那一步在真 runner 上**过了** | ✅ 实测 | `Provision dependencies` → success（choco tesseract / aqt Qt 6.8.3 / 官方 OpenCV world 包） |
| 整个 CI 一次跑到底全绿 | ⏳ **当时还没到**（下面这一章记的就是它怎么一步步走通的） | — |

**第一次 push 上去，job 3 秒就死了** ——
但不是代码问题：

```
The job was not started because recent account payments have failed
or your spending limit needs to be increased.
```

私有仓库 + 账号消费上限。
把仓库改成 **public** 之后拿到免费无限分钟，job 才真的开始跑。

> 顺带一句：**「job 没启动」和「job 跑失败了」在 UI 上长得不一样，但都叫红。**
> 差点把它当成「代码有问题」——那是**朝着错误的方向查**。

---

## 二、第一次真跑逮到的 Bug 1：空目录不在 git 里

### 现象

runner 日志：

```
1> w1_imageio.vcxproj -> ...\Release\w1_imageio.exe
 Copying OpenCV DLLs to output directory
 Error copying directory from ".../W1_ImageIO/images"
                          to ".../build/ci/W1_ImageIO/Release/images".
 ... error MSB3073: The command ""C:\Program Files\CMake\bin\cmake.exe"
     -E copy_directory .../W1_ImageIO/images .../Release/images" ...
```

### 根因

W1–W4 五个项目各有一个 `add_custom_command`：

```cmake
add_custom_command(TARGET w1_imageio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:w1_imageio>/images
)
```

而 **W1–W4 的 `images/` 在本机是空目录** ——
**git 不带空目录**（它只带文件）。

```
本机：    W1_ImageIO/images/     ← 存在（空的）
干净克隆：W1_ImageIO/images/     ← **不存在**
```

`cmake -E copy_directory` 的源目录不存在 ⇒ 它**报错** ⇒ **整个 build 挂掉**。

> **本地永远看不见这个 bug** ——
> 因为本机那个空目录**一直在**。
> 它是「**这台机器的一个事实**」，不是「**仓库的一个事实**」。

### 证据：不是推断，是本机复现

写了个探针（`%TEMP%\clone-build-probe.ps1`）：
**真的 clone 一份**，在里面 build W1。

```
--- does the clone have W1_ImageIO/images? ---
  NO -- the directory is absent from the clone
--- building ---
build exit code: 1
--- the failure, verbatim ---
  Error copying directory from ".../probe_clone_before/W1_ImageIO/images" to ...
  ... error MSB3073: ...
```

**跟 runner 上同一个错。** 根因坐实。

> ⚠ 探针自己先栽了一次，值得记下来：
> 第一版探针调的是裸 `cmake`，而 `cmake` **不在那个子进程的 PATH 上** ——
> 命令**压根没跑**，而 `$LASTEXITCODE` 停在 0，
> 探针于是报了一句 **"build OK"**。
>
> **一个没有真的跑东西的探针，报绿比没有探针更坏。**
> 修法两条：先把工具**找出来**（找不到 = 硬失败），
> 以及**验产物在不在**（exe 没生成 = 失败，不看退出码）。
> 跟 memory 里那条「**验证不等于验证**」是同一条。

### 修法

`.gitignore` 里本来就写着这条约定：

```
# ===== 大型二进制占位（datasets/images 保留目录） =====
# (datasets/ 和 images/ 目录本身用 .gitkeep 占位,实际数据不入 git)
```

根目录的 `images/.gitkeep` 和 `datasets/.gitkeep` **都在 git 里** ——
**唯独 W1–W4 这四个漏了**。

补上四个 `.gitkeep`：

```
W1_ImageIO/images/.gitkeep
W2_ColorFilter/images/.gitkeep
W3_EdgeContour/images/.gitkeep
W4_TemplateFeature/images/.gitkeep
```

**为什么这不只是为了让 build 过** ——
W2/W3/W4 的 `main.cpp` 在图不存在时会**自动生成**一张：

```cpp
const std::string imgPath = "images/pcb_test.jpg";
cv::Mat img = cv::imread(imgPath, cv::IMREAD_COLOR);
if (img.empty()) {
    img = generatePcbLikeImage();
    cv::imwrite(imgPath, img);        // ← 目录不存在,这一步也失败
}
```

**`imwrite` 不会替你建目录。** 所以 `images/` 不存在 ⇒
「自动生成一张测试图」这个**兜底路径本身也是坏的**。
`.gitkeep` 让目录**存在**（哪怕是空的）⇒ 兜底路径成立。

W5 的 `images/` 里有**已跟踪**的 png（`qrcode_test.png` / `ocr_test.png`），
克隆里本来就在 ⇒ **不用改**。

---

## 三、第二次真跑逮到的 Bug 2：Qt 的 DLL 不在 PATH 上

### 这件本地**也**看不见，但机制不一样

W7 链接 `Qt6::Core / Gui / Widgets`，而**七个项目里没有一个拷 Qt 的 DLL** ——
只有 OpenCV 的 DLL 被拷了。

```
W1:  copy_directory ${OpenCV_BIN_DIR}  → exe 目录     ✅
Qt:  （没有这一步）                              ❌
```

于是 exe 找 `Qt6Core.dll` 只能走 Windows 的老规矩：**PATH**。

| | Qt 装在哪 | `bin` 在 PATH 上吗 |
|---|---|---|
| 本机 | `D:\Qt\6.8.3\msvc2022_64` | ✅ **在** |
| runner | `D:\a\_temp\Qt\6.8.3\msvc2022_64` | ❌ **不在** |

> 又是同一族：**本机成立，runner 不成立。**
> 只不过这次连本地彩排都救不了 ——
> 彩排跑在**同一个 shell** 里，而那个 shell 的 PATH 恰好带着 Qt。

### 修法：PATH 交给**拿到路径的那份脚本**

`ci.ps1` 已经收了 `-QtRoot` 参数（构建时就是用它找头文件和库的）。
所以**同一个值**顺手也用来找运行期的 DLL：

```powershell
$qtBin = Join-Path $QtRoot 'bin'
if (-not (Test-Path $qtBin)) { Fail (...) }
if ($pathEntries -notcontains $qtBin) {
    $env:PATH = "$qtBin;" + ($pathEntries -join ';')
}
```

**一个来源，不是两个。**

### 顺带：tesseract 的 PATH 也在搬家

原来 workflow 的 YAML 里有一段：

```yaml
if ($env:TESSERACT_EXE) {
    $tess = Split-Path -Parent $env:TESSERACT_EXE
    $env:PATH = "$tess;$env:PATH"
}
```

那是**同一件事的第二份副本** ——
后果是**本地彩排根本没做这件事**（YAML 里的东西，本机跑 `ci.ps1` 时不存在）。

现在这段搬进 `ci.ps1`（由 `TESSERACT_EXE` 驱动），
YAML 里那份**删掉**。

> 副本的害处不一定是「值会漂」，
> 还可以是「**只有一边在做**」——
> 那一边的机器恰好有，于是**永远没人发现**。

---

## 四、第三次：门禁的数字对不上（这一条是**自己**抓到的）

第一次真跑之前，本机门禁 `tools/ci.ps1` **就已经是红的** ——
因为这一笔新加了两张 atomic 卡，而门禁数的是**工作区**：

```
measured (tree) : 28 notes + 73 atomic cards / 954 KB
FAIL: root README: notes volume [README.md]: group 2 is 71, reality says 73;
FAIL: close report: number table (notes): group 2 is 944, reality says 954
```

**这是门禁在干活** ——
它发现「文档里写的卡数」和「真的卡数」对不上。
如果这次没跑门禁，这 3 个数字会**带着错推到 GitHub 上**。

> 也就是说：**这一笔的文档数字，是被机器逼出来的，不是被谁记得的。**
> 这正是欠账 11 那半步（机器可查）存在的意义。

---

## 五、第四次真跑逮到的 Bug 4：同一份代码,两台机器判成两回事

### 现象

run `40293214498`：build 过了，`Run CI` 挂在 W6 的 `w6_cli` 套件。

```
[FAIL] 测试抛出异常:No mapping for the Unicode character exists in the target multi-byte code page.
       (case: 配方目录:能找到,且不会因为一个坏目录就整个废掉)
SUMMARY: pass=20 fail=1   RESULT=FAIL
```

同一份代码，本机 `tools/ci.ps1` **全绿**。

### 根因：`.string()` 是**严格**窄化，而严格的标准是**这台机器的 ACP**

炸的是 HEAD 里的第 273 行：

```cpp
const std::string d = w6::findRecipeDir((g_w6 / u8p("根本没有这个目录")).string());
```

- `u8p()` 走 `fs::u8path` ⇒ 路径里是**真的中文宽字符**；
- `.string()` 把它按 **ANSI 代码页（ACP）** 窄化成窄串 —— 而且是**严格**的：
  **字符在 ACP 里表示不出来就抛 1113**（`ERROR_NO_UNICODE_TRANSLATION`）。

| 机器 | ACP | 中文在 ACP 里？ | `.string()` |
|---|---|---|---|
| 本机（中文 Windows） | **936**（GBK） | **有** | 转得出 → **绿** |
| GitHub runner（windows-2022） | **1252**（Latin-1） | **没有** | **抛 1113** → 红 |

⇒ 差别**不在代码**，在**这台机器的 ACP**。同一行代码、同一份数据、两个结果。

### 证据：本机就能把 runner 那个失败**原样**复现

规则是「字符在 ACP 里表示不出来就抛」。
本机 ACP=936（GBK）**有**中文 ⇒ 中文不炸；
但 GBK **没有** emoji（U+1F600）⇒ 拿 emoji 当路径，本机就抛**一模一样的 1113、一模一样的消息**：

```
ASCII 路径       : OK len=11
中文路径(GBK有) : OK len=16
emoji  (GBK没有) : THREW: No mapping for the Unicode character exists in the target multi-byte code page.
```

⇒ **规则**是同一条，机器只决定**哪个字符**踩雷。
（可复现的探针：`tools/probe-acp.cpp`；它的输出块是**粘贴真实输出**的。）

⚠ 而且这种机器事实**没法在进程里造出来**：
试过用 `_setmbcp(1252)` 去改 CRT 的多字节代码页 —— `path::string()` 的行为**一点没变**。
⇒ 「本机能过」证明不了「runner 能过」；这种事只能**去 runner 跑**。

### ⚠ 另一半坑方向是**反的** —— 我自己第一版就搞混了

| 方向 | 写法 | 结果 |
|---|---|---|
| 宽 → 窄 | `path::string()` | **严格**：转不了就**抛** ⇒ 本机绿 / runner 红 |
| 窄 → 宽 | `fs::path(窄串)` | **宽松**：转不了就拆字节/换字符 ⇒ **每台机器都错**，而且一声不吭 |

我第一版把这两条混成一句「窄串当路径 = 本机绿、runner 红」，**还照着它改了 `utf8_path.h` 的注释**。
是 runner 日志里那句「**target** multi-byte code page」把它掰回来的 ——
「target」= 往**窄**里转 ⇒ 炸的是**宽→窄**那一侧。

> 教训：**推演出来的机制，必须拿原始日志对一遍。**
>
> 我那版探针的注释也是推的，跑起来跟写的**两处对不上**：
> `WC_ERR_INVALID_CHARS` 在 936/1252 上是 **1004**（`ERROR_INVALID_FLAGS`）不是 1113；
> `std::cout << const wchar_t*` 打的是**指针地址**不是字符串。
> ⇒ **注释里的数字没人验证过，它可能是推演出来的。跑一遍。**

### 修法：不修这一行，修**这一类**

1. **路径一律 `std::filesystem::path`** —— 它内部是宽字符，不过任何代码页。
2. **文本 ↔ 路径的转换只留一处**：`common/utf8_path/utf8_path.h` 的 `text()` / `fromUtf8()`。
3. **窄串守卫**：路径参数只收 `fs::path`；传 `std::string` / 字面串**编译不过**
   （`static_assert`，见 `utf8_path.h` 的「路径参数不许收窄串」）。
   守卫是**验过的**：故意把一处改回裸窄串 → build 报
   `static assertion failed: path parameter takes std::filesystem::path only` → 再改回来。
   （不是「看着对」，是**咬了一口**。）
4. **argv 是 ACP** ⇒ `fs::path(argv[i])`，**不要**再套 `fromUtf8()` —— 那是把对的搞坏。

---

## 六、这一笔的账

| 项 | 数 |
|---|---|
| 欠账 | **19 条**（清单编号 1–19，还债不改总数，只改拆分） |
| 已收 | 13 → **14**（欠账 9 结清，编号 9 从「未收」挪进「已收」） |
| 未收 | 6 → **5**（剩 2 / 8 / 10 有意不还 + 15 / 16 需真 SDK） |

欠账 9 那一行的原文：

| # | 一句话 | 边界 | 重开条件 |
|---|---|---|---|
| ~~9~~ | ~~CI 没在真 runner 上跑过~~ | —— | **已还（2026-10-26）** |

---

## 七、教训

推论一：

> **「本机能过」和「仓库能过」是两件事。**
>
> 而**只有干净克隆**才问第二个问题。
>
> ⇒ 「本地过了」这句话，**永远只覆盖第一个**。
> 要覆盖第二个，只有两条路：
> **推上去让真环境说**，或者
> **在本机 clone 一份再说**（本笔用的就是这第二条 —— 便宜、快、且能定位）。

推论二：

> **git 不带空目录 —— 所以「空目录」是 git 之外的一个事实。**
>
> 凡是构建步骤**读**某个目录，
> 那个目录要么**有文件**（git 会带），要么**有 `.gitkeep`**（人为带）。
> 第三种状态（「本机有、仓库没有」）**在构建里就是一颗雷**。

推论三：

> **本机 PATH 里有什么，是本机的事实。**
>
> 凡是 exe 在**运行期**要按名字找的东西（`Qt6Core.dll`、`tesseract.exe`），
> 那个目录要么**在 PATH 上**（由脚本从**同一个来源**加进去），
> 要么**被拷到 exe 旁边**。
> 「我机器上装了」不是这两者中的任何一个。

推论四：

> **探针自己也得先被验。**
>
> 「报绿」和「真的跑过并判定为绿」在日志上一样。
> ⇒ 探针要能回答「**我这一步到底执行了吗**」：
> 工具找不到 = 硬失败，产物不在 = 失败（不看退出码）。

推论五：

> **机器事实改不动，但它的「规则」可以在本机证成。**
>
> ACP 是机器事实：`_setmbcp(1252)` 试过了，进程里改不动，
> 所以「本机模拟一下 runner」这条路是**堵死的**。
>
> 但**规则**能证成：「字符在 ACP 里表示不出来就抛」。
> 换一个**本机 ACP 也没有**的字符（emoji），本机就抛出**同一条消息**。
>
> ⇒ **「本机复现不了」不等于「查不清」。**
> 换**触发条件**，规则就现形了 —— 机器只决定哪个字符踩雷。

推论六：

> **查错方向，比查错本身更贵。**
>
> 我把「窄→宽」和「宽→窄」混成一句，照着错的版本改了注释、改了 API 形状，
> 还差点把**病根留在原地**。
>
> ⇒ 一个方向的失败，**不能**推广到另一个方向：
> 必须分别**各测一次**，得出两条**各自**的结论。
> 混成一句的那一刻，两条都不可信了。

---

## 结局：四件事修完，再推一次，绿了

| run | 结果 |
|---|---|
| `40293214498`（第一次真跑）| ❌ W6 的 `w6_cli` 红 1 条 —— 就是本文那件 ACP 的事 |
| `40315236950`（四件事修完后再推）| ✅ **绿**：4 步全 `success`，整条 job 8m0s |

runner 上 `tools/ci.ps1` **自己**打的 summary（原文）：

```
== summary
   built   : 7 projects (Release)
   tested  : 29 ctest suites (W6 + W7)
   asserts : 2853 assertions (counted from this build)
   docs    : every number quoted in the docs matches the build
   smoke   : W5 qr --no-gui + W7 headless pipeline
   clean   : tests added nothing to the source tree

ALL CHECKS PASSED
```

**和本机一个字不差** ——
而且 `2853` 是 runner **自己数出来的**，不是抄本机的。

⇒ 「本机能过」和「仓库能过」真正对上，是**这个样子**：
不是「两边都绿」，是**两边的数对得上**。

⚠ 顺带记一笔：**依赖准备那三步**（choco 装 tesseract、aqt 装 Qt、
官方 OpenCV world 包）**也是第一次在真机器上执行** —— 它们也过了：

```
[ci-deps] tesseract installed: C:\Program Files\Tesseract-OCR\tesseract.exe
[ci-deps] QT_ROOT=D:\a\_temp\Qt\6.8.3\msvc2022_64
[ci-deps] opencv libs  : D:\a\_temp\opencv\build\x64\vc16\lib  [opencv_world4130.lib]
```

⇒ 欠账 9 到这里结清（台账的唯一登记处：
`notes/notes_W7_Knob_Audit_2026-10.md` 第十节）。

---

## 参见

- `notes/notes_CI_2026-10.md` —— CI 这条线的完整记录（第四章：依赖准备；**第十章：第一次在真 runner 上跑**）
- `notes/notes_W7_Knob_Audit_2026-10.md` §十 —— 欠账台账（欠账 9 在此结清）
- `tools/probe-acp.cpp` —— ACP 探针（可复现；输出块是粘贴的真实输出）
- `common/utf8_path/utf8_path.h` —— 路径 ↔ UTF-8 文本的**唯一**一处定义 + 窄串守卫
- `notes/atomic/at-a-rehearsal-can-pass-because-it-was-skipped.md` —— 被跳过的那段代码 = 还没写过的代码
- `notes/atomic/at-the-detector-must-not-die-on-what-it-detects.md` —— 检测器不能被它要检测的东西杀死
- `notes/atomic/at-gitignore-hides-files-not-problems.md` —— ignore 藏的是文件，不是问题
- `notes/atomic/at-no-runner-is-not-a-reason.md` —— 「没跑过」不算理由
- `notes/atomic/at-the-narrow-string-must-not-carry-a-path.md` —— 窄串不能扛路径（本次新增）
