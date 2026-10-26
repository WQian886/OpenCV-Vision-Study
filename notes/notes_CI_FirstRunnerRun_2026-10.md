# CI 第一次在真 runner 上跑（2026-10-26）：欠账 9 的重开条件兑现

> 这一章记的是**欠账 9**（`notes/notes_W7_Knob_Audit_2026-10.md` §十）兑现的那一刻。
> 它的重开条件原文是「**仓库有 remote 的那一天**」。
> 今天 `gh` 已登录、网也通 → 仓库建了 → 这条路开了。
>
> 但**这一章真正记的不是「CI 跑起来了」**，
> 是**第一次真跑就挖出来的三件事** ——
> 三件都**不是代码写错了**，是「**只有这台机器成立**」的东西。
> 其中**两件本地永远看不见**。

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

## 五、这一笔的账

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

## 六、教训

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

---

## 参见

- `notes/notes_CI_2026-10.md` —— CI 这条线的完整记录（第四章：依赖准备；第十章：一句话带走）
- `notes/notes_W7_Knob_Audit_2026-10.md` §十 —— 欠账台账（欠账 9 在此结清）
- `notes/atomic/at-a-rehearsal-can-pass-because-it-was-skipped.md` —— 被跳过的那段代码 = 还没写过的代码
- `notes/atomic/at-the-detector-must-not-die-on-what-it-detects.md` —— 检测器不能被它要检测的东西杀死
- `notes/atomic/at-gitignore-hides-files-not-problems.md` —— ignore 藏的是文件，不是问题
- `notes/atomic/at-no-runner-is-not-a-reason.md` —— 「没跑过」不算理由
