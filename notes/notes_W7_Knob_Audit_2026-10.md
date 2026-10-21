# W7 旋钮审计：把「界面上摆着的旋钮」和「背后那段代码」对上

> 2026-10-21
>
> 这一轮做两件事：
> **A2** 把算子旋钮的默认值收成唯一一份；
> **A3** 加一条「旋钮审计」——每个旋钮都得对得上，
> 对不上就报出来。

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

新建 `W7_VisionMasterLite/tests/test_operator_knobs.cpp`（168 条断言），
链进全部算子，接进 ctest（套件名 `operator_knobs`）。

六条用例，各自的能力边界都写在用例的注释里：

| 用例 | 查什么 | 手段 | 边界 |
|---|---|---|---|
| 0 | 审计本身没有空转 | 断言扫到的算子数 ≥ 15、旋钮数 ≥ 70 | —— |
| 1 | 死旋钮 | 源码 lint：登记了但当前构建的 `run()` 没读 | 只认 `params.value("x")` 这种读法 |
| 2 | 没登记的旋钮 | 源码 lint：`run()` 读了但 `defaultParams()` 没登记 | 同上 |
| 3 | 默认值只有一份 | 源码 lint：`defaultParams()` 里不许有裸字面量；引用的 `Defaults` 常量必须真的在 `operator_defaults.h` 里；全仓库只有那一个文件能定义 `Defaults` 结构体 | 只查 `defaultParams()` |
| 4 | 豁免清单不能过期 | 问算子自己的 `runtimeAvailable()` | —— |
| 5 | 空参数 ≡ 声明的默认参数 | **行为测试**：两种跑法结果必须一样 | 只有"输出里看得出差别"的算子才被这条抓住 |
| 6 | 关键默认值符合真值表 | 期望值写在测试里（基准不来自被测对象） | —— |

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

## 六、仍然欠着的账

1. **审计只查 `defaultParams()`**。
   兜底（`run()` 里的 `toDouble(...)`）和面板初值（`buildUI()` 里的
   `setValue(...)`）要是哪天又抄了一份，审计看不见。
   *要补的话*：用例 3 可以扩成"同一个字面量在同一个算子文件里
   出现 >1 次就报" —— 但 0/1/2 这种数字太常见，需要先区分"阈值"和"普通数字"。

2. **用例 5 只对"输出里看得出差别"的算子有分辨力**。
   `template_match` 的 `score` 与阈值无关、`pass` 在完美匹配时恒为 true ——
   那条只能钉住"两次运行一致"，钉不住具体阈值（由用例 6 的真值表兜底）。

3. **用例 1 只认 `params.value("x")` 这种读法**。
   换成 `readHsvScalar(params.value("x"), ...)` 这种包一层的写法它就看不见
   （实际上 `readHsvScalar` 的参数是 `params.value(...)` 的结果，
   所以现在能看见；但换成 `params["x"]` 之外的自定义读取就悬了）。

4. **两个 SDK 缺失分支的真实推理路径一行都没测过**。
   `dl_infer` 的 `#else` 分支、`image_source_camera` 的 `#else` 分支，
   本机没装 SDK，编译都不参与。
   豁免清单能保证"没装的时候别假装它能跑"，但保证不了"装了的时候它是对的"。

5. **面板置灰没有自动化测试**（需要真窗口）。

6. **审计扫的是源码，不是编译产物**。
   如果以后有人用脚本生成 .cpp，或者把算子挪到别的目录，
   审计会扫不到 —— 用例 0 会报红（算子数掉下去），但不会告诉你为什么。

---

## 七、核实清单

- [x] 16 个算子的 `defaultParams()` 全部引用 `operator_defaults.h`
- [x] `defaultParams()` 里没有裸数字/裸字符串字面量
- [x] `run()` 里每个登记的 key 都有兜底，且兜底 == 声明值
- [x] 必填旋钮（两个算子的 HSV 区间）**不**补默认值，缺了报错
- [x] `pcb_inspect` 的死 key `min_area` 已删，真正的旋钮 `min_area_pct` 已登记
- [x] 豁免清单与当前构建状态一致，且会自己报过期
- [x] 8 个变异全杀
- [x] W7 15 套件 / 1382 条断言全绿
- [x] W6 3 套件 / 477 条断言全绿
- [x] 跑完测试源码树零污染
- [ ] CI（GitHub Actions）上真跑一遍三步依赖准备 —— 仓库无 remote，仍悬着

---

参见：

- `notes/atomic/at-a-lint-that-finds-nothing-looks-green.md`
- `notes/atomic/at-a-port-is-not-the-original.md`
- `notes/atomic/at-the-copies-had-already-drifted.md`
- `notes/atomic/at-four-copies-of-one-number.md`（上一轮：同一个数抄四份）
- `notes/atomic/at-mutation-ruler-broken.md`（变异活下来先怀疑尺子）
