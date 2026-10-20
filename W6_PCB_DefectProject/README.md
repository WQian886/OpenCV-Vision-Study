# W6: PCB 缺陷检测综合项目

## 🎯 学习目标
1. 把 W2（HSV 颜色提取）+ W3（边缘/轮廓）+ W4（模板/特征）+ W5（OCR/QR）**串成完整管线**
2. 实现 7 类 PCB 缺陷判定：**缺件 / 错件 / 极性反 / 焊桥 / 焊少 / 偏移 / 短路**
3. 用 Python 合成 baseline + 7 张缺陷变体,作为标准测试集
4. 输出 **CSV 报告**（per-board-per-component）
5. **实战：跑一张图 → 得到一份带缺陷清单的 CSV**

## 📐 2026-10-20 的架构调整：判定逻辑搬到共享核心

原来这个项目里是**一张写死的 `SITES[6]` 真值表 + 七处 `if (label == "R2")` 的规则分派**。
实测代价（拿三种别的板型直接跑）：

- 元件数恒等于 6，与图上实际有什么无关
- 三块**好板**全被判废（误报 3/3）
- 两块废板的输出与好板**逐字相同**，等于没检

根因不是"坐标写死了"，而是**规则也写死了**。

而 W7 那边早就把判定改成了**配方驱动 + 逐条标定**的版本。也就是说：
**能力在 W7、数据在 W6，而 W6 自己用不上。**

现在两边都调 `common/pcb_core/`（无 Qt 依赖，W6 不引 Qt）：

```
W6 命令行 ─┐
            ├─→ common/pcb_core/  （配方解析 + 定位 + 六条判据 + 短路）
W7 算子  ──┘
```

**硬把 W7 的算法抄一份到 W6 是本项目栽过四遍的「抄副本」** —— 抄出来的那份
会随 W7 的标定一起漂，漂了没人知道。现在只有一份实现。

W6 因此能检**全部 17 张测试图**（8 张 demo + 9 张 complex），
结论和 W7 算子**逐字一致**。

## 📁 工程结构
```
W6_PCB_DefectProject/
├── CMakeLists.txt              # Release-only / OpenCV / utf-8 / 链共享核心 + 2 个测试套件
├── main.cpp                    # 4 个子模式入口（只管解析参数 + 打印）
├── w6_cli.h                    # ← W6 的可测逻辑层（纯逻辑，不打印）
├── w6_cli.cpp                  #   查板型 / 找配方 / 跑一块板 / 跑一批板 / 摊 CSV 行
├── tests/
│   └── test_w6_cli.cpp         # W6 CLI 层的回归测试（142 条断言）
├── gen_pcb_dataset.py          # 用 Pillow 合成 baseline + 7 张缺陷图
├── gen_complex_pcb.py          # 三种复杂板（power / mcu / signal）
├── README.md                   # 本文件
├── NOTES_W6.md                 # 术语速查卡
├── images/                     # 测试图 + 输出
│   ├── baseline.png            ← 标准板
│   ├── defect_missing.png
│   ├── defect_wrong.png
│   ├── defect_polarity.png
│   ├── defect_bridge.png
│   ├── defect_insufficient.png
│   ├── defect_shifted.png
│   ├── defect_short.png
│   ├── manifest.csv            ← 测试图元数据（也是"这块板属于哪种板型"的真值）
│   ├── complex/                ← 三种复杂板
│   │   ├── complex_manifest.csv
│   │   ├── power_*.png         (16 元件, 1200x900)
│   │   ├── mcu_*.png           (22 元件, 1200x900)
│   │   └── signal_*.png        (18 元件, 1200x900)
│   └── w6_inspect_marked.png   ← inspect 输出
├── out/                        # OCR 中间产物（**不放 images/**，否则会被当成待检板）
├── report/                     # CSV 报告输出
└── ../common/pcb_core/         # ← 共享判定核心（W6/W7 共用）
```

## 🛠️ 构建（VS 方式，同 W5）
1. VS 2022/2026 → **打开本地文件夹** → `E:\qianwie\00study\OpenCV\W6_PCB_DefectProject`
2. CMake 自动配置 → **配置切到 x64-Release**（必须 Release！Debug 会触发 OpenCV ABI 不匹配）
3. F7 编译 → Ctrl+F5 运行

## 🛠️ 命令行构建（备选）
```powershell
cd E:\qianwie\00study\OpenCV\W6_PCB_DefectProject
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 1. 生成测试数据（首次必跑）
python gen_pcb_dataset.py

# 2. 默认模式 = baseline 全管线
build\w6_pcb_defect.exe

# 3. inspect 单图（--no-gui 不弹窗，便于自动化）
build\w6_pcb_defect.exe inspect images/baseline.png --no-gui

# 4. 批量报告（递归扫 images/，含 images/complex/）
build\w6_pcb_defect.exe report images

# 5. 重新生成数据
build\w6_pcb_defect.exe gen
```

### 可选参数

| 参数 | 说明 |
|---|---|
| `--recipes <dir>` | 板型配方目录。默认按 `recipes/` → `../W7_VisionMasterLite/recipes/` 顺序找 |
| `--no-gui` | `inspect` 模式不弹窗（无显示器 / 自动化用） |

### 板型配方从哪来

配方 JSON 由 `W7_VisionMasterLite/tools/export_recipes.py` 从**本项目的生成器**导出
（`gen_pcb_dataset.py` 的 SITES + `gen_complex_pcb.py` 的 layout）——
坐标和规则都来自生成器，**不是手抄的**。

改配方要去改生成器再重跑导出脚本，不要手改 JSON。

W6 不复制一份配方（复制就是抄副本），但构建时会把 `W7_VisionMasterLite/recipes/`
拷到 exe 旁边，所以 exe 单独拿走也能跑。

## 🧪 跑测试

W6 有 **2 个 ctest 套件、305 条断言**（2026-10-20 之前是 **0 个**）。

```powershell
cd E:\qianwie\00study\OpenCV\W6_PCB_DefectProject
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# ⚠ 必须带 -C Release,否则套件全报 Not Run
ctest --test-dir build -C Release --output-on-failure
```

| 套件 | 断言 | 测什么 |
|---|---|---|
| `pcb_core` | 163 | 共享判定核心：JSON 解析、配方校验、拒绝判定、端到端对照真值表 |
| `w6_cli` | 142 | W6 自己的 CLI 行为：查板型、递归扫描、跳过必须报原因、CSV 转义、per-component 行 |

`pcb_core` 的**源码在 `common/pcb_core/tests/`**，W6 和 W7 链的是同一份 ——
两边都跑才能说明"判定是对的，而且 W6 真的用上了它"。

### 断言从哪来

不抄一份。板子清单读 `images/manifest.csv` 和 `images/complex/complex_manifest.csv`
（本来就是真值表）—— 加一张新图、改一条期望，测试自动跟着变。

### 有两条断言记的是"已知漏检"

`signal_polarity.png` 和 `power_short.png` 目前判不出缺陷（根因见上面的
「已知限制」表）。断言记的是**当前真实行为**（PASS）。
哪天判据重做完了，断言会立刻转红，提醒补上"必须命中"的断言。

### 加了新代码记得做变异验证

改了判定/CLI 逻辑之后，把代码**改坏一次**，看测试会不会红：

| 变异 | 红了几条 |
|---|---|
| `boardKindOf` 退回 `.string()`（还原路径编码 bug） | 3 |
| `runReport` 退回非递归遍历（还原漏检 9 块板） | 5 |
| 多个缺陷只写第一个 | 3 |
| `csvEscape` 不转义 | 5 |

**全绿说明测试没牙。** 注意构建失败会被误读成"变异存活"——
脚本里构建失败必须抛异常（见 `notes/atomic/at-build-failure-looks-like-mutation-survived.md`）。

## 📚 知识点（贯穿前 5 周）

### PCB 缺陷判定管线（2026-10-20 之后）

```
images/<board>.png (BGR)
   ↓ 板型:从同目录的 manifest.csv 查 kind → recipes/<kind>.json
   ↓        （**不是按文件名猜**,也不是硬编码 —— 坐标和规则都在配方里）
   ↓
   ↓ ═══ common/pcb_core/ ═══════════════════════════════
   ↓ ① 定位(inspect)
   ↓    板面基色逐图估计(相对判据的右端必须带 k 才叫相对)
   ↓    cv::cvtColor → HSV (W2)
   ↓    cv::inRange(色带) → goldMask (W2)
   ↓    morphologyEx OPEN (W3),核跟着图缩放
   ↓    按配方构造每个站点的 ROI(按 ref_w/ref_h 等比缩放)
   ↓    量 body_pct(框里非板面像素占比)
   ↓      └─ 这道闸:框里全是板面基色 = 配方配错图 → 拒绝判定
   ↓
   ↓ ② 判定(judge) —— 逐元件跑**它自己配了的那几条规则**
   ↓    缺件:  暗像素占 ROI %(黑本体) / 非板面占同板中位数的比值(彩色本体)
   ↓    错件:  错色像素占 ROI %
   ↓    极性反: 极性条质心 x 占 ROI 宽 %(单边判据)
   ↓    焊桥:  ROI 内最长一行金像素占 ROI 宽 %
   ↓    焊少:  最小焊盘比同板中位数低 %
   ↓    偏移:  本体质心偏离 ROI 中心 %
   ↓    短路:  蓝紫圆环面积/宽/高 占全图 % 与对角线 %(全局缺陷)
   ↓      └─ 规则被守卫挡掉时**必须报出来**(skipped),不能默默不跑
   ↓ ═══════════════════════════════════════════════════
   ↓
   ↓ ┌─── W6d: OCR 丝印(放大 3 倍 + tesseract --psm 11)+ QR 串号(W5)
   ↓ └─── W6e: 写 CSV 报告(per-board-per-component)
```

### 7 类缺陷速查

判定依据现在全是**"占某个基准的百分比"**，不再有裸像素数。
基准逐条不同：面积类的量（暗像素 / 浅黄 / 焊盘）→ 该元件的 ROI 面积；
长度类的量（最长金段 / 质心偏移）→ 该元件的 ROI 宽或高；
全局缺陷（短路圆环）→ 全图面积 / 全图对角线。

| 缺陷 | 英文 | 判定依据 | 难度 |
|---|---|---|---|
| 缺件 | missing | 暗像素占 ROI %（黑本体）/ 非板面像素占同板中位数的比值（彩色本体） | ⭐ |
| 错件 | wrong | 错色像素占 ROI % > 阈值 | ⭐⭐ |
| 极性反 | polarity | 极性条质心 x 位置占 ROI 宽 %（**单边判据**，见下方已知限制） | ⭐⭐ |
| 焊桥 | bridge | ROI 内最长一行金像素数占 ROI 宽 % | ⭐⭐ |
| 焊少 | insufficient | 最小焊盘比中位数低 % | ⭐ |
| 偏移 | shifted | 本体质心偏离 ROI 中心的 % | ⭐ |
| **短路** | **short** | **蓝紫圆环：面积/宽/高占全图 % 与对角线 %** | ⭐⭐⭐ |

### 关键 API（不引入新依赖）

| API | 复用 | 用途 |
|---|---|---|
| `cv::inRange` | W2 | 提取金色 mask |
| `cv::findContours` | W3 | 数焊点 / 元件轮廓 |
| `cv::connectedComponentsWithStats` | W3 | **短路判定** + 焊盘中位数 |
| `cv::moments` | W3 | 本体质心（偏移 / 极性条位置） |
| `cv::QRCodeDetector` | W5 | 读序列号 |
| `CreateProcessW + tesseract` | W5 | 读丝印文字（`--psm 11` 散落文本模式） |
| `cv::adaptiveThreshold` | W2 | 丝印 OCR 预处理（**先放大 3 倍**） |

## ⚠️ 已知限制

| 限制 | 说明 |
|---|---|
| **极性反只抓得到一半** | 判据是**单边**的（只在质心偏右时报）。demo 板反装后极性条挪到右边 → 抓得到；signal 板反装后挪到左边 → 单边阈值根本不看那边 → **漏检**。改成双边的容差区间实测**无解**（好板 41.84% vs 废板 29.87%，要求容差同时 < 8.16 且 > 20.13）。要真解决得给配方加 `polarity_side` 字段。 |
| **power 板的短路抓不到** | 判据找的是**蓝紫圆环**，而 power 那张短路图画的是**一条金条** —— 面积 0.1992% 撞出上界 0.08%，宽 41.4% 对角线也撞出上界 8%。不是阈值没调好，是**缺陷定义**和判据不匹配。 |
| **丝印 OCR 在合成图上读不准** | 板上的字是 14~18px 的白字，已经放大 3 倍 + 自适应阈值，仍只能读出大概（"PCB-2026-00)"）。**QR 串号是准的**（`cv::QRCodeDetector`）。实拍图上效果会好很多。 |
| **complex 三块板的 insufficient 没跑过** | 金层掩膜的开运算核跟着图缩放，而生成器画焊盘的像素尺寸是写死的 —— 核变大、焊盘不变，小焊盘被开运算吃光，凑不出 4 个连通域取不了中位数。这条规则**在报告里会明说"没查"**，不是"查过没问题"。 |

## ⚠️ 常见踩坑

| 现象 | 原因 | 修复 |
|---|---|---|
| `Debug` 编译失败 | OpenCV ABI 不匹配 | 切到 x64-Release |
| `imread failed: images/baseline.png` | 没跑 `gen_pcb_dataset.py` | 先 `python gen_pcb_dataset.py` |
| 找不到板型配方 | 没跑导出脚本 | `python W7_VisionMasterLite/tools/export_recipes.py` |
| 某张图被"跳过" | 同目录没有登记它的 manifest.csv | 补进 manifest.csv（**别在代码里硬写板型**） |
| `std::max` 报 C2589 | `windows.h` 把 max 定义成宏 | `#define NOMINMAX` 或写 `(std::max)(...)` |
| OCR 输出乱码 | 控制台非 UTF-8 | CMake 加 `/utf-8` + `SetConsoleOutputCP` |
| CSV 列错位 | 字段里有逗号没转义 | `writeCSVReport` 里的 `csvEscape()` 已处理 |

## ✏️ 课后练习

### 基础
1. **改 PCB 配色**：把 `GREEN_PCB` 改成蓝色（#1a3d5d）看 inspect 输出
2. **加更多元件**：在 `gen_pcb_dataset.py` 的 SITES 里加第 7 个元件，
   重跑 `export_recipes.py`，看 baseline 是否仍能跑通
3. **新缺陷类型**：加 "元件倾斜 45°" 作为第 8 类缺陷

### 进阶
4. **给 signal 板补上极性反**：给配方加 `polarity_side` 字段，
   让极性判据变成双边（详见 `common/pcb_core/pcb_core.cpp` 里的注释）
5. **重做短路判据**：power 板那张是金条不是环 ——
   改成"不该存在的跨区导体"（见核心里 short 那段的分析）
6. **给 complex 三块板补 insufficient 真值图**：生成器里画几块焊少的板，
   **9 张图全部要重量一遍**（这条在那段注释里挂了很久了）
7. **给 W6 的 CLI 再加一条断言**：W6 现在有 2 个套件 305 条断言了
   （`tests/test_w6_cli.cpp`）。挑一条现在没覆盖的行为补上 ——
   比如"`--recipes` 指向一个只有部分配方的目录"该报什么
8. **让路径编码这条路也能测**：现在 `w6_cli` 里已经有 `pathText()` / `loadImage()`
   两个原语，但它们自己**没有单独的断言**（只被间接用到）。
   给它们补上直接用例

### 实战
9. **替换实拍图**：用手机拍一块 PCB，放到 images/ 下，跑 inspect
10. **加新缺陷**：自己造一张"元件错位 + 焊桥"的复合缺陷图，看检测结果

## 🎓 下一节预告

W7+（如果你想做）：
- 多个 PCB 同时检测（批处理）
- 与 PLC / MES 系统对接（写 XML/JSON 上传）
- 深度学习方案（YOLO 检测元件）
- 3D 检测（结构光 + 点云）
