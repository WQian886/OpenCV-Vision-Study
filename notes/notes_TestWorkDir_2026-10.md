# 测试产物落构建目录：`.gitignore` 藏起来的是文件，不是问题（2026-10-20）

> 📅 **记录时间**：2026-10-20
> 📁 **项目来源**：W6_PCB_DefectProject / W7_VisionMasterLite / .gitignore
> 🔬 **核实方式**：本篇所有技术论断均于 2026-10-20 当天编译运行 / 读源码确认，核实清单见文末
> 🎯 **本次目标**：让测试产物全部落进构建目录，源码树不再被污染；并且**不许**用 `.gitignore` 把问题藏起来

---

## ⚠️ 使用前必读：本文的证据等级

| 标记 | 含义 | 能否当事实引用 |
|---|---|---|
| 【实测】 | 当天编译运行 / 读源码确认，有具体输出 | ✅ 可以 |
| 【实测·项目】 | 在本项目实测成立，换项目需重新验证 | ⚠️ 需验证前提 |
| 【推演】 | 基于实测数据的合理推断，未单独构造用例 | ❌ 不要当事实 |

**本篇没有"听说""应该""一般来说"这类表述。**

---

## 📌 一句话总结

测试原来把自己的中间产物**写进源码树**，然后往 `.gitignore` 里加规则把它们**藏起来** ——
于是 `git status` 干净、看上去"处理好了"，文件其实还在树里。
这一轮把 14 个 W7 套件和 3 个 W6 套件的工作目录**全部挪进 `build/test_work/<套件名>/`**，
删掉那两条纯为藏文件而存在的 ignore 规则，源码树里 25 个残留文件清空，
并且**顺手逮到一条一直在"靠 CWD 恰好正确"才成立的断言**。

---

## 一、症状：源码树被测试污染

### 1.1 【实测】污染清单

改这一轮之前，本机上的源码树里躺着这些文件：

**`W7_VisionMasterLite/images/`（14 个）**

| 文件 | 谁写的 |
|---|---|
| `w7_pcb_inspect.png` / `w7_pcb_defect.png` / `w7_pcb_defect_judge.png` | PCB 算子（`artifact_path.h` 的"测试路径"分支）|
| `w7_color_segment_mask.png` | `color_segment` 算子（无 uuid 时的固定名分支）|
| `w7_video_f0000..0004.png` | `image_source_video` 算子 |
| `_srcframe_00..03.png` | `test_image_sources.cpp` 自造 |
| `_test_src.avi` | `test_image_sources.cpp` 自造 |

**仓库根 `images/`（11 个，2026-10-15 的旧账）**：同样的一批
（`w7_color_segment_mask.png`、`w7_video_f*.png`、`_srcframe_*.png`、`_test_src.avi`）——
这说明**从仓库根手动跑过测试 exe**，产物就落在那儿。

**`W6_PCB_DefectProject/images/`（4 个）**：`w6_inspect_*.png`，这是 W6 **应用**的产物，
不是测试产物，保留（见 4.3）。

### 1.2 【实测】为什么 6 个 W7 套件会写进源码树

改动前 `W7_VisionMasterLite/CMakeLists.txt` 里 14 个 `add_test` 的写法：

```
add_test(NAME pcb_operators COMMAND w7_pcb_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})     ← 6 个是源码树
add_test(NAME qr_ocr COMMAND w7_qr_ocr_test)  ← 8 个没给,跑在 build/ 根目录
```

**6 个**套件的 CWD 是 `${CMAKE_SOURCE_DIR}`（= `W7_VisionMasterLite/`），
**8 个**没给 `WORKING_DIRECTORY`（ctest 默认用 build 根目录）。

而这 6 个里正好是所有**会写文件**的套件：

| 套件 | 写什么 |
|---|---|
| `pcb_operators` | `images/w7_pcb_*.png`、`build/_bright/`、`build/_gm/` |
| `image_sources` | `images/_test_src.avi`、`images/_srcframe_*.png`、`images/w7_video_f*.png` |
| `measurement` | `images/_t_*.png`（自己起自己删，所以树里没留）|
| `run_policy` | 无（纯逻辑）|
| `detection_operators` | `images/w7_*.png` |
| `template_match` | `images/w7_tm_*.png`（收尾删掉 4 个，剩几个）|

### 1.3 【实测】当时的应对是往 `.gitignore` 里加规则

```gitignore
**/images/w7_*.png
**/images/_test_src.avi
**/images/_srcframe_*.png
```

**这是治标。** 文件还在源码树里：

- 还在被人看见（`git status` 看不见，但 `ls` 看得见）
- 还会被 `copy_directory` 一起拷进构建目录（构建产物里因此也有一份来源不明的旧数据）
- 规则本身在撒谎：它说的是"这里不会有文件"，实际是"这里有文件，但 git 假装看不见"
- 最要命的是**看起来已经处理好了**

> 详见原子卡 `notes/atomic/at-gitignore-hides-files-not-problems.md`

---

## 二、修法：每个套件一份工作目录，全在 build 里

### 2.1 【实测】改完之后

```
W7_VisionMasterLite/build/test_work/
    qr_ocr/  pcb_core/  undo_redo/  pcb_operators/  dl_postprocess/
    image_sources/  step_result/  pin_and_recent/  line_fit/
    measurement/  run_policy/  detection_operators/  template_match/
    operator_library/
W6_PCB_DefectProject/build/test_work/
    pcb_core/  qr_ocr/  w6_cli/
```

每个目录里是这一套件自己的 `recipes/`、`images/`（W7 还有 `images/complex/`），
以及**这一套件跑出来的产物**。

跑完全部测试之后：

```
$ git status --short
 M .gitignore
 M W6_PCB_DefectProject/CMakeLists.txt
 M W7_VisionMasterLite/CMakeLists.txt
 M W7_VisionMasterLite/tests/test_pcb_operators.cpp
 M W7_VisionMasterLite/tests/test_run_policy.cpp
```

**只有我这次改的 5 个文件，一个残留都没有。**

### 2.2 【实测】W7 的 CMake 改法

新增一个 `w7_add_test(suite target)` 函数，14 个 `add_test` 全部换成它：

```cmake
function(w7_add_test suite target)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/test_work/${suite}")
    add_test(NAME ${suite} COMMAND ${target} WORKING_DIRECTORY "${_dir}")
    set(W7_TEST_SUITES ${W7_TEST_SUITES} ${suite} PARENT_SCOPE)
endfunction()
```

### 2.3 【实测】数据从哪来：暂存，不是抄副本

工作目录里必须有 `recipes/`（`pcb_inspect` 按相对路径 `"recipes/xxx.json"` 读）
和 `images/`（demo 板图当输入，同时是算子画框图的落点）。

**配方只有一份**（`W7_VisionMasterLite/recipes/`，由 `tools/export_recipes.py`
从 W6 自己的生成器导出）。工作目录里那一份是**构建期暂存**，
不是第二份定义 —— 真正被读的仍然是那一份，导出脚本也只认那一份。

复杂板（power / mcu / signal）的图在 W6 那边 —— 那边才是生成器所在地 ——
所以也一并暂存到 `images/complex/`。

### 2.4 【实测】为什么用「每次构建都重拷」，而不是 POST_BUILD / configure 时拷

这一条是本轮最花心思的地方，因为两个"看起来对"的方案都有静默失效的坑。

| 方案 | 问题 | 实测结论 |
|---|---|---|
| **POST_BUILD** 挂在目标上 | 只在目标**重新链接**时才跑。改了 `recipes/demo.json` 而代码一行没动 → 不重编 → 不重拷 → 测试拿**旧配方**判板，判出来还"是对的"（旧配方下的正确答案）→ **改配方其实没生效，没人知道** | ✗ 静默 |
| **configure 时拷** | 只在"有人跑 cmake"时才更新。改了图直接 `ctest`，拿到的还是旧数据，**没有任何提示** | ✗ 静默 |
| **每次构建都重拷** | `add_custom_target` 没有输出文件，CMake 永远认为它"过期"，所以**每次构建都会跑** | ✅ 无条件新鲜 |

```cmake
add_custom_target(w7_test_data ALL ${_stage_cmds}
    COMMENT "Staging W7 test data into build/test_work/")
```

代价是每次构建多几十个 `cmake -E copy_directory`（实测 1 秒上下），
换来的是"改了数据，下一次构建就生效"，而且这个性质是**无条件**的。

W6 那边同款（只暂存 `recipes/`），见 3.1。

---

## 三、顺带逮到的两件事

### 3.1 【实测】一条一直在"靠 CWD 恰好正确"才成立的断言

把 W6 的工作目录也挪进 `build/test_work/` 之后，`w6_cli` 套件**当场红了一条**：

```
[FAIL] 自动找得到配方目录  (case: 配方目录:能找到,且不会因为一个坏目录就整个废掉)
SUMMARY: pass=161 fail=1   RESULT=FAIL
```

原因：`w6::findRecipeDir("")` 的自动发现是**按相对路径试**的：

```cpp
static const char* kRecipeDirs[] = {
    "recipes",                                  // 编译时拷到 exe 旁边
    "../W7_VisionMasterLite/recipes",           // 仓库里跑
    "W7_VisionMasterLite/recipes",
};
```

工作目录从 `W6_PCB_DefectProject/build/` 挪到 `build/test_work/w6_cli/`，
这三个相对路径**全落空**了。

**这条断言红得对。** 它本来就是在断言"自动发现能找到配方"，
而自动发现**确实**依赖 CWD。

修法有三条，只有一条是对的：

| 修法 | 评价 |
|---|---|
| 把断言改松 / 删掉 | ✗ 把一条真的失效的断言改成"通过" |
| 把 CWD 拉回 `build/` | ✗ 等于白改这一轮 |
| **把配方暂存进工作目录** | ✅ 这也正是 W6 真跑起来时的样子 —— CMake 会把 `recipes/` 拷到 exe 旁边，自动发现找的就是它 |

```cmake
list(APPEND _stage_cmds
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../W7_VisionMasterLite/recipes"
            "${_d}/recipes")
```

补完暂存 → `SUMMARY: pass=163 fail=0`，回到改动前的条数。

### 3.2 【实测】W7 `run_policy` 的源码级守卫：以前靠 CWD 恰好正确

`test_run_policy.cpp` 里有两条**源码级守卫** —— 它们去读 `mainwindow.cpp`
的源码，检查里面有没有退化成"只缓存最后一帧"。

以前能工作，是因为 `run_policy` 套件的 CWD **恰好**是 `${CMAKE_SOURCE_DIR}`，
相对路径 `"mainwindow.cpp"` 恰好能打开。

这段历史有段血案（见 `notes/atomic/at-silent-fail-shape.md` 一类）：
第一版守卫写成 `if (!opened) return;` —— 找不到文件就直接返回，
于是一条断言都没跑，测试照样全绿。那是**恒真的空断言**，
比没有守卫更坏，因为它让人以为守卫在生效。

现在工作目录挪到 `build/test_work/run_policy/`，相对路径必然打不开 ——
所以守卫必须**自己去找**：

```cpp
static QString findMainWindowCpp() {
    QDir dir = QDir::current();
    for (int i = 0; i < 8; ++i) {
        const QString cand = dir.filePath("mainwindow.cpp");
        if (QFile::exists(cand)) return cand;
        if (!dir.cdUp()) break;
    }
    return QString();
}
```

找**不到就报红**（`check(opened, ...)` 在 `if (!opened) return;` 之前），
不是安静地跳过。

### 3.3 【实测】`kComplexDir`：复杂板路径必须跟着工作目录一起改

`test_pcb_operators.cpp` 里复杂板的数据目录原来是
`"../W6_PCB_DefectProject/images/complex/"`（相对工程根的）。
工作目录一挪，这个相对关系就断了。

改法不是"再写一个找仓库根的函数"，而是**用暂存好的那份**：

```cpp
static const char* kComplexDir = "images/complex/";
```

理由：拷不动会在**构建阶段**就炸（见变异 M2），不会拖到测试阶段
变成一句含糊的"找不到文件"。

> ⚠ 顺带一个教训：这个常量如果忘了改，症状是"复杂板用例全红"，
> 但红的原因看起来像"判据失效"，不像"路径错了" ——
> 查错方向完全不同。变异 M4 专门钉了这条。

---

## 四、变异验证（4 个，全杀）

**判据**：改坏之后必须**看得见**。对配置类改动，"看得见"的标准是
「构建/测试以非零码失败」或「源码树变脏」；对代码改动，是「断言变红」。

| # | 变异 | 结果 |
|---|---|---|
| **M1** | 把 `image_sources` 套件的 `WORKING_DIRECTORY` 改回 `${CMAKE_SOURCE_DIR}` | **杀**。测试**仍然 100% 通过**（这不是 bug，是事实），但源码树里**多出 5 个文件**，`git status` 直接列出来 → 证明"产物落点真的由工作目录决定"，也证明 ignore 规则删掉之后污染是**看得见**的 |
| **M2** | 把暂存的数据源路径指错（`images/complex` → `images/complexX`） | **杀**。**构建直接失败**，退出码 1：`Error copying directory from ".../images/complexX" ... 系统找不到指定的路径` → 数据拷不到必须炸，不许静默 |
| **M3** | `findMainWindowCpp()` 改成永远返回空串 | **杀**。`run_policy` 红 **2** 条（两条源码级守卫的"能打开 mainwindow.cpp"）→ 守卫是活的，不是恒真 |
| **M4** | `kComplexDir` 改成 `"images/complexX/"` | **杀**。`pcb_operators` 红 **36** 条，报错**直接点名路径**：`定位不到元件 (imread failed: images/complexX/power_baseline.png)` → "读不到图"是**响的**，不是"少检了板" |

### 4.1 【实测】M1 的输出

```
$ ctest -R image_sources        # 工作目录改回源码树
100% tests passed, 0 tests failed out of 1

$ git status --short
 M .gitignore
 M W6_PCB_DefectProject/CMakeLists.txt
 M W7_VisionMasterLite/CMakeLists.txt
 M W7_VisionMasterLite/tests/test_pcb_operators.cpp
 M W7_VisionMasterLite/tests/test_run_policy.cpp
?? W7_VisionMasterLite/images/_srcframe_00.png
?? W7_VisionMasterLite/images/_srcframe_01.png
?? W7_VisionMasterLite/images/_srcframe_02.png
?? W7_VisionMasterLite/images/_srcframe_03.png
?? W7_VisionMasterLite/images/_test_src.avi
```

**"测试全绿 + 源码树变脏"这个组合，就是原 bug 的全部危害。**
改动之前，第二行 `git status` 是干净的 —— 靠 ignore 规则遮住了。
改动之后它露出来了：规则没了，测试也不写那儿了。

### 4.2 【实测】M2 的输出

```
Error copying directory from "E:/.../W6_PCB_DefectProject/images/complexX"
to "E:/.../build/test_work/qr_ocr/images/complex": 系统找不到指定的路径。
...w7_test_data.rule" 的自定义生成已退出，代码为 1。
构建退出码: 1
```

**构建期失败，而不是测试期"读不到图"。** 这是更早、更响的一处。

### 4.3 【实测】M3 / M4 的输出

```
M3: [FAIL] 能打开 mainwindow.cpp —— 打不开说明它不在预期位置(找过
    E:/.../build/test_work/run_policy 往上 8 层),这条源码守卫会变成恒真的空断言
    0% tests passed, 1 tests failed out of 1

M4: PASS=162  FAIL=36
    [FAIL] power_baseline.png 定位不到元件 (imread failed: images/complexX/power_baseline.png)
    [FAIL] power_baseline.png 有 16 个元件 (实际 0)
    ...
```

### 4.4 【实测】还原之后

全部还原 → 重新配置编译 → 全量复测：

```
W6: PASS=379  FAIL=0     3/3 套件通过
W7: PASS=1101 FAIL=0     14/14 套件通过
```

**断言总数 1480 条，与改动前逐条相同**（180 + 36 + 163 + 1101）——
一个不少，一个不多。数值没动说明这一轮**只动了落点，没动行为**。

---

## 五、`.gitignore` 的改动

删掉两条**纯粹为了藏测试产物**而存在的规则：

```diff
 **/images/w7_*.png
-# 图像源回归测试自造的临时素材
-**/images/_test_src.avi
-**/images/_srcframe_*.png
```

**保留**的规则（这些是**应用**的运行时产物，不是测试产物）：

```gitignore
images/*_marked*.png      # W6 inspect 的标注图
images/w6_inspect_*.png   # W6 inspect 的标注图
images/w7_*.png           # W7 算子落盘图(应用跑起来时写)
**/images/w7_*.png
images/complex/marked*.png
report/*.csv
report/*.log
```

保留它们的理由：应用跑起来时确实会往 `images/` 写算子产物，
那是**运行时行为**，ignore 一条是正常做法。
而 `_test_src.avi` / `_srcframe_*.png` 只有测试会写 ——
测试不再写它们了，规则就没有存在的理由。

**规则删掉之后，从仓库根手动跑测试 exe 的人会立刻在 `git status` 看到产物。**
这正是想要的：那是"你选错了工作目录"，应该被看见，而不是被藏起来。

---

## 六、还剩什么（本轮没做的，列出备查）

| 项 | 现状 | 判断 |
|---|---|---|
| **W5 没有测试套件** | W5_OCR_QRCode 的 `CMakeLists.txt` 里没有 `enable_testing` / `add_test`。它的核心能力（QR / OCR）被 `common/qr_ocr` 的 36 条断言覆盖，但 `main.cpp` 这个壳没有测试 | CI 上线时它只能"能编译"，跑不到任何断言。**要么补一个薄壳测试，要么在 CI 里明说"W5 只编译不测"** —— 别让它看起来像测过了 |
| **应用运行时产物仍进源码树** | W7 应用跑起来时，`color_segment` 等算子会往 `images/` 写 `w7_*.png`；W6 的 `inspect` 会往 `images/` 写 `w6_inspect_*.png` | 这是**应用**行为不是测试行为，ignore 兜着。但它和"images/ 是待检图目录"是同一个坑（见 ② 的笔记），W7 侧还没修 |
| **`ctest -j` 并行** | 现在每套件一份工作目录，理论上可以并行了 | **实测过串行，没实测过 `-j`**。下一轮 CI 上线时应该用 `-j` 跑一次 |
| **仓库根 `images/` 是空目录** | 现在只剩 `.gitkeep` | 保留占位（`.gitignore` 里已经写明） |

---

## 七、核实清单

本文所有数字与输出，均于 2026-10-20 当天在本机实测：

| 论断 | 核实方式 |
|---|---|
| 改动前 6 个 W7 套件 `WORKING_DIRECTORY=${CMAKE_SOURCE_DIR}` | `git show HEAD:W7_VisionMasterLite/CMakeLists.txt` |
| 源码树里 25 个残留文件（W7 14 + 仓库根 11） | `Get-ChildItem` 列目录 + `git status` |
| W6 的 `w6_cli` 挪工作目录后红 1 条 | `ctest -V` 输出 `SUMMARY: pass=161 fail=1` |
| `findRecipeDir` 按相对路径试三个位置 | 读 `W6_PCB_DefectProject/w6_cli.cpp` `kRecipeDirs` |
| M1：测试全绿但源码树脏 5 个文件 | `ctest -R image_sources` + `git status` |
| M2：构建退出码 1 | `cmake --build` 输出 + `$LASTEXITCODE` |
| M3：run_policy 红 2 条 | `ctest -V -R run_policy` |
| M4：pcb_operators 红 36 条 | `ctest -V -R pcb_operators` |
| 还原后 W6 379 / W7 1101，全绿 | `ctest -V` 正则统计 `[PASS]` / `[FAIL]` |
| 跑完全部测试后 `git status` 只有 5 个改动 | `git status --short` |

---

## 八、一句话带走

> **让 `git status` 干净有两条路：往 `.gitignore` 里加规则（藏），或者让测试别往那儿写（修）。**
> 只有第二条是真的。
>
> 删掉 ignore 规则、跑一遍测试、看看 `git status` 变不变 ——
> 这是分辨「问题被解决了」和「问题被藏起来了」最快的一步。
