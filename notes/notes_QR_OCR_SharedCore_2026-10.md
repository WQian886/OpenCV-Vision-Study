# QR / OCR 合并成共享核心（第 5 次抄副本）（2026-10-20）

> 📅 **记录时间**：2026-10-20
> 📁 **项目来源**：W5_OCR_QRCode / W6_PCB_DefectProject / W7_VisionMasterLite / common/qr_ocr
> 🔬 **核实方式**：本篇所有技术论断均于 2026-10-20 当天编译运行 / 读源码确认，核实清单见文末
> 🎯 **本次目标**：把 W5 / W6 / W7 各写一份的 QR 解码与 tesseract 调用合成一份，并给这段「零覆盖的代码」补上自动化测试

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

QR 解码和 tesseract 调用在仓库里有**三份**拷贝（W5 一份、W6 一份、W7 一份），
而且**从来没有被放在一起测过** —— 于是连"W6 的 `--psm 11` 和 W5 的 `--psm 6` 哪个对"
这个问题都答不上来：它们是两份拷贝，不是**一份**的两个参数。
合并到 `common/qr_ocr/` 之后，psm 变成了一个参数，
而且**三份代码第一次有了测试**；顺手还逮到 W6 一个真缺陷 ——
**OCR 的失败原因算出来就直接丢掉了**，报告里 silk 列空着，
和"板上没丝印"长得一模一样。

---

## 一、抄了几份，各自差在哪

### 1.1 【实测】三份拷贝的位置

| 位置 | QR 解码 | tesseract 调用 |
|---|---|---|
| `W5_OCR_QRCode/main.cpp` | ✅ | ✅ |
| `W6_PCB_DefectProject/w6_cli.cpp` | ✅ | ✅ |
| `W7_VisionMasterLite/operators/qr_decode.cpp` | ✅ | ❌ |

### 1.2 【实测】三份已经漂开了

漂开的方向不是"写法风格不同"，是**行为不同**：

| | W5 那份 | W6 那份 |
|---|---|---|
| `--psm` | **6**（整块文字）| **11**（散落文字）|
| 宽字符 → 窄字符 | `std::wstring(prePath.begin(), prePath.end())` —— **逐元素窄化** | 走 `WideCharToMultiByte` ✅ |
| 放大倍数 | **没有**（直接扔给 tesseract）| 3 倍 ✅ |
| 中间产物落在 | `images/`（**待检图目录**）| `out/silk/` ✅ |

两处是**真 bug**，不是风格差异：

- **逐元素窄化**：路径里带中文就毁掉。W6 那份修好了，W5 那份没有。
  ——修一处、另一处照旧坏着，而且没人会怀疑它们本来应该一样。
- **中间产物落在 `images/`**：那是批量报告的**输入目录**。
  下一次扫描会把自己的中间产物当成待检板扫一遍；
  而它没有 manifest 登记，于是被报成"跳过" —— **看着像少检了一块板**。

### 1.3 【实测】为什么没人发现

因为**三份都没有测试**。W6 的旧注释写着：

> `readSilkscreen` **没被自动测**（要起外部进程，慢且依赖环境）

这句话是事实，但它的推论是：
**tesseract 没装 / 跑不起来的时候，程序是明说了还是悄悄返回空串？没人知道。**

---

## 二、合并成什么样

### 2.1 【实测】新目录 `common/qr_ocr/`

```
common/qr_ocr/
├── qr_ocr.h            # 接口(无 Qt 依赖)
├── qr_ocr.cpp          # 实现
└── tests/
    └── test_qr_ocr.cpp # 36 条断言
```

接口只有两个函数：

```cpp
QrResult  readQr(const cv::Mat& img);                    // found / text / corners
OcrResult readText(const cv::Mat& img, const OcrOptions& opt = {});
```

`OcrOptions` 里带 `psm` / `scale` / `lang` / `tesseractPath` / `workDir` / `stem`。

### 2.2 【实测】psm 变成了参数

| 调用方 | psm | 为什么 |
|---|---|---|
| W5 | **6** | 测试图是三行整整齐齐的字 |
| W6 | **11** | 板丝印是**散落的**（板号在一角、SN 在二维码下、每个元件上面还有位号）|

合并之前，这两个数分别硬编码在两份拷贝里，
**谁也说不清哪个对** —— 因为它们不是一份实现的两个参数。
合并之后，改一处两边跟着动，而且两个值都有测试钉着。

### 2.3 【实测】"没读"和"读了但是空"必须分得开

这是本模块对外最重要的契约：

| 情况 | `OcrResult::ok` | `OcrResult::text` | `OcrResult::error` |
|---|---|---|---|
| 读到了 | `true` | 内容（可能为空）| 空 |
| 没读到 | **`false`** | 空 | **人话**：哪一步失败、为什么 |

三种结果在报告里长得一模一样（都是空字符串），
所以失败原因必须**另外报出去** —— 见 3.2。

### 2.4 【实测】合并后 silk 列的**空白**变了（内容没变）

旧 W6 拼接 OCR 结果的写法：

```cpp
while (std::getline(f, line)) {
    if (!all.empty()) all += " ";
    while (... 行尾的 \r 和空格 ...) line.pop_back();
    all += line;          // ← 空行也照样拼进去
}
```

于是 tesseract 输出的**空行**也占了一个分隔符，
拼接结果里出现**双空格**：

    旧: MCUICore WIEST M3 2B 0SIOR P64)  | °  |  ma  aa  oe  a  mae,  ee  aa  ...
    新: MCUICore WIEST M3 2B 0SIOR P64) | ° | ma aa oe a mae, ee aa ...

**识别的内容一个字没变**，只是行与行之间的空白从两个变成一个。

为什么改：双空格不是"格式约定"，是"空行也占了分隔符"的副作用。
共享实现里改成**跳过空行**，拼接结果更干净。

> 📌 这种"顺手改了一点"的行为变化必须**写出来**，
> 否则下一个看 CSV diff 的人会以为出了什么问题。
> 写清楚"内容没变、只是空白"和"内容变了"，是两回事。

---

## 三、顺带逮到的两个真问题

### 3.1 【实测】W6 把 OCR 的失败原因**算出来就丢掉**

原来的代码：

```cpp
std::string qr, silk, silkErr;
if (!img.empty()) {
    qr = readQRCode(img);
    if (doSilkscreen) silk = readSilkscreen(img, silkErr);
    //  ← silkErr 算出来了,之后再也没被用过
}
```

失败原因被算出来，塞进一个变量，然后**直接丢掉**。
报告里 silk 列空着 —— 和"板上没丝印"分不开。

修法：

- W6 的 `BoardCodes` 里多了 `silkRead`（有没有读）和 `silkError`（为什么没读）
- 批量报告多了 **`readFailures`** 通道，和 `skipped` **分开**

### 3.2 【实测】为什么 `readFailures` 不能混进 `skipped`

| | 含义 | 后果 |
|---|---|---|
| `skipped` | 这块板**没判**，结论不成立 | 少检了板 |
| `readFailures` | 板判了，只是丝印那一栏是空的 | 那一栏没读到 |

混在一起的后果：看报告的人以为"少检了一块板"，
去查板型、查配方、查真值表 —— 而真正的问题是 **tesseract 没装**。

查错方向比查错问题本身还费时间。

### 3.3 【实测】`main.cpp` 的单板模式也改了

以前单板模式打印 `(空 —— 原因)`，但那个 `silkErr` 在 `main.cpp` 里
**同样是接了就没用**（它只在 `silk.empty()` 时被拼一下，
而 `readSilkscreen` 失败时返回的是空串 —— 碰巧能显示出来，
但"开关关着"和"读失败"两件事共用一个变量）。

现在分成两句话：

```
  丝印:      ⚠ 没读到(指定的 tesseract 路径不存在: D:/xxx —— 不会悄悄换一个来跑)
  丝印:      (空 —— 读到了,图上没字)
```

---

## 四、测试：第一次给这三份代码装上牙齿

### 4.1 【实测】`common/qr_ocr/tests/test_qr_ocr.cpp`（36 条断言）

| 组 | 钉的是 |
|---|---|
| QR | 内容对（期望值来自 `gen_qr.py`）、4 个角点、空白图不崩、空 Mat 不崩 |
| 找 tesseract | **显式指定的不存在路径 -> 返回空串，不悄悄换成默认的** |
| OCR | 读出三行字、错误消息为空、预处理图和输出落在 `workDir`、命令行带 `--psm` |
| 失败路径 | 空图 -> `ok=false`；指定路径不存在 -> `ok=false` 且错误消息里写明路径 |
| 目录隔离 | 中间产物**不许**落在当前目录 |

### 4.2 【实测】期望值从哪来

- QR 的内容来自 **W5 自己的生成器** `gen_qr.py`
  （`qr.add_data("PCB-2026-001-SN:A12345X")`）—— 不是在测试里抄一份。
  抄的那份会跟生成器漂开。
- OCR 的三行字来自 `gen_ocr_test.py` 画的
  `R123 100R` / `C456 10uF` / `PCB-2026-001`。

### 4.3 【实测】tesseract 没装怎么办

本机**装了** tesseract（`D:\Program Files\Tesseract-OCR\tesseract.exe`），
所以 OCR 那几条真的跑了。

但测试写成了**环境缺东西就跳过、并且把跳过的条数明写在 SUMMARY 里**：

```
SUMMARY: pass=36 fail=0 skip=0   RESULT=PASS
```

跳过时是这样：

```
  [SKIP] 读出文字 / 中间产物落盘 / 报错为空 —— 本机没装 tesseract,跑不了真实 OCR
...
注意:有 3 条因为环境缺 tesseract 被跳过 —— 这些**没验过**,不是验过了
```

> 📌 **跳过必须报出来。** 一条测试因为环境缺东西而没跑，
> 如果不写出来，看报告的人会以为"这条已经验过了" —— 那比没有还坏。

### 4.4 【实测】同一份测试源码链进 W6 和 W7

理由和 `pcb_core` 一样：

- W6 那边是 **Ninja + 无 Qt** —— 证明核心能脱离 Qt 单独用（这正是 W6 的用法）
- W7 那边是 **VS 生成器** —— 证明核心换个构建环境也没坏

两边都绿才说明"核心自己是对的"。

---

## 五、变异验证：4 个变异，4 个都红

| 变异 | 预期 | 实测 |
|---|---|---|
| M1 `readQr` 恒返回 `found=false` | QR 断言红 | ✅ **杀**：qr_ocr 红 2 条 |
| M2 `findTesseract` 显式路径不存在时**偷偷回退**到默认 | "不偷换"断言红 | ✅ **杀**：qr_ocr 红 4 条 + **W6 CLI 红 5 条** |
| M3 找不到 tesseract 时 `ok=true`（假装成功）| 失败路径断言红 | ✅ **杀**：qr_ocr 红 1 条 + **W6 CLI 红 2 条** |
| M4 默认 `workDir` 改成当前目录 | 目录隔离断言红 | ✅ **杀**：qr_ocr 红 2 条 |

### 5.1 【实测】M2 特别值得记

「显式指定的 tesseract 路径不存在，就悄悄换成默认的那个去跑」——
这个变异**两边的测试都抓到了**。

因为它是最阴险的一类退化：
**拿另一个程序跑出来的结果，调用方却以为是自己指定的那个。**
调用方完全看不出来，只有结果对不上时才会怀疑。

所以 `findTesseract` 的契约写死在头文件里：
**显式指定就只认显式指定，找不到就返回空串，不许换一个。**

### 5.2 【实测】M4 复测暴露的第二个问题：**测试自己制造的失败**

M4 跑完在构建目录里留下了两个文件。把它还原之后**再跑一次** ——
**测试还是红的**，而且红的原因和当次改动毫无关系：

```
  [FAIL] 中间产物没有落在当前目录
```

这就是「**测试自己制造的失败**」：一次坏掉、**永久变红**。
比 flaky 还难查 —— flaky 至少每次随机，这个是**一次性的、永久的**。

修法很朴素：**跑之前先清掉上一轮跑剩的同名文件**。
改完之后复测：M4 生效时红 2 条并留下残留 → 还原 → **不手工清理** → 36/36 全绿。

---

## 六、还改了哪些文件

| 文件 | 改动 |
|---|---|
| `common/qr_ocr/qr_ocr.h/.cpp` | **新增**。共享的 QR / OCR 实现 |
| `common/qr_ocr/tests/test_qr_ocr.cpp` | **新增**。36 条断言 |
| `W5_OCR_QRCode/main.cpp` | 删掉本地实现，改调共享核心；加 `--no-gui`；中间产物从 `images/` 挪到 `out/ocr/` |
| `W5_OCR_QRCode/CMakeLists.txt` | 链 `common/qr_ocr` |
| `W6_PCB_DefectProject/w6_cli.h/.cpp` | 删掉 `readSilkscreen` / `readQRCode`；新增 `BoardCodes` / `readBoardCodes` / `ReadFailure` / `readFailures` |
| `W6_PCB_DefectProject/main.cpp` | 用 `readBoardCodes`；打印读取失败 |
| `W6_PCB_DefectProject/tests/test_w6_cli.cpp` | 新增 2 组用例（16 条断言）|
| `W7_VisionMasterLite/operators/qr_decode.cpp` | 删掉本地实现，改调共享核心 |
| 三个 `CMakeLists.txt` | 链接共享核心 + 注册 `qr_ocr` 套件 |

### 6.1 【实测】W5 保留了教学演示，删掉了定义

W5 的 `main.cpp` 仍然打印每一步、画框、显示窗口 ——
「预处理长什么样」本身就是这周要学的东西。

但**定义**（预处理怎么算、tesseract 怎么调、结果怎么读）搬走了。
W5 现在打印的是共享核心**回传**的命令行：

```
  调用: "D:\Program Files\Tesseract-OCR\tesseract.exe" "out/ocr\w5_ocr.png" "out/ocr\w5_ocr" -l eng --psm 6
```

这比原来在 W5 里手写一遍更有教学价值 ——
它展示的是**系统实际执行的那一行**。

### 6.2 【实测】W5 加了 `--no-gui`

`cv::imshow` 在没有窗口系统的环境（CI、容器）里会抛异常。
W6 早就有 `--no-gui`，W5 现在也有：

```
w5_ocr_qrcode.exe ocr --no-gui
w5_ocr_qrcode.exe qr  --no-gui
```

这是为下一阶段（上 CI）铺路的。

---

## 七、还没修的 / 已知限制

| 缺口 | 状态 |
|---|---|
| 丝印 OCR 在合成板图上读不准 | **仍然不准**。板上的字是 14~18px 的白字，放大 3 倍 + 自适应阈值后仍只能读出大概（"MCUICore WIEST M3 2B 0SIOR P64)"）。**QR 串号是准的**。实拍图上效果会好很多。|
| W5 的 `--psm 6` / W6 的 `--psm 11` | 现在都是**参数**了，但「哪个更适合板丝印」这个问题**仍然没有数据支撑** —— 合成板图上的丝印本来就读不准，没法用来比较。换真实板图才能定。|
| 非 Windows | `readText` 只有 Windows 实现（`CreateProcessW`）。非 Windows 上返回 `ok=false` 并说明原因。|

---

## 八、核实清单

| 论断 | 核实方式 |
|---|---|
| 三份拷贝的位置 | grep `QRCodeDetector` / `tesseract` 全仓 |
| W5 用 `--psm 6`、W6 用 `--psm 11` | 读旧源码 |
| W5 的逐元素窄化 / W6 的 WinAPI 转换 | 读旧源码（W6 的 `wideToUtf8`）|
| 中间产物落在 `images/` | 读 W5 旧源码 `images/ocr_preprocessed.png` |
| W6 把 OCR 失败原因丢掉 | 读旧 `w6_cli.cpp`：`silkErr` 赋值后再无引用 |
| tesseract 装了 | `D:\Program Files\Tesseract-OCR\tesseract.exe` 存在 |
| tesseract 对测试图的输出 | 实跑：`R123 100R` / `C456 10uF` / `PCB-2026-001`（psm 11 和 6 一致）|
| 合并后 W6 报告的 QR 列与合并前逐字一致 | 实跑 `report images/complex`，CSV 的 qr 列逐字相同 |
| 合并后 W6 报告的 silk 列**空白**变了(内容没变) | 见 2.4 |
| W5 合并后实跑正常 | `w5_ocr_qrcode.exe ocr --no-gui` / `qr --no-gui` |
| M1~M4 全杀 | 变异脚本：改坏 → 编译 → 跑测试 → 还原 |
| M4 残留导致"永久变红" | M4 → 还原 → **不手工清理** → 复测（修前红 / 修后绿）|

### 8.1 断言数

| 套件 | 之前 | 之后 |
|---|---|---|
| `qr_ocr`（W6 + W7 共用同一份源码）| — | **36**（新增）|
| `w6_cli` | 142 | **163**（+21）|
| `pcb_core` | 180 | 180 |
| **W6 合计** | 2 套件 / 322 | **3 套件 / 379** |
| **W7 合计** | 13 套件 / 1065 | **14 套件 / 1101** |
| **全仓合计** | 15 套件 / 1387 | **17 套件 / 1480** |
