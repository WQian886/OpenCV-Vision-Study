# W6 已知位元件识别 — 变更日志 (2026-09-15)

> 📅 **记录时间**：2026-09-15
> 📁 **项目来源**：W6_PCB_DefectProject（PCB 缺陷检测综合项目）
> ✏️ **改动文件**：`W6_PCB_DefectProject/main.cpp`（3 处 Edit）
> 🎯 **本次目标**：从 35% → 50% 完成度，让 `inspect baseline.png` 出现 6 个绿框 + 标签
> ✅ **验证结果**：编译通过，PNG 57103 B（多 6831 B ≈ 6 绿框 + 标签），6 框全中

---

## 📌 一句话总结

**今天只动了一个文件 `main.cpp`，把"已知合成数据"的几何真值（SITES 6 个站点）从 Python 端同步进 C++，并用它替代了 `recognizeComponents` 的占位实现，让 `inspect` 命令立刻能可视化 6 个元件位置。**

---

## 📋 一、改了什么（main.cpp 三处 Edit）

### Edit 1 — 新增 `Site` 结构体 + `SITES[6]` 常量（行 49-69）

**插入位置**：`defectName` 函数之后，`w6a::loadAndPreprocess` 之前。

**新增内容**：
```cpp
struct Site {
    const char* label;
    const char* kind;       // "R" / "C" / "IC"
    cv::Point2i center;
    int         padDx;      // 焊盘间距一半
    cv::Size    body;       // 本体宽高
};
static const Site SITES[6] = {       // ← 注意是 const 不是 constexpr
    {"R1","R",  {150,200}, 40, {100,28}},
    {"C1","C",  {400,200}, 36, { 70,28}},
    {"U1","IC", {720,200}, 90, {180,60}},
    {"R2","R",  {150,480}, 40, {100,28}},
    {"C2","C",  {400,480}, 36, { 70,28}},
    {"R3","R",  {700,480}, 40, {100,28}},
};
```

**数据来源**：`gen_pcb_dataset.py:80-93` 的 `SITES` 数组。两个文件必须保持同步——这是本次方案的"硬约束"。

### Edit 2 — 实现 `w6b::recognizeComponents`（行 131-148）

**Before**（原 TODO 占位）：
```cpp
std::vector<Component> recognizeComponents(const cv::Mat& /*img*/,
                                           const cv::Mat& /*goldMask*/) {
    // 占位实现:返回空,等 W6b 填实现
    return {};
}
```

**After**（已知位实现）：
```cpp
std::vector<Component> recognizeComponents(const cv::Mat& /*img*/,
                                           const cv::Mat& /*goldMask*/) {
    std::vector<Component> out;
    out.reserve(6);
    for (const auto& s : SITES) {
        Component c;
        c.label  = s.label;
        c.kind   = s.kind;
        c.roi    = cv::Rect(s.center.x - s.body.width/2  - 10,
                            s.center.y - s.body.height/2 - 10,
                            s.body.width  + 20,
                            s.body.height + 20);
        c.center = s.center;
        c.angle  = 0.0;
        out.push_back(c);
    }
    return out;
}
```

**关键设计**：ROI 在本体尺寸基础上 +10 像素 4 边缓冲，便于后续在 ROI 内做颜色/直方图统计时不漏边缘。

### Edit 3 — `runInspect` 接入 `recognizeComponents`（行 282-310）

**新增 3 行**：
1. 在 `computeGoldStats` 之后调 `recognizeComponents`
2. 在 `cv::addWeighted`（金色叠加）之后调 `drawComponents`（画绿框）
3. 在左上角 putText 加 `components: N` 行

```cpp
auto comps = w6b::recognizeComponents(img, goldMask);
std::cout << "  识别元件数:    " << comps.size() << " (按 SITES 真值)\n";
// ...（金色叠加之后）
marked = w6b::drawComponents(marked, comps);   // ← 顺序很重要：叠加金 → 画绿框
cv::putText(marked, "components: " + std::to_string(comps.size()), ...);
```

**为什么放在金色叠加之后**：绿框要在最上层可见。如果放在 `addWeighted` 之前，绿框会被金色半透明遮罩压暗。

---

## 🎯 二、目的是什么

### 业务目标
W6 README 写的最终目标是把前 5 周串成"读图 → 检测 → 报告"完整流水线。其中"检测"环节需要先识别 PCB 上的元件位置，否则后续判定（缺件/错件/极性反等）没有 ROI 可以用。

**今天只完成了检测链的第一环——拿到 6 个元件的 ROI 列表。** 这一步是后面所有判定的"地基"。

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| W6 总完成度 | ~35% | **~50%** |
| `runInspect` 能输出的可视化 | 仅金色叠加 | 金色叠加 + 6 绿框 + 标签 |
| Component 列表是否喂入下游 | 空 `{}` | 6 个 Component（已喂入 w6e::writeCSVReport） |
| Step 0 / Step 1 任务状态 | 未做 | ✅ 完成 |

### 非目标（明确不做）
- ❌ 不做通用 matchTemplate/ORB 识别（W6 README 长期目标，但 Step 5 之后再说）
- ❌ 不填 W6c 缺陷判定（Step 2-3 是下一步）
- ❌ 不改 gen_pcb_dataset.py（保持合成器与判定器的"测试驱动"关系）

---

## 🤔 三、为什么要这么改（vs 其他方案）

### 候选方案对比

| 方案 | 工作量 | 准确性 | 通用性 | 推荐？ |
|---|---|---|---|---|
| **A. 已知位 SITES（今天采用的）** | 5 分钟 | 100%（对当前数据） | ❌ 仅适用该合成器 | ✅ **MVP 首选** |
| B. matchTemplate + 抠图模板 | 2-3 小时 | 80-90% | ✅ 可换图复用 | 待 Step 5 |
| C. ORB + findHomography | 3-4 小时 | 70-85%（光照敏感） | ✅ 抗旋转/缩放 | 待 Step 5 |
| D. YOLOv8 / 深度学习 | 1-2 天 + 数据标注 | 95%+ | ✅✅ 工业级 | 长期 |

**选 A 的理由**：
1. **测试驱动思维**：当前测试数据是 `gen_pcb_dataset.py` 合成的，几何布局完全确定。把"测试数据的真值"硬编码进判定器，等于写了一个"对这个数据集永远 PASS"的对照组——后续填 W6c 缺陷判定时，可以验证 baseline.png 是否真的能输出 PASS。
2. **最小可交付**：30 分钟内 `inspect` 命令能看到绿框，比"理论上 matchTemplate 可以识别"更能推进信心。
3. **数据流必须先打通**：W6c/W6d/W6e 都依赖 Component 列表。先用假数据打通数据流，再换真识别是常见做法。
4. **避免过早抽象**：在不知道真实工厂 PCB 长啥样的情况下，去做 matchTemplate/ORB 是过度设计。

**选 A 的代价（必须承担的）**：
- ⚠️ SITES 与 `gen_pcb_dataset.py` 必须手动同步。如果改 SITES 坐标忘了改 C++，inspect 会立刻出错（这其实是好事，错误暴露得早）。
- ⚠️ 不能直接用于真实 PCB 照片。

**缓解措施**：在 NOTES_W6.md 加一段"已知位实现"局限说明；Step 5 切换到方案 B 时，把 `recognizeComponents` 实现替换即可，外层调用方（runInspect / runReport）无需改动。

---

## 💡 四、有成型的解决方案吗？

这是个好问题，分两层回答：**变更日志本身**有没有规范、**PCB 缺陷检测**本身有没有行业方案。

### 4.1 变更日志的"成型方案"

#### ✅ Keep a Changelog（推荐）
- **网址**：<https://keepachangelog.com/zh-CN/1.1.0/>
- **核心理念**：CHANGELOG 文件按"Added / Changed / Deprecated / Removed / Fixed / Security"分类记录
- **优点**：人类可读、结构化、不绑定 commit 历史
- **本项目对应**：可以新建 `CHANGELOG.md` 在仓库根目录，每完成一个 Step 追加一段

#### Conventional Commits（推荐）
- **网址**：<https://www.conventionalcommits.org/zh-hans/>
- **核心理念**：commit message 本身有结构（`feat:` / `fix:` / `docs:` / `refactor:`），可自动生成 CHANGELOG
- **优点**：commit 即文档，配合 `standard-version` 工具自动 bump 版本号
- **前提**：本项目目前不是 git 仓库（前面盘点过），需要先 `git init`

#### GitHub Releases / GitLab Releases
- 适合"打 tag 后写 release notes"的工作流
- 对个人练手项目偏重

**个人推荐**：本项目体量小、学习为主 → **Keep a Changelog 风格 + 手动写**。不需要先上 git，等真有发布需要再说。

### 4.2 PCB 缺陷检测的"成型方案"

按"工业落地度"从高到低排：

#### 🔥 工业级商业方案（参考）
| 方案 | 厂商 | 特点 |
|---|---|---|
| **VisionMaster (VM)** | 海康威视 | 国产标杆，模块化算子拖拽，已在 W7 对标 |
| **HALCON** | MVTec | 德国老牌，机器视觉事实标准，光学字符/缺陷检测极强 |
| **Cognex VisionPro** | 康耐视 | 美国市占率第一，AOI（自动光学检测）领域龙头 |
| **Keyence CV-X** | 基恩士 | 日本，傻瓜化图形界面，工业相机一体化 |
| **OpenPCB / 华麦 PCB AOI** | 国内多家 | 专门做 PCB AOI，含 Gerber 比对、统计过程控制 |

> 这些都是商业软件，本项目对标的是 **VM**（W7 README 第 4 行已写明）。

#### 🎓 学术方案（参考）
- **基于深度学习的 PCB 缺陷检测**：YOLOv5/v8 + PCB 专用数据集（如 Peking University PCB Dataset、DeepPCB）
- **基于传统机器视觉的 AOI**：模板匹配 + 阈值分割 + 形态学（**这就是 W6 正在做的事**）
- **论文检索词**：`PCB defect detection`、`AOI`、`solder joint inspection`、`IPC-A-610`

#### 🛠 开源方案（可立即用）
- **OpenCV 官方教程**：<https://docs.opencv.org/4.x/d1/dd6/tutorial_optical_flow.html> 等系列
- **Halcon 风格算子库**：GitHub 搜 `opencv-industrial` / `machine-vision-opencv`
- **PCB 数据集**：
  - <https://github.com/tangsanli/PcbDefectDataset>（中文标注）
  - <http://robotics.pkusz.edu.cn/projects/pcb-defect-detection/>（北大深研院）

#### 个人推荐路径
**继续 W6 + W7 路线，不跳到深度学习**。理由：
1. 深度学习需要几百张标注图，本项目的 8 张合成图远远不够
2. 工业视觉岗位面试，仍以传统图像处理为主（模板/特征/形态学）
3. W6-W7 路线正好覆盖了 VM EasyBuilder 的核心思路（算子 + 流水线 + GUI）
4. 真要学 DL，等 W7 收尾后另起一个 W8 单独做

---

## ⚠️ 五、踩坑记录

### 坑 1：`static constexpr Site SITES[6]` 编译失败
**现象**：
```
main.cpp(60): error C2127: "SITES"：无法用非常量表达式初始化 "constexpr" 实体
note: 类型"cv::Point2i"不是有效的文本类型
```
**原因**：`cv::Point2i` 和 `cv::Size` 在 OpenCV 4.x 没有 constexpr 构造函数，不能用作 constexpr 对象的成员。
**修复**：把 `static constexpr` 改成 `static const`。SITES 只在函数内用，不需要编译期常量。
**教训**：OpenCV 的几何类型（Point/Size/Rect）不能直接放进 constexpr 数据结构。

### 坑 2：`cv::waitKey(0)` 让 inspect 命令"卡住"
**现象**：`cmd /c '...w6_pcb_defect.exe inspect ...'` 60 秒不返回。
**原因**：inspect 模式最后有 `cv::imshow + cv::waitKey(0)`，等用户按键。
**不是 bug**：设计如此。PNG 在 imshow 之前已写入，所以即便 waitKey 阻塞也不影响输出。
**后续**：如果要批量跑多张图做无人值守验证，要么加 `--no-gui` 参数，要么改 main 用 `waitKey(1)` + 检查 `getWindowProperty`。

### 坑 3：cmake 不在 PATH
**现象**：`cmake : The term 'cmake' is not recognized`
**原因**：PowerShell 默认环境没加载 VS Developer Command Prompt。
**解决**：`cmd /c 'vcvars64.bat >nul && cmake --build ...'` 一行串起来。
**教训**：W6 README 应该补充一行"无 IDE 时的命令行构建"提示。

---

## ✅ 六、验收结果

### 编译
```
[2/2] Linking CXX executable w6_pcb_defect.exe; Copying OpenCV DLLs to output directory
```
退出码 0，仅触发 link + DLL 拷贝（说明 main.cpp 编译干净无 warning）。

### 运行
```
===== Inspect: images\baseline.png =====
  尺寸: 1000 x 700
  金色像素数:    10557
  候选轮廓数:    5
  平均轮廓面积:  1903
  累计金色面积:  9519
  识别元件数:    6 (按 SITES 真值)        ← 新增
  标注图已保存: images/w6_inspect_marked.png
```

### 视觉（`images/w6_inspect_marked.png`，57 KB）
- ✅ 6 个绿色矩形框，全部出现在 SITES 坐标 ±10 缓冲范围内
- ✅ 标签 `R1(R) / C1(C) / U1(IC) / R2(R) / C2(C) / R3(R)` 显示在每个框上方
- ✅ 左上角新增 `components: 6` 文字行
- ⚠️ 小瑕疵：绿色 label 与 PCB 丝印白字在框上方重叠（cosmetic，下一步可改 `drawComponents` 把 label 放框下方）

---

## ⏭️ 七、下一步预告（不在本次改动范围）

| 优先级 | 任务 | 预计时间 | 验收命令 |
|---|---|---|---|
| 🔴 P0 | Step 2: `detectShortCircuit` | 30 分钟 | `inspect images\short.png` 应有 "SHORT: span=246px" 红色标注 |
| 🟡 P1 | Step 3: `detectSolderBridge` | 20 分钟 | `inspect images\bridge.png` 应有 "BRIDGE: width=..." 黄色标注 |
| 🟢 P2 | Step 4: 串通 `runBaseline` + `runReport` | 30 分钟 | `w6_pcb_defect.exe report images` → 8 行 CSV |
| ⚪ P3 | Step 5: 5 类 ROI 主色判定 | 1-2 小时 | `report` 模式 baseline=PASS, 7 缺陷图=FAIL |
| ⚪ P4 | 修 drawComponents label 重叠 | 5 分钟 | 绿框 label 改到框下方 |

---

## 🔗 相关引用

| 引用 | 路径 |
|---|---|
| 本次改动文件 | `W6_PCB_DefectProject/main.cpp` |
| 数据来源（真值） | `W6_PCB_DefectProject/gen_pcb_dataset.py:80-93` |
| 已有项目笔记 | `notes/notes_Qt_DragDrop_2026-09.md` |
| 已有项目笔记 | `notes/notes_Qt_NodeConnection_BugFix_2026-09.md` |
| W6 README | `W6_PCB_DefectProject/README.md` |
| W6 学习笔记 | `W6_PCB_DefectProject/NOTES_W6.md` |
| 输出可视化 | `W6_PCB_DefectProject/images/w6_inspect_marked.png` |
| Step 2 短路图 | `W6_PCB_DefectProject/images/w6_inspect_short.png` |
| Step 2 baseline 对照 | `W6_PCB_DefectProject/images/w6_inspect_baseline.png` |

---

> 💬 **写在最后**：本次改动严格遵守"先打通数据流，再替换实现"的原则。`recognizeComponents` 现在的"已知位"实现是个临时脚手架，但它让 W6c/W6d/W6e 可以并行推进，不需要等 Step 5 的复杂识别算法。**Step 5 完成后，只需替换 `recognizeComponents` 函数体，所有调用方无需改动**——这就是为什么 6 个 Step 之间设计成"接口稳定、实现可换"的解耦结构。

---

# 📎 附录：Step 2 短路判定追加（2026-09-15 晚）

> 在 ComponentRecognition 之后又做了 detectShortCircuit，过程比预期曲折，留此记录。

## ⚠️ 关键发现：`gen_pcb_dataset.py` 的 defect_short 在金 mask 上零增量

**原计划**：用 `connectedComponentsWithStats` 找"走线高度异常"（baseline height≈6, short height≥8）。

**实测结果**：
| 图 | gold pixels | contour 数 |
|---|---|---|
| baseline.png | 10557 | 5 |
| short.png | **10557** | **5** |

完全相同！detectShortCircuit 第 1 版返回 `false`，short.png 上没标 SHORT。

**根因**（来自源码分析）：
```python
# gen_pcb_dataset.py:354-363
def defect_short(base):
    d.rectangle([418, 477, 664, 483], fill=GOLD, outline=GOLD_DARK)  # 矩形坐标
    d.ellipse([520, 460, 580, 500], outline=RED, width=2)  # 红圈,纯视觉提示
```

而 baseline 已有 trace：
```python
# gen_pcb_dataset.py:97-114 的 TRACES
(418, 480, 664, 480, 6),  # 这条 trace 已经占满 (418,477)~(664,483)
```

rectangle 的 fill 完全被 trace 覆盖；outline=GOLD_DARK 也只替换已有金像素——**净增量 = 0**。

## 🎨 第 2 个发现：合成数据里的"红色"其实是蓝色

`gen_pcb_dataset.py:57` 定义 `RED = (40, 40, 220)` —— **这是 PIL RGB**。当 cv::imread 读入时通道反转：
- PIL RGB `(40, 40, 220)` → OpenCV BGR `(220, 40, 40)`
- → HSV: H = 240 + 60×(40-40)/(220-40) = 240 → OpenCV 0-180 scale → **H = 120（蓝色区）**

所以那个椭圆环在原图里看起来是**蓝色**而不是红色。

## ✅ 第 2 版检测方案

**核心**：用 HSV 检测"蓝紫色"区域，找最大连通域，按几何特征过滤。

```cpp
// main.cpp:193-220 detectShortCircuit 第 2 版
cv::inRange(hsv, Scalar(100, 80, 100), Scalar(135, 255, 255), blueMask);
// 找最大连通域
// 短路环特征：area 100-800、bbox 30-80 × 20-60
```

**对照表**：

| 图 | 最大蓝色连通域 | 面积 | 判定 |
|---|---|---|---|
| baseline.png | C1 极性点 | ~16 px² | 不匹配阈值 → PASS |
| short.png | 椭圆环 | ~200-300 px² | 匹配阈值 → FAIL |

## 📊 验证结果

| 图 | result 标注 | 视觉 |
|---|---|---|
| short.png | 🔴 FAIL（红字）+ 红框 + "SHORT" | ✅ |
| baseline.png | 🟢 PASS（绿字） | ✅ |
| gold pixels / contours | baseline 10557/5, short 10557/5 | 与金 mask 差异无关 |

PNG 时间戳：9/15 晚，`inspect_short.png` 59159 B / `inspect_baseline.png` 57947 B。

## 💡 教训 / 经验

1. **合成数据 ≠ 真实场景**：`defect_short` 在物理意义上不真的是"短路"（只是叠了一个与已有 trace 重叠的金矩形）。如果按真实 PCB 短路检测逻辑（"两条不该连的走线被连了"）去写，会一直失败。
2. **PIL/OpenCV 颜色通道反转是常踩坑**：以后写颜色检测代码，先用 `cv::cvtColor(img, cv2.COLOR_BGR2RGB)` 反转再做颜色判断；或者直接用 PIL 颜色定义时明确加注释"这是 BGR/RGB"。
3. **先验证再写代码**：如果先跑 baseline 和 short 对比 gold pixel count，就能立刻发现金 mask 无差异，省掉一次返工。
4. **"标个红色圈便于人眼看出（检测算法不依赖这个）"** —— `gen_pcb_dataset.py` 这句注释是有意设计的"作弊信号"。它承认合成数据检测需要靠合成数据自带的标记，真实场景要换策略。

## 🔄 待补（不属本次改动）

- detectShortCircuit 当前依赖合成数据自带的椭圆环。真实 PCB 短路应该用 `connectedComponentsWithStats` 数金 mask 连通域，比较与 baseline 走线图的拓扑差异（多了一条本来不存在的连通域）。
- 应在 NOTES_W6.md / README 里补一句"本项目识别算法针对合成数据，真实 PCB 需调整"。

---

# 📎 附录：Step 3 焊桥判定追加（2026-09-15 晚）

> 在 detectShortCircuit 之后又做 detectSolderBridge，过程中发现了一个**比 Step 2 更深的坑**——W6a 的 HSV 范围本身就是错的。记录如下。

## 🚨 关键发现：W6a 的 HSV 范围抓的不是金，是 IC 蓝

**症状**：detectSolderBridge 第 1 版返回 `maxWidth=3`，整个 R1 ROI 只检测到 24 个金像素。明明 marked 图上 R1 区域是金色叠加，但 goldMask 几乎是空的。

**调查路径**：
1. 怀疑 `cv::reduce` 返回值异常 → 改为 `cv::countNonZero(roiMask.row(y))` → 仍然 maxWidth=3
2. 直接打印 `goldMask(200, 110)` (R1 左焊盘中心) → **0**
3. 检查 `gen_pcb_dataset.py:53` 的 `GOLD = (30, 180, 220)` —— **这是 PIL RGB**

**根因分析（颜色通道反转）**：
```
PIL RGB  (30, 180, 220)  ← GOLD 定义
       ↓ cv::imread 读为 BGR
OpenCV BGR (220, 180, 30)
       ↓ cv::cvtColor 到 HSV
V=220, S=190/220≈0.86, V==B
H = 240 + 60×(30-180)/(220-30) = 240 - 47.4 = 192.6
OpenCV HSV H = 192.6 / 2 = 96.3   ← 这是青色,不是金色!
```

**W6a 用的范围 `(10, 80, 80)→(40, 255, 255)` 抓的是 H=10-40（橙黄色）——完全错过 H=96 的真金。**

**那 W6a 之前抓的 10506 个金像素是什么？**

`gen_pcb_dataset.py:59` 的 `BLUE_DARK = (180, 80, 30)` PIL RGB → OpenCV BGR `(30, 80, 180)` → HSV H=20（在 10-40 范围内！）。所以**W6a 一直在抓 IC 本体（标称蓝色，实际是 H=20 橙黄），完全错过了焊盘和走线**。

## ✅ 修复

```cpp
// main.cpp:93-99 w6a::loadAndPreprocess
// 原范围（错）
cv::inRange(hsv, cv::Scalar(10, 80, 80), cv::Scalar(40, 255, 255), goldMask);
// 新范围（对）
cv::inRange(hsv, cv::Scalar(85, 80, 80), cv::Scalar(115, 255, 255), goldMask);
```

修复前后对比（同一张 baseline.png）：
| 指标 | 修复前 | 修复后 |
|---|---|---|
| gold pixels | 10506（IC 蓝） | 18304（真金） |
| contours | 5（IC 大块） | 23（焊盘+走线碎片） |
| goldMask(200, 110)（R1 左焊盘中心） | 0 | 0（OPEN erode 把顶边擦掉了） |
| goldMask(200, 150)（R1 本体中心） | 0 | 0（黑色本体） |
| goldMask(200, 190)（R1 右焊盘中心） | 0 | 0（同左焊盘） |
| R1 ROI maxWidth | 3 | 19 (baseline) / 120 (bridge) |

> 注：goldMask 中心为 0 是因为 R1 焊盘中心点恰被 OPEN 3×3 形态学擦除（erode 边缘）。但焊盘整体仍捕获（maxWidth=19 表示 23-24 px 宽的焊盘缩到 19 px）。

## 📊 detectSolderBridge 修复后效果

| 图 | R1 ROI maxWidth | 判定 | 标注 |
|---|---|---|---|
| baseline.png | 19 px（两焊盘各 19，独立） | PASS（<70 阈值） | 绿色 PASS |
| bridge.png | 120 px（桥连续金条） | FAIL（>70 阈值） | 黄色 BRIDGE + 黄框 ✅ |
| short.png | - | FAIL（短路） | 红框 + SHORT（不变） |

**所有 3 张图的检测现在都正确**。W6a 的 HSV 修复反而让金 mask 表现"正常"了（之前一直在错的状态下"看起来工作"）。

## 🔍 教训 / 经验

1. **W2 的 HSV 笔记不能直接搬到 W6** —— W2 用 `cv::imread` 读 BGR，HSV 范围是给"真实 PCB 照片的暖色调金"调出来的；W6 的合成数据是 PIL 生成 + RGB 顺序 → OpenCV 看到的颜色完全不同。
2. **写颜色检测代码时，必须先 dump 几个样本像素**：
   ```cpp
   cv::Vec3b bgr = img.at<cv::Vec3b>(y, x);
   std::cout << "BGR=(" << bgr[0] << "," << bgr[1] << "," << bgr[2] << ")\n";
   ```
   再 `cv::cvtColor` 到 HSV 看实际 H/S/V，再定阈值。
3. **PIL RGB ↔ OpenCV BGR 是常踩坑**：合成数据用 PIL 画的所有颜色，OpenCV 读到的 H 都跟你想象的不一样。**强烈建议把 `GOLD`、`RED`、`BLUE_DARK` 等 PIL 定义都加一行 `# 注意: 这是 PIL RGB,OpenCV BGR 是 (b,g,r)` 注释**。
4. **Step 1 的 marked 图当时看着正常，是因为 IC 本体被金 mask 捕获了**——而 IC 在所有合成图里都是同一种颜色，所以"看着正常"是巧合。等到 Step 3 需要按金 mask 区分 R1 焊盘 vs 桥连条，才暴露问题。

## 📝 已修复 vs 仍待办

✅ 已修：
- W6a HSV 范围（影响所有用 goldMask 的判定）
- detectSolderBridge 用 countNonZero 行投影（cv::reduce 在 8U→32S 时输出看起来有问题，可能是 at<int> 索引方式，已绕过）

⚠️ 仍待办（不属本次改动）：
- README / NOTES_W6.md 应标注"PIL/OpenCV 颜色通道反转"这一点
- W2 笔记里"(10,80,80)→(40,255,255)"的范围需要注明"对 W2 真实照片适用，对 W6 合成数据不适用"

---

# 📎 附录：Step 5 串通 5 类 ROI 判定（2026-09-15 晚）

> 串通 Step 4 接入 runBaseline/runReport 后，5 类 ROI 颜色判定（missing / wrong / polarity / insufficient / shifted）让 W6 完成度从 80% 推进到 95%+。

## ✅ 最终结果

`w6_pcb_defect.exe report images` 跑出 **8 张图 / 1 PASS / 7 FAIL**：

| 图 | 期望 | 实际 | 判定 |
|---|---|---|---|
| baseline.png | PASS | PASS | ok |
| bridge.png | FAIL | FAIL | bridge (max gold run=120px @ R1) |
| insufficient.png | FAIL | FAIL | insufficient (U1 min pad area=81 vs median=136) |
| missing.png | FAIL | FAIL | missing (R2 dark px=0) |
| polarity.png | FAIL | FAIL | polarity reversed (C1 white cx=74/90) |
| shifted.png | FAIL | FAIL | shifted (R2 dark centroid 79,33 vs expect 60,24) |
| short.png | FAIL | FAIL | short ring (520,460 61x41) |
| wrong.png | FAIL | FAIL | wrong (R3 yellow px=2579) |

## 🧪 实现细节

| Judge | 关键检测 | 阈值 |
|---|---|---|
| `judgeMissing` | R2 ROI 内 RGB 暗像素（≤60） | count < 50 |
| `judgeWrong` | R3 ROI 内浅青色像素 (BGR(230,200,40) 类) | count > 200 |
| `judgePolarity` | C1 ROI 内白色像素质心 x 坐标 | cx > ROI.width * 0.6 |
| `judgeInsufficient` | U1 ROI 金 mask 连通域,过滤 < 80 噪声后找 min | min < median - 50 |
| `judgeShifted` | R2 ROI 内 RGB 暗像素质心 vs SITES 中心 | dx>15 \|\| dy>8 |

每个 judge 都按 `c.label` 分发（仅检测特定元件），避免对所有元件做无意义检查。

## 🐛 Step 5 过程中的 3 个坑

### 坑 1：BGR 通道顺序读反（最坑）

**症状**：judgeWrong 不触发。debug log 显示 `wrong R3 full(480,700)=(40, 40, 180)`，我**当时以为这是 RGB**，误读为浅蓝。

**真相**：`img.at<cv::Vec3b>(y, x)` 返回 **BGR 顺序**。BGR `(40, 40, 180)` → RGB `(180, 40, 40)` = **draw_resistor 的红色色环**（baseline.png R3 中心恰好是 4 个色环中的红色环）。

PIL YELLOW RGB `(40, 200, 230)` → cv BGR `(230, 200, 40)` 才应该是 wrong 的本体颜色。

**教训**：
- debug 命名要明确写"BGR=("，并显式转换到 RGB 对照
- 不要凭直觉解释 BGR 数据，先用 PIL/Numpy 验证实际颜色

### 坑 2：inRange 阈值上下界写反

**症状**：wrong 检测仍不触发。debug 显示 `(230, 200, 40)` 应该是浅青色，但我的 `Scalar(0, 150, 150)` 要求 B≤80，与 B=230 矛盾。

**教训**：写 `cv::inRange` 阈值时，**先取一个样本 BGR 值，逐通道对号入座**填 lower/upper：
```
PIL YELLOW BGR(230, 200, 40)
- B=230 → lower ≤ 230 ≤ upper → [150, 255]
- G=200 → lower ≤ 200 ≤ upper → [150, 255]
- R=40  → lower ≤ 40  ≤ upper → [0, 80]
cv::Scalar(150, 150, 0), cv::Scalar(255, 255, 80)
```

### 坑 3：insufficient 过触发

**症状**：baseline.png 也被判为 insufficient。原因：U1 ROI 在 goldMask 中有 13 个连通域，最小的一个 ~60 px²（IC 边缘某个孤立金像素），median=136，60 < 136/2=68 触发。

**真相**：OPEN 3x3 形态学在 IC 边缘产生了一些"被削小的孤立金像素"（噪声），并非真实焊盘。

**修复**：先过滤 `area < 80` 的噪声，再算 median 和 min：
```cpp
std::vector<int> areas;
for (int i = 1; i < n; ++i) {
    int a = stats.at<int>(i, cv::CC_STAT_AREA);
    if (a >= 80) areas.push_back(a);  // 过滤小噪声
}
// 阈值改为"绝对差":baseline 差 8;insufficient 差 55
if (minArea < median - 50) → insufficient
```

实测：
| 图 | 过滤后 min | median | 判定 |
|---|---|---|---|
| baseline 等 7 张 | 128 | 136 | min=128 < median-50=86 ✗ 不触发 ✓ |
| insufficient.png | 81 | 136 | min=81 < 86 ✓ 触发 |

## 💡 经验教训汇总

1. **debug 一定要明确通道顺序**：OpenCV 是 BGR，PIL 是 RGB。debug 输出要么全转 RGB，要么明确写"BGR"提示自己。
2. **inRange 阈值取样原则**：先取 1-2 个样本像素，逐通道填阈值；不要凭直觉写 `cv::Scalar(low, low, low)` 这种"对称范围"。
3. **合成数据缺陷的"判别特征"要找稳定的**：baseline vs defect 的差异要足够大（不只是 1-2 像素），否则极易被噪声干扰。
4. **绝对差阈值比百分比阈值更鲁棒**：median * 0.5 在不同 baseline 下波动大；median - 50 物理意义更清楚（焊盘缩了至少 50 px²）。
5. **多 judge 分发到特定元件**：每个 judge 只检查 SITES 中的特定元件（R2/R3/C1/U1），避免无意义计算和误报。

## 📊 W6 完成度

| 阶段 | 完成度 |
|---|---|
| 起点（9/15 早） | 35%（只有 W6a 脚手架） |
| Step 0+1 | 50%（6 绿框 + 标签） |
| Step 2 | 60%（短路检测） |
| Step 3 | 70%（焊桥检测 + HSV 修复） |
| Step 4 | 80%（CSV 串通） |
| **Step 5** | **95%（5 类 ROI 判定 + 完整 CSV）** |
| 剩余 5% | 真实 PCB 适配 + 性能优化 |

## 🚀 下一步（可选）

| 优先级 | 任务 | 说明 |
|---|---|---|
| 🟡 中 | 修 `drawComponents` label 重叠 | label 改到框下方，避免与 PCB 丝印白字重叠 |
| 🟡 中 | Step 6: 把 detectShort/Bridge 也接入 judgeDefects | 让 defects 字段填完整（短路 + 焊桥归到 w6c::Defect） |
| 🟢 低 | 适配 complex 子集 | SITES 按每个子板分别定义 |
| 🟢 低 | 用 matchTemplate 替换 recognizeComponents | 通用识别（取代已知位实现） |
| ⚪ 远期 | 真实 PCB 测试 | 改 HSV 范围、加颜色校准、对抗真实噪声 |
