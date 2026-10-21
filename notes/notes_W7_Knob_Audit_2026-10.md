# W7 旋钮审计：把「界面上摆着的旋钮」和「背后那段代码」对上

> 2026-10-21
>
> 这一轮做两件事：
> **A2** 把算子旋钮的默认值收成唯一一份；
> **A3** 加一条「旋钮审计」——每个旋钮都得对得上，
> 对不上就报出来。
>
> **同一天第二轮**（本文第六节）：把审计从"只查 `defaultParams()`"
> 扩到**三处**（声明 / 兜底 / 面板初值），
> 顺手关掉欠账 6（审计扫的源码必须是产品真在编译的那份），
> 欠账 5 收掉一半（"为什么不能用"那句话现在有断言盯着）。

---

## 一、为什么要做

起因是一个很朴素的疑问：**W7 的参数面板上，有多少个旋钮是"调了不生效"的？**

脚本扫完全部 16 个算子，答案是：

| 旋钮 | 声明的默认值 | `run()` 里的兜底 | 面板上的初值 |
|---|---|---|---|
| `blob_count.min_area` | 20 | **50** | 20 |
| `count_check.expected_min` | 1 | **0** | 1 |
| `count_check.expected_max` | 100 | **999** | 100 |
| `circle_fit.min_area` | 100 | 100 | 100 |
| `circle_fit.min_circularity` | 0.70 | 0.70 | 0.70 |
| `template_match.threshold` | 0.9 | 0.9 | 0.9 |
| `dl_infer.conf_threshold` | 0.25 | 0.25（编译期停用分支） | 0.25 |
| `dl_infer.nms_iou` | 0.45 | 0.45（编译期停用分支） | 0.45 |
| `image_source_camera.exposure` | 10.0 | 10.0 | 10.0 |

**三处已经漂了**，而且漂的方式正好是最阴的那一种：

- `blob_count.min_area`：参数 JSON 里缺这个 key 时，算子按 **50** 滤，
  面板上写的是 **20**。用户调的值和算子用的值不是一回事。
- `count_check.expected_min/max`：兜底是 **0 和 999**，
  而声明的是 **1 和 100**。`0..999` 几乎等于"不判定"——
  上游给什么数都判合格。
- `pcb_inspect`：`defaultParams()` 登记的是 `min_area = 80`（绝对像素），
  而 `run()` 读的是 `min_area_pct`（占全图 %）。
  **登记的和读的不是同一个 key**，于是：
  - `min_area` 成了**死旋钮**（面板上写着、调了不生效）
  - `min_area_pct` **从来没登记过** —— 它的默认值只活在 `run()` 的兜底里，
    写进流水线的参数 JSON 里压根没有这个 key。

> 「同一个数抄多处，改的时候一起改」这句话是**许愿**，不是工程。
> 本项目已经在这条路上栽过五遍（见 `notes/atomic/at-four-copies-of-one-number.md`）。
> 这一轮的做法是：**让第二份不存在**。

---

## 二、A2：默认值收成唯一一份

新建 `W7_VisionMasterLite/operators/operator_defaults.h`：

```
namespace w7 {
struct CircleFitDefaults {
    static constexpr int    kMinArea        = 100;
    static constexpr double kMinCircularity = 0.70;
};
...
}
```

一个算子一个 struct，成员就是它的旋钮默认值。
**16 个算子的全部默认值都在这一个文件里**（共 59 个常量）。

三个使用点全部改成引用它：

| 位置 | 原来 | 现在 |
|---|---|---|
| `defaultParams()` | `p["min_circularity"] = 0.70;` | `= CircleFitDefaults::kMinCircularity;` |
| `run()` 兜底 | `params.value("min_circularity").toDouble(0.70)` | `.toDouble(CircleFitDefaults::kMinCircularity)` |
| 面板 `buildUI()` | `m_minCircularity->setValue(0.70)` | `.setValue(CircleFitDefaults::kMinCircularity)` |

改的不是"记得一起改"，是**没有第二份可改**。

### 2.1 「读不到就用声明的默认值」这条规则补齐了 5 处

`run()` 里有几处 `params.value("x").toString()` **没有兜底** ——
key 缺失时得到空串，和声明的默认值不是一回事：

| 算子 | key | 补上的兜底 |
|---|---|---|
| `dl_infer` | `model_path` | `models/yolov8n.onnx` |
| `dl_infer` | `class_names` | COCO 类名表（原来**完全没有**兜底，缺 key 时框上只剩 `cls3` 这种编号） |
| `image_source_camera` | `camera_ip` | `192.168.1.10` |
| `image_source_dir` | `dir_path` | `images` |
| `image_source_video` | `video_path` | `images/sample.avi` |
| `template_match` | `template_path` | `templates/R.png` |

### 2.2 必填旋钮：**不**补默认值

有两处**故意不补**：

- `color_segment` 的 `hsv_low` / `hsv_high`
- `pcb_inspect` 的 `hsv_h_low` / `hsv_h_high` / `hsv_s_low` / `mask_v_mul`

这两个区间**就是**用户要调的东西，没有它们等于没配置。
补一个默认值会让用户以为自己的配置生效了 ——
那正是 `operators/hsv_params.h` 开头警告的
「配置看着对、结果全错」。两边都有测试钉着
（`test_detection_operators.cpp`「参数完全为空 → 报 error」、
`test_pcb_operators.cpp`「H 上界缺失 → 报错」）。

**必填与可选要分开**：全当必填，少个 key 整条算子罢工；
全当可选，用户填错的值会被悄悄换成默认值。

### 2.3 编译期停用的算子：旋钮置灰 + 写明原因

`dl_infer`（ONNX Runtime）和 `image_source_camera`（海康 MVS SDK）
没装 SDK 时，那部分代码**根本不参与编译**。
面板上那些旋钮当前**一个都不生效** —— 摆着能调但没用的旋钮 = 骗人。

做法：

1. 两个算子各加 `static bool runtimeAvailable()` 和
   `static QString runtimeUnavailableReason()`；
   `reason` 里写清楚**怎么修**（去哪下载、解压到哪、设哪个环境变量）。
2. 运行时缺失时，面板的旋钮**置灰**，并把原因显示出来。
3. `reason` 这一份字符串同时给三处用（面板提示 / `Run()` 时的 error /
   审计套件的断言）—— 只有一份，不会出现"面板说 A、Run 说 B"。

> ⚠ 这一步**没有自动化测试**：置灰是 QtWidgets 的行为，
> 无头测试构造不了真窗口。能测的是 `runtimeAvailable()` 本身
> 和审计里"豁免清单不能过期"那条断言。

---

## 三、A3：旋钮审计

新建 `W7_VisionMasterLite/tests/test_operator_knobs.cpp`
（第一轮 168 条断言，第二轮加到 **290 条**），
链进全部算子，接进 ctest（套件名 `operator_knobs`）。

六条用例，各自的能力边界都写在用例的注释里：

| 用例 | 查什么 | 手段 | 边界 |
|---|---|---|---|
| 0 | 审计本身没有空转 | 断言扫到的算子数 ≥ 15、旋钮数 ≥ 70 | —— |
| 1 | 死旋钮 | 源码 lint：登记了但当前构建的 `run()` 没读 | 只认 `params.value("x")` 这种读法 |
| 2 | 没登记的旋钮 | 源码 lint：`run()` 读了但 `defaultParams()` 没登记 | 同上 |
| 3 | 默认值只有一份 | 源码 lint：`defaultParams()` 里不许有裸字面量；引用的 `Defaults` 常量必须真的在 `operator_defaults.h` 里；全仓库只有那一个文件能定义 `Defaults` 结构体 | 第一轮只查 `defaultParams()` |
| 3d | 兜底不许抄第二份 | 源码 lint：`run()` 里 `params.value("x").toXxx(EXPR)` 的 EXPR 不许有裸字面量；引用的 `Defaults` 必须是本算子的 | 第二轮加；只看 `toXxx` 的参数 |
| 3e | 面板初值不许抄第二份 | 源码 lint：`buildUI()` 里 `setValue(EXPR)` 的 EXPR 不许有裸数字 | 第二轮加；`setRange` 等量程不管 |
| 3f | 字符串型默认值得露在面板上 | 源码 lint：字符串型默认值对应的常量，`buildUI()` 必须引用同一个 | 第二轮加；只要求"出现"，不保证是控件初值 |
| 4 | 豁免清单不能过期 | 问算子自己的 `runtimeAvailable()` | —— |
| 5 | 空参数 ≡ 声明的默认参数 | **行为测试**：两种跑法结果必须一样 | 只有"输出里看得出差别"的算子才被这条抓住 |
| 6 | 关键默认值符合真值表 | 期望值写在测试里（基准不来自被测对象） | —— |
| 7 | 审计扫的源码 = 产品真在编译的源码 | 源码 lint：磁盘上的 `operators/*.cpp` 与 `CMakeLists.txt` 的 `set(SOURCES ...)` 双向核对 | 第二轮加；只到"文件清单"这一层 |
| 8 | 运行时缺失必须说得出为什么 | 问算子自己的 `runtimeUnavailableReason()` | 第二轮加；置灰本身仍测不了 |

### 3.1 用例 1 的"当前构建"是什么意思

`dl_infer` 有两支 `run()`，只有一支参与编译。
审计要判"**这个构建里**读不读得到"，就得知道预处理分支走哪一支。

做法：自己算一遍 `#ifdef / #ifndef / #else / #endif`，
把不参与编译的行**清空**（保留行数），再在剩下的源码里找函数体。

而"这个构建有没有装 ONNX Runtime"不靠猜 —— **直接问算子自己**
（`DlInfer::runtimeAvailable()`）。问错的话分支整个认反，
比没有审计更坏。

### 3.2 豁免清单会互相校验

```cpp
static const Exemption kExemptions[] = {
    {"dl_infer",
     {"input_w", "input_h", "conf_threshold", "nms_iou", "class_names", "output_layout"},
     "本机没装 ONNX Runtime(环境变量 ONNXRUNTIME_DIR 未设置):"
     "真实推理路径整段在 #ifdef HAS_ONNXRUNTIME 里,没编进来;..."},
};
```

- **漏登记** → 用例 1 报红（有人新加了个停用分支，忘了登记）
- **过期了**（SDK 装上了，旋钮其实已经生效）→ 用例 4 报红（该删这条）

所以这份清单不会自己烂掉。

---

## 四、这一轮最值钱的发现：审计自己一开始是**恒真的空断言**

第一版审计套件，**全绿**。

但它其实**一个算子都没扫到**：

```
DEBUG 扫到 0 个算子源文件
```

三个 lint（用例 1/2/3）全部作用在空集合上，
于是"没有死旋钮"、"没有没登记的旋钮"、"默认值没有裸字面量"
—— **三条断言全是真的，但全是废话**。

这就是本项目记过的第 12 条教训的**升级版**：
守卫写成"打不开就 `return`"，它就变成恒真的空断言。
而这次更糟一层：**空断言长得很像通过**。

### 4.1 三个 bug，一个比一个隐蔽

**① 限定符卡在中间，函数体找不到**

```cpp
QJsonObject CountCheck::defaultParams() const {
                                       ↑ 这里有个 const
```

第一版 `functionBodies()` 只跳空白，遇到 `const` 就以为"这不是定义"，
于是 **17 个算子一个都没扫到**。

**② 算子名在头文件里，不在 .cpp 里**

```cpp
// count_check.h
QString name() const override { return "count_check"; }
// count_check.cpp —— 压根没有这一行
```

第一版在 .cpp 里找 `name()`，自然找不到。

**③ for 循环的自增吃掉了一个右括号**

```cpp
QJsonObject ImageSourceCamera::run(const QJsonObject& params,
                                    const QJsonObject& /*inputs*/) {
```

去掉块注释后，`for` 循环的自动 `++i` 会把 `*/` 后面的 `)` 一起吃掉：

```
run(const QJsonObject& params, const QJsonObject&  {      ← 右括号没了
```

括号配对数错 → 整个 `run()` 函数体找不到 →
**相机的三个旋钮全被误判成"死旋钮"**。

> 同一个逻辑的 Python 复刻是**对的** —— Python 的 `while` 循环不会自动自增。
> **复刻不等于等价**：两份实现连"自增在哪"都不一样。

### 4.2 修法：给审计自己加守卫

用例 0 就是为这次写的：

```cpp
check(ops.size() >= 15, "扫到至少 15 个算子源文件 —— 扫不到就是审计没在工作");
check(total >= 70,   "一共扫到至少 70 个旋钮 —— 解析器坏了的话这个数会掉下去");
```

外加每条 lint 开头都有：

```cpp
if (ops.isEmpty()) {
    check(false, "没扫到任何算子 —— 这条审计等于没跑(恒真的空断言)");
    return;
}
```

> **断言也会骗人**，如果它断言的东西本身是空的。
> 所以断言的对象不只是"代码的行为"，还包括"审计自己的输入"。

---

## 五、变异验证：8 个，全杀

基线：旋钮审计套件 **168 条断言**。

| # | 变异 | 谁抓到 | 红了几条 |
|---|---|---|---|
| M1 | `blob_count` 的兜底抄回 `50`（漂移） | 用例 5 行为测试 | 2 |
| M2 | `count_check` 登记一个 `run()` 从不读的 `dead_knob` | 用例 1 死旋钮 | 1 |
| M3 | `count_check` 不登记 `expected_min` | 用例 2 未登记 + 用例 6 真值表 | 2 |
| M4 | `blob_count` 的默认值抄回裸字面量 `20` | 用例 3 裸字面量 | 1 |
| M5 | 在 `count_check.cpp` 里再抄一份 `Defaults` 结构体 | 用例 3「只有一处能定义」 | 1 |
| M6 | `operator_defaults.h` 里 `kMinArea` 20 → 50 | 用例 6 真值表 | 1 |
| M7 | 豁免清单里删掉 `dl_infer.conf_threshold` | 用例 1（它变成真死旋钮） | 1 |
| M8 | `DlInfer::runtimeAvailable()` 无条件返回 `true` | 用例 4 豁免过期 | 2 |

**8/8 杀掉。**

M8 值得单说：它模拟的是"有人装了 ONNX Runtime"，
于是豁免清单过期 —— 审计立刻要求把那条豁免删掉。
**清单自己也会烂，必须有人盯着。**

> 第一轮跑这 8 个变异时，M2/M4/M7 三个**活着**。
> 原因不是断言写得不好，是**审计根本没在工作**（第四节的三个 bug）。
> 教训：**变异活下来，先问"尺子量到了吗"，再问"断言写得对吗"。**

---

## 六、第二轮：审计扩到三处（欠账 1 收口）+ 欠账 6 收口

上一轮的审计只查 `defaultParams()` ——
但同一个默认值在源码里有**三个位置**：

| 位置 | 写的是什么 | 它决定的是 |
|---|---|---|
| `defaultParams()` | 写进流水线 JSON 的那份 | 参数 JSON 里的值 |
| `run()` 兜底 | `params.value("x").toInt(EXPR)` 里的 `EXPR` | key 缺了时算子按什么跑 |
| 面板 `buildUI()` | `setValue(EXPR)` 里的 `EXPR` | 界面上写着的那个数 |

第二、三个当时**没人看**。
这一轮把它们纳入审计，顺手把欠账 6 也关掉。

### 6.1 新增的三条 lint

| 编号 | 查什么 | 怎么查 | 边界 |
|---|---|---|---|
| 3d | 兜底不许抄第二份 | 扫 `run()` 里 `params.value("KEY").toXxx(EXPR)`：EXPR 不许有裸字面量；引用的 `*Defaults::` 必须是**本算子**的结构体 | 只看 `toXxx(...)` 的参数，套在外面的运算看不见 |
| 3e | 面板初值不许抄第二份 | 扫 `buildUI()` 里每个 `setValue(EXPR)`：EXPR 不许有裸数字 | 只看 `setValue`；`setRange`/`setSingleStep`/`setDecimals` 是量程不是值 |
| 3f | 字符串型默认值得露在面板上 | `defaultParams()` 引用字符串型常量时，`buildUI()` 必须引用**同一个**常量 | 只要求常量**出现**，不保证它被用作了控件初值 |

三条 lint 查的是**所有分支**的 `run()`，不只当前构建那一支 ——
`dl_infer` 那 6 个旋钮在当前构建里读不到（整段在 `#ifdef` 里），
但那一支的兜底是**装上 SDK 之后立刻生效**的代码。

> 规则设计上有一次**否决**：笔记原来建议"同一个字面量在同一个文件里出现 >1 次就报"。
> 实测**误报太多** —— `setRange(0, 255)`、`setSingleStep(10)`、`lo[0]` 全会被扫出来，
> 而它们一个是量程、一个是步长、一个是**下标**。
> 最后收成"**兜底/初值的表达式里不许有裸字面量**"：
> 判据更窄，但零误报。**规则宁窄勿宽 —— 误报的守卫会被下一个人关掉。**

### 6.2 这三条当场抓出 6 处真问题

| 算子 | 面板上的初值 | 声明的默认值 | 病 |
|---|---|---|---|
| `pcb_inspect` 金层区间 | `setValue(85/80/115/0.8)` | `pcbcolor::goldBand()` | 数字抄本 |
| `dl_infer.class_names` | placeholder `"person,bicycle,car"` | COCO 六个类名 | 抄本，**只抄了前三个** |
| `image_source_camera.camera_ip` | placeholder `"如 192.168.1.10"` | `"192.168.1.10"` | 抄本 + 框是空的 |
| `image_source_dir.dir_path` | placeholder `"如 images 或 D:/test_imgs"` | `"images"` | 抄本 + 框是空的 |
| `image_source_video.video_path` | placeholder `"如 images/sample.avi"` | `"images/sample.avi"` | 抄本 + 框是空的 |
| `pcb_inspect.mode` | `addItem(..., "known_site")` | `PcbInspectDefaults::kMode` | 抄本 |

金层那处值得单说：它和 `pcbcolor::goldBand()` 的数值**完全一致**，
所以从任何一次运行的结果上**看不出毛病**。

但它就是抄本 —— 校准值一改，面板就悄悄对不上了，
而界面上不会有任何提示。

### 6.3 「框是空的」也是一份默认值

`image_source_dir` 那三处的病比"抄本"还重一点：

```
defaultParams() 声明   dir_path = "images"
        ↓
面板 buildUI()         m_dirPath 是空的          ← 初值没给
        ↓
collect()              p["dir_path"] = ""        ← 把空串写回参数
        ↓
run()                  params.value("dir_path").toString("images")
                       → key 在,值是空串 → 返回空串,不是 "images"
        ↓
                       报错"目录不存在: "
```

**声明的默认值被面板吃掉了。**

它不是静默失效（它会报错），但报的是"目录不存在: "，
人会往**路径写错了**的方向查 ——
真实原因是"这个 key 压根没有默认值可用"。

修法统一成：初值直接取常量
（`new QLineEdit(QString::fromUtf8(ImageSourceDirDefaults::kDirPath))`），
提示语只留"例子"那一半。

### 6.4 欠账 6：审计扫的源码，必须是产品真在编译的那份

旋钮审计是**源码级**的 —— 它读 `operators/*.cpp` 的文本。
这套办法有个前提，从来没人写下来：

> **它读到的文件，必须是产品真在编译的那些文件。**

前提不成立时，审计**不会报错**，它会安静地查错东西。
最坏的一种：有人加了新算子但忘了加进 `CMakeLists.txt` ——
**主程序根本没编它**，审计却在查它的旋钮，还报"这个旋钮是死的"。

新增**用例 7**做双向核对：

- 磁盘上 `operators/*.cpp` 每个都必须在主程序的 `set(SOURCES ...)` 里
- `set(SOURCES ...)` 里列的每个都必须在磁盘上真的存在

⚠ 查 `CMakeLists.txt` 要按 **CMake 的注释规则**切（`#` 到行尾），
不能拿 C++ 那套（`//`、`/* */`）去切 ——
注释里写着 `operators/xxx.cpp` 的时候，那是**说明**，不是真的在编译。

### 6.5 欠账 5 的可测部分：运行时缺失必须说得出为什么

面板置灰本身测不了（QtWidgets 要真窗口，无头造不出来）。
但**另一半**能测：那句"为什么不能用"的文案。

新增**用例 8**：

- 运行时不可用 → 原因文案非空、点名了缺的是什么、说了要设哪个环境变量
- 运行时可用 → 不该挂着"缺失原因"文案

理由跟置灰一样：**摆着能调但跑不了的旋钮 = 骗人**。
光置灰，用户只知道"不让调"，不知道为什么；
只写"不可用"，等于把问题原样推回给用户 ——
那是**没写**，不是**写清楚了**。

### 6.6 第二轮的变异：6 个，全杀

基线：**290 条断言**（上一轮 168 条）。

| # | 变异 | 谁抓到 | 红了几条 |
|---|---|---|---|
| M9 | `pcb_inspect` 面板把金层 H 下限抄回裸字面量 `85` | 用例 3e | 1 |
| M10 | `blob_count` 的兜底抄回裸字面量 `50` | 用例 3d + 用例 5 行为 | 3 |
| M11 | `circle_fit` 的兜底引用了**别的算子**的 `AngleDefaults::kX1` | 用例 3d + 用例 5 行为 | 3 |
| M12 | `image_source_dir` 面板不给 `dir_path` 初值 | 用例 3f | 1 |
| M13 | 把 `qr_decode.cpp` 从主程序的 `SOURCES` 里删掉 | 用例 7 | 2 |
| M14 | `dl_infer` 的 `runtimeUnavailableReason()` 返回空串 | 用例 8 | 3 |

**6/6 杀掉。**

M10/M11 值得单说：它们**同时**被源码 lint（3d）和行为测试（用例 5）抓到 ——
两条完全不同的手段指向同一个病。
这正是本项目要的样子：**行为测试兜住"值不一样"，源码 lint 兜住"值一样但来源错了"。**

M11 尤其重要：它把 `circle_fit` 的兜底换成了 `AngleDefaults::kX1` ——
**引用的是一个真实存在的常量**，从"看起来对不对"完全看不出毛病，
只有"这是不是本算子的默认值"能抓住它。

> 两轮合计：**14 个变异，全杀**。

---

## 七、仍然欠着的账

这一轮收掉了 1 和 6，5 收掉了一半。剩下的：

1. ~~**审计只查 `defaultParams()`**~~ → **已收**（见第六节 6.1~6.3）。
2. **用例 5 只对"输出里看得出差别"的算子有分辨力**。
   `template_match` 的 `score` 与阈值无关、`pass` 在完美匹配时恒为 true ——
   那条只能钉住"两次运行一致"，钉不住具体阈值（由用例 6 的真值表兜底）。
   *部分缓解*：3d 从**源码层**兜住了"兜底抄副本"这一类，
   不管输出看不看得见。但用例 5 本身的分辨力边界还在。
3. **用例 1/2 只认 `params.value("x")` 这种读法**。
   换成自定义的读取封装（不是 `params.value` 套一层）它就看不见。
   已知的安全写法：`readHsvScalar(params.value("x"), ...)` ——
   内层还是 `params.value`，所以现在能看见。
4. **两个 SDK 缺失分支的真实推理路径一行都没测过**。
   `dl_infer` 的 `#else` 分支、`image_source_camera` 的 `#else` 分支，
   本机没装 SDK，编译都不参与。
   豁免清单能保证"没装的时候别假装它能跑"，但保证不了"装了的时候它是对的"。
   *本轮新增了一条覆盖*：那条分支的兜底也必须来自常量（3d）——
   但那只保证"值是同一份"，不保证"推理算得对"。
5. **面板置灰没有自动化测试**（需要真窗口）。
   *本轮关掉一半*：用例 8 测了"为什么"那一句；
   置灰本身（`setEnabled(false)`）仍然只能靠人看。
6. ~~**审计扫的是源码，不是编译产物**~~ → **已收**（见第六节 6.4）。
   *但只到"文件清单"这一层*：真要看编译产物里有没有这段代码，
   得读 .obj / 符号表，那要额外的工具链。
7. **本轮新登记的一条**：`setParams()` 里的
   `params.value(kKeys[i]).toInt(0)` 这类"类型不对就退成 0"的写法
   （`angle` / `caliper_dist` / `line_fit` / `point_line_dist` /
   `point_point_dist` 五个测量算子都有）。
   它的病不是"抄副本"，是**静默把坏值变成好值**：
   某个 key 存在但类型不对时，界面会显示 0，而不是声明的默认值。
   本轮没查、也没改 —— 它的修法要另设计
   （是报错？还是退回声明的默认值？两者对"用户手改坏了存档"的后果完全不同）。
8. **兜底表达式外面套一层运算的情况查不到**：
   `image_source_video` 写的是
   `std::max(1, params.value("frame_step").toInt(...))`，
   那个 `1` 在 `toInt` 的参数外面，3d 看不见。
   那一层是**安全下限**（防 `frame_step=0` 把帧循环卡死），不是默认值的副本 ——
   但它确实让"声明值"和"实际兜底值"在运行时才相等，而不是在源码里就相等。
9. **CI（GitHub Actions）上没真跑过** —— 仓库无 remote，仍悬着。
10. **模式名 `auto_contour` 仍然是字面量**（`pcb_inspect.cpp` 里 3 处）。
    *这是故意的*，但要写下来免得下一个人"顺手"把它也变成常量：
    它是**存档格式的一部分**（用户的 `.sol.json` 里存的就是
    `"mode": "auto_contour"`），改它等于改老存档的语义。
    审计查的是**默认值**，`auto_contour` 不是默认值 —— 它是另一个可选值。
    真要收，得先把"模式名"也列成一份受控的清单，那是另一件事。

---

## 八、核实清单

- [x] 16 个算子的 `defaultParams()` 全部引用 `operator_defaults.h`
- [x] `defaultParams()` 里没有裸数字/裸字符串字面量
- [x] `run()` 里每个登记的 key 都有兜底，且兜底 == 声明值
- [x] 必填旋钮（两个算子的 HSV 区间）**不**补默认值，缺了报错
- [x] `pcb_inspect` 的死 key `min_area` 已删，真正的旋钮 `min_area_pct` 已登记
- [x] 豁免清单与当前构建状态一致，且会自己报过期
- [x] 8 个变异全杀（第一轮）
- [x] **6 个变异全杀（第二轮）** —— 两轮合计 14 个
- [x] **兜底不许抄第二份**（3d）：71 处兜底读法逐个核过
- [x] **面板初值不许抄第二份**（3e）：27 处 `setValue` 初值逐个核过
- [x] **字符串型默认值得露在面板上**（3f）：9 个字符串型默认值逐个核过
- [x] **审计扫的源码 = 主程序真在编译的源码**（用例 7）：22 ↔ 22 双向对上
- [x] **运行时缺失说得出为什么**（用例 8）
- [x] 面板上 6 份抄本已收成一份（金层区间 4 处 + 5 处面板初值）
- [x] W7 15 套件 / 1504 条断言全绿
- [x] W6 3 套件 / 477 条断言全绿
- [x] 跑完测试源码树零污染
- [ ] CI（GitHub Actions）上真跑一遍三步依赖准备 —— 仓库无 remote，仍悬着

---

参见：

- `notes/atomic/at-a-lint-that-finds-nothing-looks-green.md`
- `notes/atomic/at-a-port-is-not-the-original.md`
- `notes/atomic/at-the-copies-had-already-drifted.md`
- `notes/atomic/at-the-third-copy-was-on-the-panel.md`（第二轮：第三份抄本在面板上）
- `notes/atomic/at-a-placeholder-is-not-a-default.md`（第二轮：提示语不是默认值）
- `notes/atomic/at-the-audit-and-the-product-must-read-the-same-files.md`（第二轮：审计扫的和产品跑的要同一份）
- `notes/atomic/at-four-copies-of-one-number.md`（上一轮：同一个数抄四份）
- `notes/atomic/at-mutation-ruler-broken.md`（变异活下来先怀疑尺子）
