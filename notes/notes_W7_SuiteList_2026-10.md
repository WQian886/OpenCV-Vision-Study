# 第八笔：套件名单只有一份 + 计数的数不许比真值小而不说话

> 2026-10-26。结项之后还的第八笔。
> 这一笔**不还账**（欠账还是 19 条 / 已收 13 / 未收 6）——
> 它修的是**门禁本身**：门禁当时是**红的**，而红的原因是**测错了地方**。

---

## 1. 一句话

> 2026-10-26 早上跑 `tools/ci.ps1`，**红了**（退出码 1）。
> 而红的不是测试挂了 —— 26 个套件**全部通过**。
> 红的是**「文档里的数和实测对不上」**那一步。
> 查下去发现：**文档是对的，实测是错的** ——
> 计数器只数到了 **18** 个套件，真值是 **26**。

---

## 2. 现象

```
== Step 3c/6  documentation numbers
   FAIL: the per-suite table lists 26 suites, reality has 18
   FAIL: the per-suite table lists a suite 'camera' that does not exist
   FAIL: the per-suite table lists a suite 'dl_infer' that does not exist
   ... (18 处)
   FAIL: the per-suite table adds up to 2371, reality says 1804
DOC NUMBERS FAILED
```

「reality says 1804」这句话就是全部的线索：

- 实测到的套件数 **18**（ctest 自己说跑了 26 个）
- 实测到的断言数 **1804**（真值 2371，少了 **567**）

---

## 3. 根因链条（每一步都实测过，不是推演）

### 3.1 少了哪 8 个

把实测到的 18 个和真值 26 个一对，缺的是：

| 套件 | 断言数 |
|---|---|
| `dl_infer` | 44 |
| `dl_infer_nosdk` | 10 |
| `camera` | 93 |
| `camera_nosdk` | 15 |
| `run_engine` | 111 |
| `result_render_plan` | 62 |
| `frame_verdict` | 90 |
| `headless_runner` | 142 |
| **合计** | **567** |

567 正好等于 2371 − 1804。**不是巧合** —— 是这 8 个套件的输出**压根没进 ctest**。

### 3.2 为什么没进 ctest

这 8 个套件都用 `qInfo()` 打汇总行。例如 `tests/test_run_engine.cpp:904`：

```cpp
qInfo().noquote() << "SUMMARY: pass=" << g_pass << " fail=" << g_fail;
```

而 **Qt 6 在 Windows 上默认把 `qInfo` 送到 Windows 调试通道（`OutputDebugString`），
不是 stderr** —— 于是 ctest 看不见。

实测（直接跑那个 exe）：

```powershell
> ..\..\Release\w7_engine_test.exe
exit=0                       # 一个字都没有
```

加上环境变量就出来了：

```powershell
> $env:QT_FORCE_STDERR_LOGGING='1'
> ..\..\Release\w7_engine_test.exe
================================
通过: 111   失败: 0
================================
SUMMARY: pass= 111  fail= 0   RESULT=PASS
```

`W7_VisionMasterLite/CMakeLists.txt` 里本来就有这个开关 ——
**问题在于它只加在了一部分套件上**（见下一节）。

### 3.3 名单少写了 8 个

`W7_VisionMasterLite/CMakeLists.txt`（改之前）：

```cmake
set_tests_properties(
    undo_redo pcb_operators dl_postprocess image_sources
    step_result pin_and_recent line_fit measurement run_policy detection_operators
    template_match operator_library operator_knobs pure_layer pipeline_graph
    mainwindow_wiring
    PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
)
```

**16 个名字。** 2026-10-24 之后新加的那 8 个套件从没被加进去。

`git blame` 也印证了这一点：这个名单 10-08 建，10-25 加过两次名字
（`template_match` / `operator_library` / `operator_knobs` / `pure_layer` /
`pipeline_graph` / `mainwindow_wiring`），
**但 10-24 新加的那 8 个，一次都没被加过**。

### 3.4 于是计数器默默报了个更小的数

`tools/ci.ps1` 的 Step 3b 数断言数，是从 ctest 的**输出文本**里正则抓的
（`SUMMARY: pass= N` / `pass=N fail=`）。

那 8 个套件的输出没进 ctest → **正则抓不到** → 计数器不知道它们存在。

**而它没有说。**
它报了一句「W7 18 套件 / 1804 断言」，就往下走了。

---

## 4. 决定性的那一步实验

不能只靠「读代码觉得是这样」。做了一次判定实验：

> 把 `QT_FORCE_STDERR_LOGGING=1` 设进**子进程环境**（不改任何代码），
> 再跑 `ctest -V`。

结果：**26 行汇总全部出现**，而且每一行都和 W7 README 的表**逐行对上**：

```
 1: SUMMARY: pass=36   fail=0        (qr_ocr)          表里 36
 6: SUMMARY: pass=44   fail=0        (dl_infer)        表里 44
 8: SUMMARY: pass=93   fail=0        (camera)          表里 93
10: SUMMARY: pass=111  fail=0        (run_engine)      表里 111
13: SUMMARY: pass=142  fail=0        (headless_runner) 表里 142
...
```

⇒ **文档是对的。** 26 套件 / 2371 断言，一行不差。

⇒ 而 Step 3c 那 18 处 FAIL 的**矛头指错了方向**：
它让人去改**文档**里的 2371。
如果照着它改，文档就被改错了 —— 真 bug 被**写进文档**，门禁反而绿了。

> 这是最坏的一种红：**门禁红了，但门禁指错方向。**
> 和 `at-the-verdict-must-be-about-the-value-not-the-name.md` 是同一个病：
> **报出来的不是那件事。**

---

## 5. 修法一：名单用**机器维护的那一份**

第一反应是「把那 8 个名字补进名单」。**那样能修好这一次，但修不好下一次。**

因为那份名单是**手抄的第二份**：

- 套件名单本来就有**一份**：`W7_TEST_SUITES`
  （`w7_add_test()` 每注册一个套件就往里 append 一次，
  本文件开头的函数定义，`:64`；末尾的暂存目标已经在用它，`:1556`）
- 而 `set_tests_properties` 那里**又抄了一份**（16 个名字）

⇒ 这就是本项目栽过十几次的**「抄副本」**，
只不过这次抄的不是数字，是**名单**：

```
抄出来的那份会随真实那一份一起漂，
漂了没人知道 —— 因为计数的那一步不核对总数。
```

**抄出来的名单不会自己报警。**

所以修法是**删掉那份抄件**，改用真的那一份：

```cmake
set_tests_properties(${W7_TEST_SUITES}
    PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
```

改完之后：

- 套件名单在这个项目里**只有一份**，且是**机器维护**的；
- 以后**再**加套件，**不用记得**改任何别的地方；
- 这类漂移**结构上不可能再发生**。

配套卡：`at-extract-it-or-it-is-just-a-second-copy.md`
（抽出来，别复制一份）。

> 顺带：`W6_PCB_DefectProject` 没有这个问题 ——
> 它没有 `set_tests_properties`，三个套件都用 `printf`，
> 实测 3/3 都数到了。**不是每个项目都需要这道闸。**

---

## 6. 修法二：计数的数**不许比真值小**而不说话

改完名单，这一轮就绿了。**但那只是治了这一次。**

真正的问题是：**计数的那一步，在数不全的时候不会说。**

它报了一个**少了一截的数**，而且**看起来很正常**。
Step 3c 拿它去对文档，于是红了 —— 但**红的原因指错了方向**。

所以给 Step 3b 加了一道**交叉核对**：

> **ctest 说跑了 N 个套件，计数器必须看见 N 个。**
> 少看见一个 → 直接红，并**点名是哪几个没看见**。

```powershell
if ($perSuite.Count -ne $ranSuites) {
    Fail ("for $p, ctest ran $ranSuites suites but the counter only saw $($perSuite.Count)." +
          "`n   Suites whose output never reached ctest: " + ($missed -join ', ') + ...
}
```

加上「$suiteNames.Count（ctest 输出里的套件行数）也必须等于 N」
这一层 —— 它防的是**解析器本身**跟输出脱节。

这一步的思路和第七笔是**同一条**：

> **量的那一步，必须能说出「我看见了几个」和「本该有几个」。**
> 两者不等 → **红**，而且要说清差在哪。

第七笔管的是「量出来的数和文档对不上」；
这一笔管的是「**量这一步本身**有没有数全」。

> 没有这一层，第七笔那道门禁**测的是半个世界** ——
> 它只能证明「我看见的那些，和文档对得上」，
> 证明不了「我看见的就是全部」。
> 这是 `at-an-audit-that-cannot-see-starts-lying.md` 的另一种形态：
> **审计看不见的那部分，等于不存在，而且没人知道。**

---

## 7. 为什么这么久没人发现

三个原因叠在一起：

1. **门禁从没被真正跑过。** 仓库**没有 remote**（欠账 9），
   GitHub Actions 从没执行过；本机 `ci-local.ps1` 走的是同一份 `tools/ci.ps1`。
2. **第七笔（10-25）写下 doc-numbers 检查器时，这 8 个套件已经存在了** ——
   也就是说，**从检查器落地那一刻起，它就一直是红的**。
3. **本地跑出来的数字看起来是对的** ——
   单独 `ctest` 一下，26 个套件全过；只有**跑完整 CI** 才会红。

> ⇒ **「跑一遍 CI」和「跑一遍测试」不是一回事。**
> 前者包含「数」和「对」这两步，而这两步才是会红的那些。

这也是欠账 9 的**新证据**：
真 runner 会用一台**没有你本机环境**的机器跑，
而这一轮的红**恰恰就是环境造成的**（本机有没有那个环境变量）。

### 7.1 一句**复现不出来**的历史声明

上一条提交（`4a13e5b`，第七笔）的提交信息里写着：

```
门禁:tools/ci.ps1 -> ALL CHECKS PASSED
  (29 套件全过,四种退出码全对,
   **Step 3c 第一次实跑就通过**(它此前一次都没跑过),
   源码树零污染)
```

**这句话复现不出来。**

2026-10-26 在**未做任何改动的**树上跑 `tools/ci.ps1`：
**退出码 1**，红在 Step 3c，原因是「18 套件 / 1804 断言 ≠ 26 / 2371」。

⇒ 也就是说：**那道门禁从落地那一刻起就是红的**，
而提交信息里记的是绿的。

为什么当时没被发现，大概率是：
那三处漂移是在**单独**跑 `tools/check-doc-numbers.ps1` 时逮到的
（`notes_W7_DocNumbers_2026-10.md` 4.1 记的就是那个），
而那个检查器需要一份**逐套件计数文件**——
它当时很可能是**手工**整理出来的，
于是 26 个套件都在里面，**检查器就绿了**。

⇒ **手工喂给检查器的那份输入，绕过了「数」这一步。**
⇒ 而「数」这一步正是出问题的那一步。

> 这也是本项目的老教训，换了个马甲：
> `at-the-expected-value-must-be-read-not-copied.md`
> 说的是「期望值别抄一份」，
> 这里是「**实测值**也别**手工整理**一份」——
> 手工整理的那份，**绕过了产生它的那道工序**。
>
> ⇒ 而这一笔的修法，正好是把它**接回**那道工序：
> `ci.ps1` 从**零构建**跑起，计数**只从刚跑完的那棵树**里取。

---

## 8. 教训

> **凡是「数出来的东西」，都要能回答「本该有几个」。**
>
> 少算几个和全算对，**在数字上长得一模一样** ——
> 除非那一步**自己核对**。
>
> ⇒ 而少算的那部分，**恰恰是最容易出事的部分**：
> 这一轮少算的 8 个套件，正好是**最新加的那批**。

推论一：

> **手抄的名单不会自己报警。**
> 名单要**机器维护**（`${W7_TEST_SUITES}`），
> 不要人维护（16 个名字）。

推论二：

> **门禁红了，先确认门禁指的方向对不对。**
>
> 这次它指错了 —— 让人去改**文档**里的 2371。
> 照着改，**真 bug 就被写进文档了**，门禁反而绿了。
>
> ⇒ 「红」和「红的原因对」是两回事。
> ⇒ 而要判断方向对不对，前提是**你知道真值**——
> 所以「量的那一步要能说清它看见了几个」不是洁癖，是**必需品**。

推论三：

> **「没人跑过」和「跑过并且是绿的」长得不一样，但都看起来像「没问题」。**
> 这一轮的门禁**一次都没被跑过**（从落地起就是红的），
> 而它在文档里被写成了「当场逮到三处真漂移」。

配套卡：`at-the-counter-must-not-undercount-silently.md`
（数出来的那一步也会数不全，而且它不说话）、
`at-the-total-can-hide-a-wrong-part.md`（总数对得上，分部可以对不上）、
`at-an-audit-that-cannot-see-starts-lying.md`（认不出就开始撒谎）、
`at-the-expected-value-must-be-read-not-copied.md`（期望值要读，不要抄）。

---

## 9. 这一笔改了什么

| 文件 | 改了什么 |
|---|---|
| `W7_VisionMasterLite/CMakeLists.txt` | `set_tests_properties` 的名单从**手抄 16 个**改成 **`${W7_TEST_SUITES}`**（机器维护的那一份） |
| `tools/ci.ps1` | Step 3b 新增**套件数交叉核对**：ctest 跑了几个，计数器就必须看见几个，否则红并点名 |
| `README.md` | 欠账数 **12 / 7 → 13 / 6**（和权威总账对齐）；「26 个套件都设了」那句话从**假的**变成**真的** |

**欠账没变**：19 条 / 已收 13 / 未收 6。
这一笔修的是**门禁**，不是账 —— 但门禁不修，账也还不干净。

---

## 10. 怎么复现这一笔

```powershell
# 一条命令跑完整门禁
powershell -ExecutionPolicy Bypass -File tools\ci.ps1
```

要**故意**复现这次的漏数，把
`W7_VisionMasterLite/CMakeLists.txt` 里那行改回手写名单
（去掉 8 个名字），重跑一次 ——
你会看到 Step 3c 的 18 处 FAIL，而且**文档一个数都没错**。

更直接的观察方式（不改任何代码）：

```powershell
# 没有这个环境变量 → 那 8 个套件一个字都不打
$env:QT_FORCE_STDERR_LOGGING = '1'   # 有了 → 26 行汇总全出来
```

---

## 参见

- `notes/notes_W7_DocNumbers_2026-10.md` —— 第七笔（把「数字对不对得上」变成机器可查的一步）
- `notes/notes_W7_Close_2026-10.md` —— W7 结项报告（欠账总账、怎么继续）
- `notes/notes_W7_Knob_Audit_2026-10.md` —— 旋钮审计（欠账的**唯一登记处**，第十节）
- `notes/atomic/at-the-counter-must-not-undercount-silently.md` —— 这一笔的教训卡
