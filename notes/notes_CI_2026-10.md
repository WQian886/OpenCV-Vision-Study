# 上 CI：路径不能写死，以及 CI 第一次跑就逮到的两个真 bug（2026-10-20）

> 📅 **记录时间**：2026-10-20
> 📁 **项目来源**：common/cmake/ThirdParty.cmake / tools/ci.ps1 / .github/workflows/ci.yml / 7 个工程的 CMakeLists.txt
> 🔬 **核实方式**：本篇所有技术论断均于 2026-10-20 当天编译运行 / 读源码确认，核实清单见文末
> 🎯 **本次目标**：让「构建 + 跑测试 + 检查源码树没被写脏」变成一条能重复执行的命令，并接上 GitHub Actions

---

## ⚠️ 使用前必读：本文的证据等级

| 标记 | 含义 | 能否当事实引用 |
|---|---|---|
| 【实测】 | 当天编译运行 / 读源码确认，有具体输出 | ✅ 可以 |
| 【实测·项目】 | 在本项目实测成立，换项目需重新验证 | ⚠️ 需验证前提 |
| 【未验证】 | 写完了但**没在真环境跑过** | ❌ 不要当事实 |
| 【推演】 | 基于实测数据的合理推断，未单独构造用例 | ❌ 不要当事实 |

**本篇没有"听说""应该""一般来说"这类表述。**

---

## 📌 一句话总结

上 CI 的第一步不是写 YAML，是**把 28 处写死的本机路径变成能覆盖的变量** ——
而在这一步之后，CI 第一次跑就逮到了**两个真 bug**：
两个都出在"测试靠猜来定位自己"这件事上，
而这两个 bug 在本地**永远测不出来**，因为本地的目录布局长得不像仓库根。

---

## 一、上 CI 之前卡在哪：路径是**这台机器**的事实

### 1.1 【实测】改动前的硬编码清单

7 个 `CMakeLists.txt` 里一共 **28 处**写死了本机路径：

| 写法 | 处数 | 出现在 |
|---|---|---|
| `D:/opencv-src/build/install/bin`（拷 OpenCV DLL 用）| **20** | 全部 7 个工程 |
| `set(OpenCV_DIR "D:/opencv-src/build/install/lib")` | **7** | 全部 7 个工程 |
| `set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")` | **1** | 只有 W7 |

本机能跑，别人跑不了 —— 因为**路径是这台机器的事实，不是项目的事实**。
一旦要上 CI，这些行就得逐个改，而且改的时候**没人会注意到漏掉的那一个**。

### 1.2 【实测】改法：CMake 缓存变量

新增 `common/cmake/ThirdParty.cmake`，7 个工程都 `include()` 它：

```cmake
if(NOT DEFINED OpenCV_DIR)
    set(OpenCV_DIR "D:/opencv-src/build/install/lib"
        CACHE PATH "OpenCV 安装目录下的 lib 子目录(不是安装根目录)")
endif()

get_filename_component(_ocv_prefix "${OpenCV_DIR}" DIRECTORY)
if(NOT DEFINED OpenCV_BIN_DIR)
    set(OpenCV_BIN_DIR "${_ocv_prefix}/bin"
        CACHE PATH "OpenCV 安装目录下的 bin 子目录(拷 DLL 用)")
endif()

if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
endif()
```

**为什么必须是 `CACHE` 而不是普通变量** —— 这是本项目栽过的那类坑的另一个形状：

CMake 里**普通变量会盖掉缓存变量**。所以

```cmake
set(OpenCV_DIR "D:/...")            # 普通变量
# cmake -DOpenCV_DIR=C:/opencv/lib   # 命令行传的值在缓存里
```

这一行会把 `-D` 传进来的值**盖掉** ——「`-D` 覆盖」看着写了、其实没生效。
缓存变量不会：命令行 `-D` 优先级最高，`set(CACHE)` 不带 `FORCE` 时也不覆盖已有缓存项。

`CMAKE_PREFIX_PATH` 那行**故意不用 CACHE**：它会被环境变量初始化成一个缓存项，
用 `set(CACHE)` 反而可能跟环境变量打架。

### 1.3 【实测】证明 `-D` 覆盖真的生效

光看代码不算数，所以做了个**联接**（junction）实测：

```powershell
cmd /c 'mklink /J "E:\qianwie\ocv_link" "D:\opencv-src\build\install"'
cmake -S W1_ImageIO -B build_ci_test -DOpenCV_DIR="E:/qianwie/ocv_link/lib"
```

全新构建目录里的 `CMakeCache.txt`：

```
OpenCV_DIR:UNINITIALIZED=E:/qianwie/ocv_link/lib     ← -D 传的值,没被盖掉
OpenCV_BIN_DIR:PATH=E:/qianwie/ocv_link/bin          ← 从 OpenCV_DIR 退一级推出来
```

`UNINITIALIZED` 这个类型本身就说明它是**命令行来的**（`set(CACHE)` 建的会带类型和说明）。
编译通过，14 个 OpenCV DLL 都被拷到 exe 旁边 ✅。

> ⚠ 顺带一个已知副作用：缓存变量是**粘**的 —— 第一次 configure 之后，
> 再改这个文件里的默认值不生效了（缓存里存着旧值）。
> 要换路径就 `-DOpenCV_DIR=<新的>` 或删掉 build 目录重来。

---

## 二、CI 脚本 `tools/ci.ps1`（**已验证**）

### 2.1 为什么是脚本而不是一堆 YAML

**只存在于 YAML 里的 CI，等于没人能在 push 之前跑一遍的 CI** ——
于是它的第一次真实执行，同时就是它的第一次调试。
所以真正干活的步骤放在 `tools/ci.ps1` 里，
`.github/workflows/ci.yml` 只负责**准备机器**，然后调用同一个脚本。

### 2.2 五步

| 步 | 做什么 | 失败时 |
|---|---|---|
| 1 | 预检：cmake / ctest / OpenCV / Qt 都在吗 | **当场失败**，不跳过、不猜 |
| 2 | 7 个工程全部 configure + build（Release）| 打印失败的那条命令 |
| 3 | W6 3 套件 + W7 14 套件 | `ctest --output-on-failure` |
| 4 | W5 冒烟：跑 `qr --no-gui`，**检查输出里真的有串号** | 见 2.4 |
| 5 | 源码树**没有被测试改动** | 列出新增了哪些文件 |

### 2.3 【实测】最终一次跑通的输出

```
== Step 1/5  preflight
   cmake : D:/Program Files/Microsoft Visual Studio/18/Community/.../cmake.exe
   ctest : .../ctest.exe
   opencv: D:/opencv-src/build/install/lib
   qt   : D:/Qt/6.8.3/msvc2022_64
   working tree before: 15 uncommitted entr(y/ies)

== Step 2/5  configure + build (Release)
-- W1_ImageIO ... -- W7_VisionMasterLite        (7 个全部)

== Step 3/5  test suites
-- W6_PCB_DefectProject
100% tests passed, 0 tests failed out of 3
-- W7_VisionMasterLite
100% tests passed, 0 tests failed out of 14

== Step 4/5  W5 smoke check
   W5 decoded a serial number: OK

== Step 5/5  source tree must be clean
   source tree unchanged by the tests: OK

== summary
ALL CHECKS PASSED
```

### 2.4 W5 的冒烟为什么不能只看退出码

W5 的 `main()` 在"图读不到"和"没识别到二维码"这两种情况下**都返回 0**：

```cpp
if (img.empty()) { std::cerr << "❌ 找不到 " << imgPath << "\n"; return; }   // → 退出码 0
if (!qr.found)  { std::cout << "  ❌ 没识别到二维码\n"; }                      // → 退出码 0
```

所以只查退出码，等于**什么都没查**。脚本改成查输出里**有没有串号**：

```powershell
if ($w5text -notmatch 'PCB-\d{4}-\d{3}-SN:[A-Z0-9]+') { ... Fail ... }
```

注意这里匹配的是**形状**（`PCB-2026-001-SN:A12345X` 这种格式），**不是值本身** ——
值只有一处（`common/qr_ocr/tests/test_qr_ocr.cpp` 里那条断言）。
在 CI 脚本里再抄一份值，就是又一次"抄副本"。

### 2.5 【实测】两个 PowerShell 的坑

**坑 1：`$ErrorActionPreference='Stop'` + native 命令写 stderr = 终止错误。**

第一次跑就撞上：

```
powershell : ctest.exe : Errors while running CTest
所在位置 ... At E:\qianwie\00study\OpenCV\tools\ci.ps1:139 char:17
    $out = (& $exe @argv 2>&1 | Out-String)
```

Windows PowerShell 5.1 会把 native 命令的 stderr 当成错误记录，
在 `Stop` 下直接变成**终止错误**。而 MSBuild / ctest 天天往 stderr 写东西
（警告，以及套件失败时的 `Errors while running CTest`）。

修法：调 native 命令时把 `$ErrorActionPreference` 临时设成 `'Continue'`，
**真正的信号是退出码**，退出码自己判。

**坑 2：检查源码树"干净"的语义一开始写错了。**

第一版是"跑完之后 `git status --porcelain` 必须是空" ——
结果它把**我自己还没提交的改动**判成了泄漏：

```
The source tree is NOT clean after running the tests:
 M README.md
 M W1_ImageIO/CMakeLists.txt
 ...
```

一个开发者有未提交的工作时，就会收到一个**跟他完全无关的红**。
正确语义是「**测试没有新增任何东西**」：跑之前拍一次快照，跑之后拍一次，
只比**新增**的那部分。

### 2.6 脚本必须是纯 ASCII

Windows PowerShell 5.1 读**没有 BOM** 的 `.ps1` 时按 ANSI 解析 ——
文件里任何一个非 ASCII 字节都会变成乱码，甚至语法错。
所以 `tools/ci.ps1` 里的注释和消息**全是英文**，中文解释放在 `notes/` 里
（那是 UTF-8，有 BOM 也没有问题）。

---

## 三、GitHub Actions workflow（依赖准备那几步：**2026-10-22 已在本机验完**，见第四章）

`.github/workflows/ci.yml`：

| 步骤 | 做什么 | 验证状态 |
|---|---|---|
| `actions/checkout` | 拉代码 | ✅ 标准动作 |
| `tools/ci-deps.ps1` | 装 tesseract / Qt 6.8.3 / OpenCV 4.13.0 | ✅ **命令本身已本机实测**；✅ **整套 CI 也对着官方包跑通了**；❌ 只剩 runner 特有那三条（见 4.7）|
| `tools/ci.ps1` | 真正干活的那五步 | ✅ **本机跑通 4 次** |

### 3.1 为什么 OpenCV 那步是「发现」而不是「写死路径」

⚠ **本节写于 2026-10-20；第四章（2026-10-22）把它推翻了** ——
下面这段发现逻辑**假设的形状压根不存在**，真跑起来会直接 throw。
留在这里不是为了正确，是为了记住**当时为什么会那样想**。

本机试过下载官方包，**网络超时**（`操作超时`），所以布局没法在这里实测。
于是这一步改成一个**不依赖记忆**的发现过程：

```powershell
# 找"既有 config、又有 import lib"的那个目录
foreach ($c in (Get-ChildItem $pkg -Recurse -Filter 'OpenCVConfig.cmake' -File)) {
    $libCandidate = Join-Path $c.DirectoryName 'lib'
    if (Test-Path $libCandidate) {
        $libs = Get-ChildItem $libCandidate -Filter 'opencv_core*.lib' -File -ErrorAction SilentlyContinue
        if ($libs) { $cfgDir = $c.DirectoryName; break }
    }
}
# 找 DLL 目录
$dll = Get-ChildItem $pkg -Recurse -Filter 'opencv_core*.dll' -File | Select-Object -First 1
```

猜布局是 CI 变成"cannot open opencv_core4130.lib"、
而没人知道为什么的经典原因。
**发现 + 找不到就大声失败**，是把"5 分钟能修"和"5 小时能修"分开的地方。

> ⚠ 但"不依赖记忆"和"依赖事实"是两回事。
> 这一版确实不依赖记忆 —— 它依赖的是**一个从来没验证过的假设**
> （"config 旁边有个 lib/，里面是 opencv_core*.lib"）。
> **假设也是猜测的一种，只是穿着"发现"的外衣。**
> 第四章的收法是：把假设换成**唯一一条真事实** —— 「库文件在哪，DLL 就在哪」。


### 3.2 tesseract 装不上就让 job 失败

`common/qr_ocr` 的 36 条断言里，有一部分依赖 tesseract；
没装的话那些用例会**跳过**（并明确说"本机没装 tesseract"）。
一个"OCR 那条路根本没被测过"的绿色 CI，比一个红的 CI 更危险 ——
所以 workflow 里装 tesseract 失败 = job 失败，不允许静默跳过。

### 3.3 【未验证】这件事的诚实说法（2026-10-20 写下，2026-10-26 作废）

> ⚠ **这一节是当时的快照，现在过期了 —— 原文保留，因为「过期」本身就是证据。**
> 真跑结果见**第十章**。

**本地能验证的都验证了**（脚本五步、路径覆盖、两个 bug 的修复、变异），
**没法验证的是"GitHub 的 Windows runner 上，Qt/OpenCV/tesseract 能不能装上"** ——
这台机器连 GitHub 的 release 都下不动。

第一次真的 push 上去跑，大概率要改 `ci.yml` 里那三步
（aqtinstall 的参数、OpenCV 包的目录名、tesseract 的装法），
**而要改的具体是哪一行，取决于 runner 上的实际情况** ——
这一条在 workflow 顶部的注释里也写清楚了。

> **后来（2026-10-26）：**
> 三步**全都要改**，而且**不是改一行** ——
> aqtinstall 的参数、world 包的目录名、tesseract 的装法，**三处各错各的**。
> 改完真跑一次，8m0s 全绿（run `40315236950`）。

---

## 四、依赖准备那三步：本机验完了（2026-10-22）

### 4.1 起因：卡住这条路的不是"没有 runner"，是"下载超时"

第三章那三步一直挂着「❌ 未在真 runner 上跑过」。
当时写下的原因是：

> 本机试过下载官方包，**网络超时**（`操作超时`），所以布局没法在这里实测。

也就是说，**卡住的不是"缺一个 GitHub runner"，是"包没下下来"**。
2026-10-22 下载重试成功了（185.5 MB，官方 release 链接仍有效），
于是这一步第一次能**在动手写之前**看清那个包长什么样。

结果：**那三步里有三个会让 CI 第一次跑就挂的真 bug。**

### 4.2 先把「唯一一份」立起来，再验

原来的三步是 `.github/workflows/ci.yml` 里的一段内联 YAML。
内联 YAML 有一个致命的性质：**它是整个仓库里唯一一份从没执行过的代码** ——
跑不了、测不了、也改不动。

所以第一步不是去修那三步，而是把它们**搬成一个脚本**：

```
tools/ci-deps.ps1        ← 唯一一份 provisioning 命令
    ├─ .github/workflows/ci.yml  → 调它
    └─ 本机彩排                   → 也调它（-VerifyOnly）
```

这样"本机验的那份"和"runner 跑的那份"**是同一个文件** ——
不是"验了一份副本"，那正是本项目栽过五遍的坑（抄副本）。

### 4.3 挖出来的三个真 bug

#### ① Qt 的架构参数写错了（**必挂**）

工作流里写的是 `aqt install-qt windows desktop 6.8.3 msvc2022_64`。
实测：

```
$ python -m aqt install-qt windows desktop 6.8.3 msvc2022_64 --dry-run
ERROR : The packages ['qt_base'] were not found while parsing XML of package information!

$ python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --dry-run
INFO  : DRY RUN: Would download and install the following:
INFO  :   - qtbase: ... qt6.683.win64_msvc2022_64 ...
```

`msvc2022_64` **不是别名**，真实名字是 `win64_msvc2022_64`。
`aqt list-qt windows desktop --arch 6.8.3` 列出来的也只有 `win64_*` 那一套。

> ⚠ 这个 bug 的性质值得记一笔：**它不会"下载失败"，它会"下载之前就报错"** ——
> 看起来像网络问题，实际是参数写错。差一步就会去查网络。

#### ② OpenCV 的库名假设错了（**必挂**）

工作流里找的是 `opencv_core*.lib` / `opencv_core*.dll`。
**官方预编译包是 world 构建**：

```
opencv/build/x64/vc16/lib/opencv_world4130.lib     ← 只有这一个库
opencv/build/x64/vc16/bin/opencv_world4130.dll     ← 只有这一个 DLL
```

整个包里**一个 `opencv_core4130.*` 都没有**（6 个 DLL 里三个是 java 绑定）。
于是：

- 找 `opencv_core*.lib` → 找不到 → 「config + lib」那个候选永远选不中
- 找 `opencv_core*.dll` → 找不到 → **直接 `throw`** → job 挂在那儿

本机用的是**模块化**构建（`opencv_core4130.lib` 一堆，没有 world），
所以本地怎么测都不会暴露这个错 —— **两份 OpenCV 的"形状"不一样**。

#### ③ OpenCV 的目录形状假设错了（**不挂，但会挑错**）

工作流找的是「config 所在目录**旁边**有个 `lib/`」：

```powershell
$libCandidate = Join-Path $c.DirectoryName 'lib'
```

而实际布局是 **config 和 import lib 在同一个目录**：

```
opencv/build/x64/vc16/lib/OpenCVConfig.cmake
opencv/build/x64/vc16/lib/opencv_world4130.lib
```

所以 `<config>/lib/` 从来不存在 → 那个判据**从来没成立过**，
每次都掉进"退而求其次"分支，随机挑一个 config 文件。

### 4.4 收法：一条不认模块名的判据

新脚本里的发现逻辑**一个模块名都不提**：

1. 找 `opencv_*.lib`（排除 java 目录）→ 它的**目录**就是库目录；
2. 库目录里（或其上层）找 `OpenCVConfig.cmake` → 就是 config 目录；
3. **DLL 的名字由库名推出来**（`opencv_world4130.lib` → `opencv_world4130.dll`），
   找到它的目录就是 bin 目录。

第 3 条是关键：**一条规则同时吃两种形状** ——
world 构建和模块化构建都不用改。

⚠ 挑 config 时特意**不**选顶层那个包装 config（`opencv/build/OpenCVConfig.cmake`）：
它自己要按编译器版本去推 `x64/vc16`，而本机是 MSVC 19.51（VS 2026），
那个包装 config 的正则**认不出这个版本**（`ci.ps1` 的报错里写过这件事）。
内层那个 config 靠 `../../../` 算回 `opencv/build`，不认编译器 → 最稳。

### 4.5 本机彩排（六次，全部实测）

| 彩排 | 验的是 | 结果 |
|---|---|---|
| A | 官方 **world** 包（已解包）| config=`…/x64/vc16/lib`、lib=`opencv_world4130.lib`、dll=`…/x64/vc16/bin` ✅ |
| B | 本机**模块化**安装（`D:\opencv-src\build\install`）| config=`…/lib`、lib=`opencv_calib3d4130.lib`、dll=`…/bin` ✅ |
| C | tesseract 搜索 | 找到 `D:\Program Files\Tesseract-OCR\tesseract.exe` ✅ |
| D | Qt 包名 | `--dry-run` 解析出 qtbase 等 ✅（不下载 2 GB）|
| **E** | **整套 CI 对着官方 world 包跑** | **`ALL CHECKS PASSED`** ✅ —— 见 4.5.1 |
| **F** | **整份 `ci.yml` 在本机跑**（解析 YAML 执行它的 `run:` 块）| **`ALL WORKFLOW STEPS PASSED`** ✅ —— 见 4.5.3 |

外加一次**完整路径**彩排：下载好的 `.exe` → 解压（走 bsdtar 兜底）→ 发现 → 断言 ✅。

⚠ **tesseract 那一步是"读包验的"，不是"跑出来的"**：
本机没有 choco，所以没法本地跑 `choco install tesseract`。
但那个 nupkg 可以直接下载下来读它的 `chocolateyinstall.ps1`：

```powershell
Install-ChocolateyInstallPackage @packageArgs   # /S 静默装 UB Mannheim 的安装器
Install-ChocolateyPath $env:ProgramFiles\Tesseract-OCR
```

→ 装到 `C:\Program Files\Tesseract-OCR`，并且被加进 PATH。
**工作流断言的那个路径是对的**（之前只是"看起来对"，现在是查过包的）。

### 4.5.1 彩排 E：把**整个 CI** 对着官方 world 包跑一遍

前面几次彩排验的都是"找得对不对"。这一条验的是**"找到之后，货色到底能不能用"**：

```powershell
powershell -ExecutionPolicy Bypass -File tools/ci.ps1 `
    -OpenCvLib '…/opencv/build/x64/vc16/lib' `
    -OpenCvBin '…/opencv/build/x64/vc16/bin' `
    -QtRoot    'D:/Qt/6.8.3/msvc2022_64' `
    -BuildRoot 'build/ci_world'
```

结果：**`ALL CHECKS PASSED`**

| 步骤 | 结果 |
|---|---|
| W6 三个套件 | **3/3 通过** |
| W7 十五个套件 | **15/15 通过** |
| W5 冒烟（真解出一个串号）| ✅ |
| 源码树零污染 | ✅ |

而且测试 exe 旁边躺着的确实是 `opencv_world4130.dll`（**world 构建**的那个，不是本机模块化的那一堆）。

> **这一条把 4.7 里原本最没底的那格填上了。**
> 之前担心的是：官方包只给一个合并库，而项目链接的是 `${OpenCV_LIBS}`
> （一串模块名）—— 这个映射到底能不能过，**只有真链接一次才知道**。
> 现在知道了：**能过**。
>
> ⚠ 而且我验的是**更难的那个方向**：本机编译器是 MSVC 19.51（VS 2026），
> 官方包是 v142 构建的 —— **新编译器链旧库**。
> runner 上是 VS 2022（v143）链 v142，那是**常规方向**，更稳。

### 4.5.2 顺带查清的：runner 镜像上到底有什么

不用 runner 也能查 —— GitHub 官方把每张镜像装了什么写成了一份清单
（`actions/runner-images` 仓库的 `Windows2022-Readme.md`）。核对 `windows-2022`：

| provisioning 依赖的东西 | 镜像上有没有 | 备注 |
|---|---|---|
| `choco install tesseract` | ✅ **Chocolatey 2.7.4** | |
| 解压 SFX | ✅ **7zip 26.04** | 脚本里 7z / bsdtar 两条路都有 |
| `python -m pip install aqtinstall` | ✅ **Python 3.12.10**（和本机同版本）| pip 26.2.1 |
| C++ 编译器 | ✅ **Visual Studio Enterprise 2022 17.14** | v143 |

> 这不是"猜 runner 上有什么"，是**查镜像清单**。
> 剩下的真不确定项又少了一条。

### 4.5.3 彩排 F：把**整份 YAML** 在本机跑一遍

前面所有彩排验的都是「**脚本**」。但 `ci.yml` 里还有一层没验过的东西：
**编排** —— 哪条命令在什么时候跑、上一步的变量怎么传到下一步。

以前这一步只能靠推上 GitHub 才知道。而 GitHub Actions 的工作流是 **YAML**，
YAML 不是能在自己机器上执行的程序 —— 这就是它「只能推上去才知道」的原因。

所以新写了一个 `tools/ci-local.ps1`，它**解析 `ci.yml` 并逐条执行里面的 `run:` 块**：

| runner 的行为 | 本机模拟器怎么做的 |
|---|---|
| 每个 step 用 `shell: pwsh` 跑 | 写成临时 .ps1，用 pwsh 执行 |
| `$ErrorActionPreference='stop'` + `exit $LASTEXITCODE` | 一模一样包在脚本外面 |
| `GITHUB_ENV` 里的值传给**后续** step | 每步结束后读回来写进环境变量 |
| `GITHUB_PATH` 追加到 PATH | 同上 |
| `RUNNER_TEMP` / `GITHUB_WORKSPACE` / `CI=true` | 都按 runner 的样子设 |
| `uses: actions/checkout` | 跳过（代码就在本机）|

**关键在于它不抄任何东西** —— 它读 YAML、执行 YAML。
所以「本机跑的」和「GitHub 跑的」**是同一批命令**，不是两份会漂移的副本。

实测（pwsh 7.6.6，和 runner 镜像上的版本一致）：

```
-- STEP 1 : Provision dependencies
   [ci-deps] opencv libs  : …\vc16\lib  [opencv_world4130.lib]
   [ci-deps] opencv dlls  : …\vc16\bin  [opencv_world4130.dll]
-- STEP 2 : Run CI
   -DOpenCV_DIR=…\vc16\lib  -DOpenCV_BIN_DIR=…\vc16\bin
   -DCMAKE_PREFIX_PATH=…\Qt\6.8.3\msvc2022_64     ← 变量确实传过来了
   100% tests passed, 0 tests failed out of 3
   100% tests passed, 0 tests failed out of 15
   smoke : W5 qr --no-gui
   clean : tests added nothing to the source tree
   ALL CHECKS PASSED
ALL WORKFLOW STEPS PASSED (run locally, on this machine)
```

> **这一步验到的是"管道"**：`GITHUB_ENV` 那一段是**最容易静默失败**的地方 ——
> 变量没传过去，ci.ps1 收到的就是空串，报错是
> "OpenCVConfig.cmake not found under: "，而真正的原因是三步之前。
> 这种错只有真跑一遍编排才看得见。

⚠ 三处**本机与 runner 的已知差异**（都记在这儿，免得下一个人以为完全等价）：

1. **shell**：本机没有 pwsh 7 的话，脚本会**直接报错**，不会退回 5.1 ——
   5.1 和 7 的编码默认值、错误语义都不同，跑了也不算同一个实验。
   （本机这次是拉了一份官方便携版 pwsh 7.6.6，版本和 runner 镜像一致。）
2. **Qt 那一步**：本机用 junction 把已有的 6.8.3 摆成 aqt 的目录布局，
   模拟"已装好"的状态，省掉 2 GB 下载。
   真的 aqt 安装动作**只用 `--dry-run` 验过包名解析**（彩排 D）。
3. **tesseract**：本机本来就装了（在 D 盘），所以走的是"已存在"那条路，
   **没有**跑过 choco 的安装动作。

### 4.5.4 那三条"纯环境性的"，现在还剩几条

彩排 F 之后，剩下的又少了一条：

| 项 | 彩排 F 之后 |
|---|---|
| 三步的命令本身 | ✅ 验过（彩排 A~D） |
| 官方包能不能编译链接 | ✅ 验过（彩排 E） |
| runner 镜像上有没有那些工具 | ✅ 查过官方镜像清单 |
| **workflow 的编排、变量交接** | ✅ **验过**（彩排 F）|
| choco 装 tesseract 的**动作** | ❌ 本机没 choco |
| GitHub 的**调度器 / runner 镜像 / 网络** | ❌ 环境本身 |

### 4.5.5 顺带查清的：「W7 必须用 CMake 4.3.1」的真实原因

本机一直有条规矩：W7 必须用 VS 自带的 CMake 4.3.1，Qt 那套 3.30.5 不行。
这次顺手验了它到底为什么：

```
$ cmake-3.30.5 -S W7_VisionMasterLite -B ...
CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
```

**不是 W7 要 CMake 4.x**（七个工程的 `cmake_minimum_required` 全是 3.21），
是 **CMake 3.30 不认识 VS 2026**（它只认到 VS 17.x），
所以**发现不到编译器**。加上 vcvars64 之后同一个 3.30.5 就能配出来：

```
-- The CXX compiler identification is MSVC 19.51.36256.0
-- Configuring done / Generating done
```

> ⚠ **所以这条不用改代码**：`windows-2022` 镜像自带的 CMake 3.31.6 配镜像自带的
> VS 2022（v143）—— **版本对得上**，是配好的一对。
> 而 `ci.ps1` 的 cmake 探测是 **PATH 优先**（`Get-Command`），
> runner 上会直接拿到镜像自带的那份，不靠那些 `D:/...` 硬编码路径撞运气。
>
> 记下来是因为：**"某工程必须用某个工具版本"这种规矩，得知道它到底为什么** ——
> 否则换个环境就变成"玄学",而真正的原因（工具认不认编译器）是可以查的。


### 4.6 顺手修的：产品代码里的机器事实

`common/qr_ocr/qr_ocr.cpp` 的 `defaultTesseractPath()` 写死了

```cpp
return "D:\\Program Files\\Tesseract-OCR\\tesseract.exe";
```

那是**这台机器**的装法；choco 装的是 `C:\Program Files\...`。
写死的后果不只是"多试一个地方"：在别的机器上，"默认位置"指向一个压根不存在的文件，
**错误消息还会把人引到那个不存在的目录去**。
（CI 上是靠 PATH 兜底才找到的 —— 那是运气，不是设计。）

现在按标准位置逐个找，找到真的那个就返回；一个都没有才返回标准位置（错误消息总得有个地方可说）。

> 这跟第三章讲的是同一件事：**路径是某一台机器的事实**。
> 上一轮把 CMakeLists 里 28 处硬编码清了，却漏了产品代码里这一处。

### 4.7 现在还剩什么

| 项 | 状态 |
|---|---|
| 三步的命令**本身** | ✅ **本机实测过**（彩排 A~D + 一次完整路径） |
| Qt 包名 | ✅ 实测（`--dry-run` 解析成功） |
| OpenCV 布局 | ✅ 实测（对着真包装的**两种**形状都验过） |
| tesseract 安装路径 | ✅ 读包验的（装到哪、PATH 怎么加） |
| 解压 | ✅ 实测（7z 与 bsdtar 两条路都试过，本机走的是 bsdtar） |
| **对着官方包真的能编译链接** | ✅ **本机实测**（彩排 E：整套 CI 对着 world 包 `ALL CHECKS PASSED`）|
| **workflow 的编排、变量交接** | ✅ **本机实测**（彩排 F：解析 `ci.yml` 执行它的 `run:` 块，`ALL WORKFLOW STEPS PASSED`）|
| **runner 镜像上有没有 choco / 7z / python / VS** | ✅ **查了官方镜像清单**（4.5.2）|
| **runner 的 cmake 配它的 VS 能不能用** | ✅ 版本对得上（4.5.5）|
| **choco 装 tesseract 这一步本身** | ✅ **真 runner 验过**（2026-10-26，run `40315236950`：21 秒装完，路径与包内脚本一致）|
| **GitHub 的调度器 / runner 镜像 / 网络** | ✅ **真 runner 验过**（整条 job 8m0s 全绿）|

> ⚠ 这一节原本写着「剩下那两行**为什么不能在本机消掉**」——
> 它们不是"知识缺口"，是**环境本身**——
> 只有真的在 GitHub 的机器上跑一次，才知道 GitHub 的网络给不给力、
> choco 那个安装动作会不会成功。
> 硬说"本机也能验"，那就是拿"验过命令"冒充"验过环境"——
> 那是本项目记过的第 11 条教训：**别把两件事混成一件**。
>
> ✅ **2026-10-26：这两行现在都有答案了**（第十章）。
> 补一句当时没想到的：真跑**不是**只验证了环境 ——
> 它还挖出了**四件本地看不见的东西**，
> 其中一件（ACP）在本地**永远看不见**，因为本机恰好有中文。
>
> 而且现在**整条链已经能在本机端到端跑**（彩排 F）：
> 从依赖准备到 7 个工程编译、18 个套件、W5 冒烟、源码树洁净检查，
> 跑的就是 `ci.yml` 里那几行命令。
> 推上 GitHub 剩下的只是"让同一批命令在别人的机器上再跑一遍"。

### 4.8 为什么值得先做这件事

欠账 9 的原文是「CI 没在真 runner 上跑过」。
这句话听着像"缺一个环境"，其实**缺的是证据**。
而证据的绝大部分**根本不需要 runner** —— 需要的是那个包本身。

于是顺序变成：

1. 把唯一的依赖（那个包）拿到手 ✅
2. 把命令抽成一份、让本机和 runner 跑同一份 ✅
3. 在本机把这三步**每一步**都跑一遍 ✅
4. 剩下的才是真正需要 runner 的那几条

> **"没在真环境上验过"是个结论，不该是个借口。**
> 先问一句：**这条证据真的只有在真环境上才拿得到吗？**
> 大部分时候，答案是"不是"——
> 而"不是"的部分，就是**当场就能挖出真 bug** 的那部分。

---

## 五、★ CI 第一次跑就逮到的两个真 bug

这两个是本轮**最有价值的产出**。它们的共同点：
**在本地永远测不出来**，因为本地的目录布局"长得不像仓库根"。

### 4.1 Bug 1：仓库根的启发式，被构建目录骗了

三个测试文件里各有一份 `findRepoRoot()`，判据是
「往上找，直到看见 `W6_PCB_DefectProject` 和 `W7_VisionMasterLite`」。

CI 的构建目录是 `build/ci/<工程名>/` ——
于是 `build/ci/` 下面**正好有一份**那两个名字。循环在 `build/ci/` 就停了，
把**构建目录当成了仓库根**。

**症状**：

```
[FAIL] 读得到 E:\qianwie\00study\OpenCV\build\ci\W5_OCR_QRCode\images\qrcode_test.png
SUMMARY: pass=5 fail=2 skip=0   RESULT=FAIL
```

看到这条，第一反应一定是"图没了"或者"路径写错了" ——
**没有人会想到"根找错了"**。而真正的原因是"根找错了"。

**修法**：判据换成**只有一处会有**的东西 —— `common/`（共享代码目录）。
构建树里不可能有同名子目录。

```cpp
if (fs::exists(p / "common" / "qr_ocr" / "qr_ocr.h") &&
    fs::exists(p / "W5_OCR_QRCode") &&
    fs::exists(p / "W6_PCB_DefectProject")) {
    return p;
}
```

改了 3 个文件：`common/pcb_core/tests/test_pcb_core.cpp`、
`common/qr_ocr/tests/test_qr_ocr.cpp`、`W6_PCB_DefectProject/tests/test_w6_cli.cpp`。

### 4.2 Bug 2：「往上找 mainwindow.cpp」的前提不成立

`test_run_policy.cpp` 里有两条**源码级守卫**（读 `mainwindow.cpp` 的源码，
检查它有没有退化成"只缓存最后一帧"）。它们用
「从当前目录往上找 `mainwindow.cpp`，最多 8 层」定位文件。

**这个做法的前提是"CWD 在源码树里"** —— 2026-10-20 起就不成立了：

- 测试工作目录挪到了 `build/test_work/<套件名>/`
- CI 的构建目录是 `build/ci/W7_VisionMasterLite/` ——
  它和**源码目录同名**

从 `build/ci/W7_VisionMasterLite/test_work/run_policy/` 往上走，
经过的是**构建目录**那个 `W7_VisionMasterLite/`，
一路走到 `E:/` 都碰不到源码目录。

**症状**（红是真的红，但消息把人引向完全错误的方向）：

```
[FAIL] 能打开 mainwindow.cpp —— 打不开说明它不在预期位置(
       找过 E:/.../build/ci/W7_VisionMasterLite/test_work/run_policy 往上 8 层)
```

源码**就在 6 层外**，只是那个 6 层是**另一个**同名目录。

**修法**：不猜了，**让构建系统告诉测试**。

```cmake
target_compile_definitions(w7_run_test PRIVATE
    W7_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
```

```cpp
#ifndef W7_SOURCE_DIR
#error "W7_SOURCE_DIR is not defined. The build system must tell the test where the sources are."
#endif
static QString findMainWindowCpp() {
    return QString(W7_SOURCE_DIR) + "/mainwindow.cpp";
}
```

`#error` 是关键：**编译期就炸**，不会"安静地退化成一个空断言"。

### 4.3 为什么这两个 bug 在本地测不出来

| | 本地构建目录 | CI 构建目录 |
|---|---|---|
| 布局 | `build/<工程名>/` | `build/ci/<工程名>/` |
| `build/` 下有没有 `W6_PCB_DefectProject` | ❌ 没有 | ✅ **有** |
| 往上走会不会碰到同名目录 | ❌ 不会 | ✅ **会** |

**只有当构建目录的结构"长得像仓库"时，这两个 bug 才会现形。**
而"构建目录按工程名分目录"恰恰是 CI 很自然的一种布局 ——
也就是说，**CI 一上来就把本地永远测不到的 bug 挖出来了**。

> 详见原子卡 `notes/atomic/at-lookalike-dir-fools-the-locator.md`

> 第四章那条教训单独成卡：
> `notes/atomic/at-no-runner-is-not-a-reason.md`
> —— **"没有真环境"是一句结论，不是一个理由。**

---

## 六、变异验证（2 个，全杀）

| # | 变异 | 结果 |
|---|---|---|
| **M1** | 把 `image_sources` 套件的 `WORKING_DIRECTORY` 改回 `${CMAKE_SOURCE_DIR}` | **杀**。**测试本身仍然全绿**（14/14 + 3/3 —— 污染不影响测试结果），但 CI 的第 5 步红了，并**逐个点名**泄漏的文件：`?? W7_VisionMasterLite/images/_srcframe_00.png` … `_test_src.avi`（5 个）。这正是这条检查存在的意义：**测试绿 ≠ 没污染** |
| **M2** | 去掉 `W7_SOURCE_DIR` 的编译定义 | **杀**。**编译直接失败**，退出码 1：`error C1189: #error: "W7_SOURCE_DIR is not defined. The build system must tell the test where the sources are."` → 守卫不可能静默退化 |

M1 与 ③a 的 M1 是同一个变异，但**观察点不同**：
③a 看的是「源码树被写脏了」，③b 看的是「**CI 会因此报红**」。
同一个 bug，从"人工能看出来"变成"**机器每次都会拦**"。

---

## 七、CI 检不出的东西（已知盲区，列出来备查）

| 项 | 现状 | 判断 |
|---|---|---|
| **W1–W4 没有测试** | 4 个工程只有 `main.cpp`，CI 里**只编译不测** | 这 4 个是入门 demo（读写/滤波/轮廓/模板匹配），算法薄。但"只编译"就是"没测"，**别把它当测过了** |
| **W5 只有冒烟** | W5 没有自己的测试套件。CI 只验证"exe 能跑并解出串号" | QR/OCR 的**核心**被 `common/qr_ocr` 的 36 条断言覆盖（两个工程各跑一遍）。W5 的 `main.cpp`（参数解析、`--no-gui`、退出码）**只有冒烟这一层** |
| **W6 的 `main.cpp` 没被跑** | W6 的 CLI 逻辑被 `w6_cli` 的 163 条断言覆盖，但 `main.cpp` 的参数解析只在 CI 里被**编译** | 缺口比 W5 小（`runReport` / `writeCSV` 都测了），但确实没跑 |
| **依赖准备步骤未在真 runner 上验证** | 见第三、四章 | ⚠ **2026-10-22 变了**：三步的**命令本身**已在本机实测（挖出三个真 bug 并修了），**整套 CI 也对着官方 world 包跑通了**（彩排 E），runner 镜像清单也查过。仍需真 runner 的只剩三条**环境性**的：choco 装 tesseract 这一步的动作、job 在 GitHub 基础设施上的编排、runner 侧的网络带宽 |
| **`ctest -j` 并行没测过** | CI 脚本跑的是串行 | 每套件一份工作目录，理论上可以并行（下轮可以改成 `-j` 提速）|
| **应用的运行时产物仍进源码树** | W7 应用跑起来时算子会往 `images/` 写 `w7_*.png`；W6 的 `inspect` 写 `w6_inspect_*.png` | 那是**应用**行为不是测试行为，`.gitignore` 兜着。CI 的第 5 步只查"**测试**有没有往里写" |

---

## 八、怎么用

```powershell
# 本机：一条命令跑完全部五步
powershell -ExecutionPolicy Bypass -File tools/ci.ps1

# 只想重跑测试（不重新编译）
powershell -ExecutionPolicy Bypass -File tools/ci.ps1 -SkipBuild

# 依赖装在别处
powershell -ExecutionPolicy Bypass -File tools/ci.ps1 `
    -OpenCvLib 'C:/opencv/build/lib' `
    -OpenCvBin 'C:/opencv/build/bin' `
    -QtRoot    'C:/Qt/6.8.3/msvc2022_64'
```

依赖准备那三步（第四章新增）：

```powershell
# 彩排：只验，不下载不安装（≈30 秒）
powershell -ExecutionPolicy Bypass -File tools/ci-deps.ps1 -VerifyOnly

# 只验 OpenCV 的布局 —— 对着本机那套**模块化**安装
powershell -ExecutionPolicy Bypass -File tools/ci-deps.ps1 -VerifyOnly `
    -SkipTesseract -SkipQt -OpenCVRoot 'D:/opencv-src/build/install'

# 真装（会下几个 GB）
powershell -ExecutionPolicy Bypass -File tools/ci-deps.ps1
```

⚠ 彩排和真跑**是同一个文件** —— 这正是把它从 YAML 里搬出来的理由。

**整份 GitHub Actions workflow 在本机跑**（第四章 4.5.3）：

```powershell
# 先确认有 pwsh（PowerShell 7）；没有就装一份便携版的
powershell -ExecutionPolicy Bypass -File tools/ci-local.ps1 -ListOnly
powershell -ExecutionPolicy Bypass -File tools/ci-local.ps1 `
    -PwshExe 'C:/path/to/pwsh.exe'
```

它**解析 `.github/workflows/ci.yml` 并执行里面的 `run:` 块** ——
不抄任何东西，所以本机跑的 和 GitHub 跑的是同一批命令。

CI 的构建产物落在 `<仓库>/build/ci/<工程名>/`（`build/` 在 `.gitignore` 里），
所以第 5 步"源码树没被写脏"这句话仍然成立。

---

## 九、核实清单

本文所有数字与输出，均于 2026-10-20 当天在本机实测：

| 论断 | 核实方式 |
|---|---|
| 改动前 28 处硬编码路径（20 + 7 + 1）| `git show HEAD:<每个 CMakeLists>` 正则计数 |
| `-D` 覆盖真的生效、且推导出 bin 目录 | 建联接 `ocv_link` → 全新 configure → 看 `CMakeCache.txt`（`UNINITIALIZED` / 推导出的 bin）→ 编译通过 + 14 个 DLL 就位 |
| CI 脚本最终跑通 | 连续 4 次 `powershell -File tools/ci.ps1`，最后一次 `ALL CHECKS PASSED`（退出码 0）|
| Bug 1 的症状 | CI 第一次跑，3 个套件红，报"读得到 .../build/ci/W5_OCR_QRCode/images/qrcode_test.png" |
| Bug 2 的症状 | CI 第二次跑，`run_policy` 红 2 条，报"往上 8 层都找不到"；逐层 `Test-Path` 证明源码在 6 层外、但那 6 层是构建目录 |
| M1 被杀 | 改回 `${CMAKE_SOURCE_DIR}` → 测试 17/17 全绿，但第 5 步点名 5 个泄漏文件 |
| M2 被杀 | 去掉编译定义 → `error C1189: #error`，退出码 1 |
| PowerShell stderr 坑 | 第一次跑的真实报错（`ctest.exe : Errors while running CTest` + 脚本行号）|
| 「源码树干净」语义写错 | 第二次跑的真实报错（把 13 个未提交改动判成泄漏）|

### 2026-10-22 新增（第四章）

| 论断 | 核实方式 |
|---|---|
| 官方 OpenCV 4.13.0 包的**真实布局** | 下载真包（185.5 MB）→ 解包 → 逐个目录 `Test-Path` + 列文件 |
| 那个包里**没有** `opencv_core4130.*` | `Get-ChildItem -Recurse -Filter 'opencv_core*.lib'/'*.dll'` → 零结果；只有 `opencv_world4130.*` |
| config 和 import lib **在同一个目录** | `opencv/build/x64/vc16/lib/` 里同时有 `OpenCVConfig.cmake` 和 `opencv_world4130.lib` |
| 头文件路径仍然对 | 内层 config 用 `../../../` 算回 `opencv/build`，`opencv/build/include/opencv2` 下 297 个 `.hpp` |
| `msvc2022_64` 不是 aqt 的合法架构名 | `aqt install-qt ... msvc2022_64 --dry-run` → `ERROR: The packages ['qt_base'] were not found` |
| `win64_msvc2022_64` 是合法的 | 同一命令换成规范名 → `DRY RUN: Would download ... qtbase ...` |
| tesseract 装到哪 | 下载 choco 的 nupkg，读 `chocolateyinstall.ps1`：`Install-ChocolateyPath $env:ProgramFiles\Tesseract-OCR` |
| 新脚本吃**两种** OpenCV 形状 | 彩排 A（官方 world 包）与彩排 B（本机模块化安装）都定位到正确的 config/lib/dll |
| 解压可行 | bsdtar 直接读 SFX（`tar -tf` 能列目录）；7z 也在 runner 镜像上 |
| `tools/ci-deps.ps1` 是纯 ASCII | 逐字节扫 `> 127` → 0 个 |
| **整套 CI 对着官方 world 包跑通** | `ci.ps1 -OpenCvLib <pkg>/lib -OpenCvBin <pkg>/bin` → `ALL CHECKS PASSED`（W6 3/3 + W7 15/15 + W5 冒烟 + 源码树零污染）|
| world 包的 `${OpenCV_LIBS}` 映射能过 | 同上：15/15 套件全绿，exe 旁边是 `opencv_world4130.dll` |
| **runner 镜像上有什么** | 查 `actions/runner-images` 的 `Windows2022-Readme.md`：Chocolatey 2.7.4 / 7zip 26.04 / Python 3.12.10 / VS Enterprise 2022 17.14 |
| 「W7 必须用 CMake 4.3.1」的真实原因 | CMake 3.30.5 报 `CMAKE_CXX_COMPILER not set`（认不出 VS 2026）；加 vcvars64 后同一个 3.30.5 配得出来 → 是**工具认不认编译器**，不是工程要 4.x |
| **整份 `ci.yml` 在本机跑通** | `tools/ci-local.ps1`（解析 YAML 执行 `run:` 块）→ `ALL WORKFLOW STEPS PASSED` |
| **`GITHUB_ENV` 交接真的通了** | 彩排 F 的第 2 步收到的 `-DOpenCV_DIR` / `-DOpenCV_BIN_DIR` / `-DCMAKE_PREFIX_PATH` 正是第 1 步写进去的值 |
| **模拟器的 shell 和 runner 一致** | 拉便携版 pwsh **7.6.6**（runner 镜像清单上的版本），默认编码 UTF-8 与 runner 相同 |

### 2026-10-26 新增（第十章）

| 论断 | 核实方式 |
|---|---|
| **整条 CI 在真 runner 上绿了** | run `40315236950`：4 个 step 全 `success`，整条 job **8m0s**，日志尾部 `ALL CHECKS PASSED` |
| runner 上的数和本机**一个不差** | runner 的 `== summary` 原文：`29 ctest suites` / `2853 assertions` / `docs: every number quoted in the docs matches the build` |
| choco 装 tesseract 在真 runner 上成功 | 日志：`[ci-deps] tesseract installed: C:\Program Files\Tesseract-OCR\tesseract.exe`（21 秒）|
| aqtinstall 在真 runner 上成功 | 日志：`[ci-deps] QT_ROOT=D:\a\_temp\Qt\6.8.3\msvc2022_64`（aqt v3.3.0 / Python 3.12.10，51 秒）|
| 官方 world 包在真 runner 上能编译链接 | 日志：`opencv_world4130.lib` / `opencv_world4130.dll` 就位；7 个工程全编过；W6 3 套件 + W7 26 套件全绿 |
| ACP 那条修复在真 runner 上**真的成立** | 同一个 `w6_cli` 套件：上一次真跑 `pass=20 fail=1`（异常打断进程，后面的用例没跑），这一次 `pass=167 fail=0` |
| 「本机和 runner 的数一样」不是抄的 | `2853` 是 runner **自己数出来**的（`counted from this build`），本机与 runner 各自独立数出同一个数 |

---

## 十、★ CI 第一次在真 runner 上跑（2026-10-26）

### 10.1 一句话

> **2026-10-26 11:27，run `40315236950` 绿了。**
>
> 7 个工程编译、29 个 ctest 套件、**2853** 条断言、W5 冒烟、
> W7 无头跑通一条流水线、源码树零污染 ——
> 全部在 GitHub 的 Windows runner 上跑完，`ALL CHECKS PASSED`。
>
> **本机和 runner 报出来的数，一个都不差。**

### 10.2 三次真跑，三种死法

| run | 死在哪 | 那是什么性质的问题 |
|---|---|---|
| `40288478649` | job **没起来** | YAML 里的错：变量没交接、路径写死 |
| `40293214498` | W6 的 `w6_cli` **红 1 条** | 机器事实：ACP 把同一份代码判成两回事 |
| `40315236950` | —— | ✅ **全绿**（8m0s）|

⇒ 前两次**不是**「环境不行」，是**代码里的东西从没在真环境里活过**。

### 10.3 原来标着「没法验证」的那几条，现在有答案了

| 原来标的 | 现在 | 真跑给出的证据 |
|---|---|---|
| **choco 装 tesseract 这一步本身** | ✅ 验过 | `tesseract installed: C:\Program Files\Tesseract-OCR\tesseract.exe`（21 秒）|
| **aqtinstall 在 runner 上跑不跑得动** | ✅ 验过 | `QT_ROOT=D:\a\_temp\Qt\6.8.3\msvc2022_64`（aqt v3.3.0 / Python 3.12.10，51 秒）|
| **官方 OpenCV world 包能不能编译链接** | ✅ 验过 | `opencv_world4130.lib` / `.dll` 就位，7 个工程全编过 |
| **runner 的调度器 / 镜像 / 网络** | ✅ 验过 | 整条 job 8m0s 全绿 |

⇒ §3.3 那句「没法验证的是 GitHub 的 Windows runner 上能不能装上」——
**今天这句话已经过期了**。它当时是真的。
>
> ⚠ **一句结论的保质期，就是它被推翻为止。**
> 所以 §3.3 当时写的是「大概率要改那三步，**要改的是哪一行取决于 runner 上的实际情况**」——
> 现在知道**要改的是三处、不是一行**，而且每一处改法都不一样。

### 10.4 本机和 runner 的数必须**一样**，不是「两边都绿」

runner 上 `tools/ci.ps1` 自己打的 summary（原文）：

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

**和本机一个字不差。**

⇒ 「本机能过」和「仓库能过」真正对上，是**这个样子**：
不是「两边都绿」，是**两边的数对得上**。
`2853` 这个数是 runner **自己数出来的**，不是抄本机的。

### 10.5 真跑挖出来的四件事

（完整记录见 `notes/notes_CI_FirstRunnerRun_2026-10.md`）

| # | 是什么 | 本机为什么看不见 |
|---|---|---|
| ① | 空目录不在 git 里 | 本机那目录**在**（有别的文件）|
| ② | Qt 的 DLL 不在 PATH 上 | 本机 Qt 装在 PATH 里 |
| ③ | tesseract 的 PATH 只写在 YAML 里 | 本机环境变量早就有 |
| ④ | **ACP 把同一份代码判成两回事** | 本机 ACP=936（GBK）**有中文** ⇒ `.string()` 不抛 |

前三条是**纯环境性**的；第四条是**代码本身的歧义** ——
它在这台机器上**永远是绿的**：
绿到写完注释、写完探针、本地门禁全过，还是绿的。

⇒ ④ 的教训最贵：
**「本机绿」不是「仓库绿」，而且这类坑专门挑「本机恰好有那个东西」的时候发威。**

### 10.6 欠账 9 的账，到这里结清

`notes/notes_W7_Knob_Audit_2026-10.md` 第十节里那条
「CI 三步依赖准备没在真 runner 上验过」，
它的重开条件是**「仓库有 remote 的那一天」**。
那天 remote 有了，但**光有 remote 不够** —— 得真的推上去、真的跑一次。

⇒ 现在两件事都成立：**有 remote**，而且**跑绿了**。
欠账 9 收口（清单 19 条 / 已收 14 条 / 未收 5 条）。

---

## 十一、一句话带走

> **CI 的第一批产出，通常不是"CI 跑起来了"，而是"CI 第一次跑就挖出了本地测不到的 bug"。**
>
> 本轮挖到的两个 bug 有一个共同点：
> **它们都出在"测试靠猜来定位自己"这件事上。**
> 本地目录布局"长得不像仓库根"，所以猜法一直是对的；
> 换了 CI 的布局，同一个猜法就指错了地方。
>
> 所以：定位用的判据必须是**只有一处会有**的东西；
> 构建系统明明知道的路径，就不要让测试去猜。

---

> **补一条（2026-10-22，第四章）：**
>
> **"没在真环境上验过"是个结论，不该是个借口。**
>
> 欠账挂了很久的那条「CI 三步依赖准备没在真 runner 上验过」，
> 听着像"缺一个环境"，其实**缺的是证据**。
> 而证据的绝大部分**根本不需要 runner** —— 需要的是**那个包本身**。
>
> 包一下来，三步里当场挖出**三个会让 CI 第一次跑就挂的真 bug**：
> 一个参数名错、一个库名假设错、一个目录形状假设错。
>
> 三个都在"从没执行过的那段代码"里 ——
> 而那段代码之所以从没被执行过，
> 是因为它**写在唯一一份没人跑得了的地方**（YAML 里）。
>
> 所以先做的那件事不是"修 bug"，是**把那三步搬成一个本机也跑得了的脚本**：
> 本机验的那份和 runner 跑的那份**是同一个文件**。
>
> **证据不会因为环境缺了就少要，只是取证的路换了一条。**

> **补一条（彩排 E，2026-10-22）：**
>
> 拿到包之后还能再进一步 —— **把整个 CI 对着那个包跑一遍**，
> 而不只是"把找路径的那段跑一遍"。
>
> 彩排 A~D 验的是「**找得对不对**」，
> 彩排 E 验的是「**找到之后货色能不能用**」——
> 也就是 `find_package` 拿到 world 构建时 `${OpenCV_LIBS}` 那个映射到底过不过。
> 这是**最没底**的一格，而它也没用到 runner。
>
> ⚠ 顺手还查了 GitHub 官方那张**镜像清单**（`actions/runner-images`），
> 确认 runner 上有 Chocolatey / 7zip / Python / VS 2022。
> **不用 runner 也能查"runner 上有什么"** —— 有一张公开的清单在那儿。
>
> 于是欠账从「三步全没验过」缩到三条**纯环境性**的：
> choco 那个安装动作、job 在 GitHub 基础设施上的编排、runner 侧的网络。
> 这三条**没法在本机消掉** —— 但它们也**都不是代码写错了**。
> 硬说"本机也能验"，就是拿"验过命令"冒充"验过环境"。

> **再补一条（彩排 F，2026-10-22）：**
>
> **"只能推上去才知道"这件事，本身也可以拆。**
>
> `ci.yml` 是 **YAML** —— YAML 不是能在本机执行的程序，
> 所以"workflow 的编排"一向只能推上去才知道。
> 但**编排是可以模拟的**：把 runner 干的那几件事（本机做一遍）列出来 ——
> 用哪个 shell、`GITHUB_ENV` 怎么传给下一步、失败怎么退出、
> `RUNNER_TEMP` 在哪 —— 然后**把 YAML 里的 `run:` 块读出来执行**。
>
> 于是本机跑的不再是"照着 YAML 手敲一遍"，
> 而是**那几行命令本身**。没有副本，就没有漂移。
>
> ⚠ 顺带发现的最容易静默失败的一处：**`GITHUB_ENV`**。
> 变量没传过去，ci.ps1 收到的是空串，报错是
> "OpenCVConfig.cmake not found under: " ——
> 而真正的原因在三步之前。这种错只有真跑一遍编排才看得见。
