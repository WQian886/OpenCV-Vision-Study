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
| `tools/ci-deps.ps1` | 装 tesseract / Qt 6.8.3 / OpenCV 4.13.0 | ✅ **命令本身已本机实测**；❌ 只剩 runner 特有那几条（见 4.7）|
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

### 3.3 【未验证】这件事的诚实说法

**本地能验证的都验证了**（脚本五步、路径覆盖、两个 bug 的修复、变异），
**没法验证的是"GitHub 的 Windows runner 上，Qt/OpenCV/tesseract 能不能装上"** ——
这台机器连 GitHub 的 release 都下不动。

第一次真的 push 上去跑，大概率要改 `ci.yml` 里那三步
（aqtinstall 的参数、OpenCV 包的目录名、tesseract 的装法），
**而要改的具体是哪一行，取决于 runner 上的实际情况** ——
这一条在 workflow 顶部的注释里也写清楚了。

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

### 4.5 本机彩排（四次，全部实测）

| 彩排 | 验的是 | 结果 |
|---|---|---|
| A | 官方 **world** 包（已解包）| config=`…/x64/vc16/lib`、lib=`opencv_world4130.lib`、dll=`…/x64/vc16/bin` ✅ |
| B | 本机**模块化**安装（`D:\opencv-src\build\install`）| config=`…/lib`、lib=`opencv_calib3d4130.lib`、dll=`…/bin` ✅ |
| C | tesseract 搜索 | 找到 `D:\Program Files\Tesseract-OCR\tesseract.exe` ✅ |
| D | Qt 包名 | `--dry-run` 解析出 qtbase 等 ✅（不下载 2 GB）|

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
| **choco 在 runner 上能不能装** | ❌ 仍需真 runner（本机没 choco） |
| **aqt 在 runner 镜像上跑不跑得动** | ❌ 仍需真 runner |
| **下载带宽 / 超时** | ❌ 仍需真 runner（本机 185 MB 下了十几秒，但 runner 的网络不一定） |
| **对着官方 world 包真的能编译链接** | ❌ 本机是 MSVC 19.51，官方包是 v142 构建；链接层面没验 |

最后一条是**唯一还可能真出问题**的：包找到了、路径对了，
但 `find_package(OpenCV)` 拿到 world 构建时 `${OpenCV_LIBS}` 能不能链接通 ——
本机没法验（编译器版本对不上），**得在 runner 上验（runner 是 VS 2022，和 v142 同代）**。

> ⚠ 顺带一个发现：**runner 的编译器比本机旧**（windows-2022 = VS 2022 v143，本机 = VS 2026 v143/v145 系）。
> 项目用的是 C++17，VS 2022 完全支持 ✓；
> 但这条以前没人写过 —— 它属于"只有真 runner 才能验"的那一类。

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
| **依赖准备步骤未在真 runner 上验证** | 见第三、四章 | ⚠ **2026-10-22 变了**：三步的**命令本身**已在本机实测（还挖出三个真 bug 并修了）。仍需真 runner 的只剩：choco 能不能装、aqt 在 runner 上跑不跑得动、下载带宽，以及**对着 world 包真的能链接** |
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

---

## 十、一句话带走

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
