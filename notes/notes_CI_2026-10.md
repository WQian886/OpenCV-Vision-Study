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

## 三、GitHub Actions workflow（**依赖准备那几步未验证**）

`.github/workflows/ci.yml`：

| 步骤 | 做什么 | 验证状态 |
|---|---|---|
| `actions/checkout` | 拉代码 | ✅ 标准动作 |
| `choco install tesseract` | 装 tesseract（OCR 那条路要它）| ❌ **未在真 runner 上跑过** |
| `aqtinstall` 装 Qt **6.8.3** | 版本**钉死**在开发时用的那个 | ❌ **未在真 runner 上跑过** |
| 下载官方 OpenCV **4.13.0** 预编译包 + 7z 解压 + **布局发现** | 找 `OpenCVConfig.cmake` 和 `opencv_core*.dll` | ❌ **未在真 runner 上跑过** |
| `tools/ci.ps1` | 真正干活的那五步 | ✅ **本机跑通 4 次** |

### 3.1 为什么 OpenCV 那步是「发现」而不是「写死路径」

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

## 四、★ CI 第一次跑就逮到的两个真 bug

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

---

## 五、变异验证（2 个，全杀）

| # | 变异 | 结果 |
|---|---|---|
| **M1** | 把 `image_sources` 套件的 `WORKING_DIRECTORY` 改回 `${CMAKE_SOURCE_DIR}` | **杀**。**测试本身仍然全绿**（14/14 + 3/3 —— 污染不影响测试结果），但 CI 的第 5 步红了，并**逐个点名**泄漏的文件：`?? W7_VisionMasterLite/images/_srcframe_00.png` … `_test_src.avi`（5 个）。这正是这条检查存在的意义：**测试绿 ≠ 没污染** |
| **M2** | 去掉 `W7_SOURCE_DIR` 的编译定义 | **杀**。**编译直接失败**，退出码 1：`error C1189: #error: "W7_SOURCE_DIR is not defined. The build system must tell the test where the sources are."` → 守卫不可能静默退化 |

M1 与 ③a 的 M1 是同一个变异，但**观察点不同**：
③a 看的是「源码树被写脏了」，③b 看的是「**CI 会因此报红**」。
同一个 bug，从"人工能看出来"变成"**机器每次都会拦**"。

---

## 六、CI 检不出的东西（已知盲区，列出来备查）

| 项 | 现状 | 判断 |
|---|---|---|
| **W1–W4 没有测试** | 4 个工程只有 `main.cpp`，CI 里**只编译不测** | 这 4 个是入门 demo（读写/滤波/轮廓/模板匹配），算法薄。但"只编译"就是"没测"，**别把它当测过了** |
| **W5 只有冒烟** | W5 没有自己的测试套件。CI 只验证"exe 能跑并解出串号" | QR/OCR 的**核心**被 `common/qr_ocr` 的 36 条断言覆盖（两个工程各跑一遍）。W5 的 `main.cpp`（参数解析、`--no-gui`、退出码）**只有冒烟这一层** |
| **W6 的 `main.cpp` 没被跑** | W6 的 CLI 逻辑被 `w6_cli` 的 163 条断言覆盖，但 `main.cpp` 的参数解析只在 CI 里被**编译** | 缺口比 W5 小（`runReport` / `writeCSV` 都测了），但确实没跑 |
| **依赖准备步骤未在真 runner 上验证** | 见第三章 | 第一次 push 大概率要改 `ci.yml` 里那三步 |
| **`ctest -j` 并行没测过** | CI 脚本跑的是串行 | 每套件一份工作目录，理论上可以并行（下轮可以改成 `-j` 提速）|
| **应用的运行时产物仍进源码树** | W7 应用跑起来时算子会往 `images/` 写 `w7_*.png`；W6 的 `inspect` 写 `w6_inspect_*.png` | 那是**应用**行为不是测试行为，`.gitignore` 兜着。CI 的第 5 步只查"**测试**有没有往里写" |

---

## 七、怎么用

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

CI 的构建产物落在 `<仓库>/build/ci/<工程名>/`（`build/` 在 `.gitignore` 里），
所以第 5 步"源码树没被写脏"这句话仍然成立。

---

## 八、核实清单

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

---

## 九、一句话带走

> **CI 的第一批产出，通常不是"CI 跑起来了"，而是"CI 第一次跑就挖出了本地测不到的 bug"。**
>
> 本轮挖到的两个 bug 有一个共同点：
> **它们都出在"测试靠猜来定位自己"这件事上。**
> 本地目录布局"长得不像仓库根"，所以猜法一直是对的；
> 换了 CI 的布局，同一个猜法就指错了地方。
>
> 所以：定位用的判据必须是**只有一处会有**的东西；
> 构建系统明明知道的路径，就不要让测试去猜。
