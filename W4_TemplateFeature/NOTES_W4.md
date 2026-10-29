# W4 学习笔记：模板匹配 + ORB 特征点 — 术语详解

> 📌 **给电气工程师的图像处理术语速查卡** —— W4 出现的 8 个核心术语，每个讲"是什么 / 原理 / 类比 / 用途"。

---

## 1️⃣ 模板匹配（Template Matching）

**是什么**：在一张大图里**滑动一个已知小图**，找出最相似位置的算法。

**原理**：
1. 把模板（已知小图）在大图上**逐像素滑动**
2. 每个位置算一次"相似度"
3. 输出 result 图，每个像素 = 该位置的相似度
4. `minMaxLoc` 找最值 = 模板在大图里的位置

**电气类比**：
- **相关检测器**——发射已知信号 `x(t)`，在接收信号 `y(t)` 里滑动，求 `∫x(τ)y(t+τ)dτ` 最大值的位置
- 模板匹配就是这个的 2D 版本

**PCB 用途**：
- ✅ 找已知形状的元件（电阻封装、电容封装）
- ✅ 印刷缺陷检测（找完整模板是否在图里）

**关键 API**：
```cpp
cv::matchTemplate(img, tpl, result, method);
// result 是单通道浮点图,每个像素 = 模板在该位置的相似度
```

---

## 2️⃣ matchTemplate 的 6 种 method

| Method | 公式思路 | 取最值 | 范围 | 用途 |
|---|---|---|---|---|
| `TM_SQDIFF` | (T-I)² 求和 | minLoc | [0, ∞) | — |
| `TM_SQDIFF_NORMED` | 归一化 SQDIFF | minLoc | [0, 1] | — |
| `TM_CCORR` | T·I 相关 | maxLoc | [-∞, ∞) | — |
| `TM_CCORR_NORMED` | 归一化相关 | maxLoc | [-1, 1] | 简单场景 |
| `TM_CCOEFF` | 去均值相关 | maxLoc | [-∞, ∞) | — |
| **`TM_CCOEFF_NORMED`** ⭐ | 去均值 + 归一化 | maxLoc | **[-1, 1]** | **工业首选** |

**为什么 `TM_CCOEFF_NORMED` 最常用？**
- **去均值**：光照变化不影响（亮的图和暗的图都能匹配）
- **归一化**：结果在 [-1, 1]，易于设阈值（如 >0.9 算匹配）
- 1 = 完全相同，0 = 不相关，-1 = 完全相反

> ⚠️ **2026-10-29 实测补的一条限定**（这一条教材不写）：
> **「去均值」不是免费的。**
>
> 同一个金块模板、同一张 PCB 图：
>
> | method | 最佳分数 | 位置 |
> |---|---|---|
> | `TM_CCORR_NORMED` | **0.985996** | (200,100) 焊点上 ✅ |
> | `TM_CCOEFF_NORMED` | **0.030943** | (386,373) **背景上** ❌ |
>
> **为什么**：`TM_CCOEFF` **去掉均值** ⇒ 纯色模板**没有起伏**，
> 去掉均值之后能比的只剩**噪声** —— 而模板的噪声和图上的噪声是两回事，
> 相关性本来就该接近 0。
> ⇒ **「形状一样」这件事，在 CCOEFF 的眼里根本不存在。**
>
> ⚠️ 而且它**不报错**：照样给你一张分数图、一个位置、一个看起来很正常的分数。
>
> ⇒ **「工业首选」不是一条关于目标的话，是一条关于「目标 × method」的话。**
> 纯色模板用 `TM_CCORR_NORMED`；有纹理的模板用 `TM_CCOEFF_NORMED`。
> 详见 `../notes/notes_W4_Tests_2026-10.md`。

**返回值图尺寸**：
```
result 大小 = (img.cols - tpl.cols + 1) × (img.rows - tpl.rows + 1)
```

---

## 3️⃣ minMaxLoc — 找最值位置

**是什么**：在一张图里同时找最小值、最大值和它们的位置。

**API**：
```cpp
cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
```

**怎么用**：
- `TM_CCOEFF_NORMED` → 用 **`maxLoc`**（相似度最高）
- `TM_SQDIFF_*` → 用 **`minLoc`**（差异最小）

**多目标匹配技巧**：
- 不用 minMaxLoc，直接遍历 result，设阈值（如 > 0.9）
- 找出所有相似度高的位置

---

## 4️⃣ ORB 特征点 ⭐⭐⭐

**是什么**：ORB = Oriented FAST + Rotated BRIEF。一种**快 + 免费**的特征点检测+描述算法。

**原理**：
1. **FAST 角点检测**：找"周围一圈像素和中心差异大"的位置
2. **方向计算**：给每个关键点算一个主方向（让描述子有旋转不变性）
3. **BRIEF 描述子**：用关键点周围的 256 对像素比较结果，编码成 32 字节的二进制串

**电气类比**：
- 关键点 = 信号的"突变点"（边沿）
- 描述子 = 突变点周围的"指纹"（和周围关系）

**PCB 用途**：
- ✅ 元件识别（电阻 vs 电容 vs IC 的"指纹"不一样）
- ✅ 倾斜校正（即使元件旋转了 30° 也能匹配）

**关键 API**：
```cpp
auto orb = cv::ORB::create(500);  // 最多 500 个关键点
std::vector<cv::KeyPoint> kps;
cv::Mat desc;
orb->detectAndCompute(img, cv::noArray(), kps, desc);
// kps = 关键点列表(位置+方向+大小)
// desc = 描述子矩阵(N x 32,每行一个关键点的 256 位二进制)
```

---

## 5️⃣ BFMatcher — 暴力匹配器

**是什么**：把模板的每个特征点和大图的所有特征点比一遍，找最近的 K 个。

**API**：
```cpp
cv::BFMatcher matcher(cv::NORM_HAMMING);  // ORB 用汉明距离(位不同个数)
std::vector<cv::DMatch> matches;
matcher.match(descTemplate, descBigImg, matches);
// matches 按距离从小到大排序
```

**电气类比**：像一个**全网比对器**——每个模板特征点都在大图里找"最像的 N 个"。

**匹配质量筛选**：
```cpp
// 距离 < 2 * min_distance 的算"好匹配"
double minDist = 100;
for (const auto& m : matches) minDist = std::min(minDist, m.distance);
std::vector<cv::DMatch> good;
for (const auto& m : matches) {
    if (m.distance < 2 * minDist) good.push_back(m);
}
```

---

## 6️⃣ DMatch 匹配结果

**是什么**：一个匹配对，包含 4 个字段：

```cpp
struct DMatch {
    int queryIdx;     // 模板的第几个关键点
    int trainIdx;     // 大图的第几个关键点
    float distance;   // 距离(越小越相似)
    // 其它...
};
```

**怎么看 matches**：
- `matches[i].queryIdx` → 这是模板上的第 i 个特征
- `matches[i].trainIdx` → 它在大图上对应的特征编号
- `matches[i].distance` → 两个特征的"距离"（越小越像）

---

## 7️⃣ findHomography — 单应性矩阵 ⭐⭐

**是什么**：根据 N 对匹配点，算一个 **3×3 矩阵 H**，满足：

```
大图点 = H × 模板点
```

**API**：
```cpp
std::vector<cv::Point2f> srcPts, dstPts;  // 从 matches 里提取
cv::Mat H = cv::findHomography(srcPts, dstPts, cv::RANSAC);
```

**RANSAC 是什么**：从一堆匹配点里，**剔除错配**（外点），用内点算矩阵。

**前提**：至少 **4 对**好的匹配点（4 对刚好能算出 H）。

**电气类比**：**坐标变换矩阵**——把模板坐标系"扭"到大图坐标系。

---

## 8️⃣ warpPerspective — 透视变换

**是什么**：用 `findHomography` 算出的 H，把模板"扭"到大图位置。

**API**：
```cpp
cv::Mat warped;
cv::warpPerspective(templateImg, warped, H, bigImg.size());
```

**用途**：可视化——把模板"贴"到大图里，看匹配对不对。

---

## 🎯 PCB 检测完整管线（W2 + W3 + W4）

```
原始 PCB 图
   ↓ bilateralFilter (W2)              ← 去噪保边
   ↓ cvtColor → HSV (W2)               ← 色彩空间
   ↓ inRange(金色HSV) → mask (W2)      ← 颜色提取
   ↓ morphologyEx OPEN (W3)            ← 去小噪点
   ↓ findContours (W3)                 ← 找焊点位置
   ↓ contourArea 过滤 (W3)             ← 区分焊点 vs 噪点
   ↓ boundingRect (W3)                 ← 给出每个元件 ROI
   ↓
   ↓ ┌─── W4a: matchTemplate(roi, template)
   ↓ │      → 识别"这是电阻还是电容"
   ↓ │
   ↓ └─── W4b: ORB + BFMatcher + findHomography
   ↓        → 抗旋转/缩放的元件识别
   ↓
"PCB 上有 5 个电阻、2 个电容、1 个 IC,位置/角度已记录"
```

---

## 📚 一句话总结 W4

**模板匹配 = 滑动对比 (固定视角), ORB 特征点 = 指纹匹配 (抗旋转)。PCB 元件识别首选: matchTemplate 简单场景, ORB 复杂场景。**

---

## 🔧 8 个工具决策表

| 目标 | 选哪个 | 为什么 |
|---|---|---|
| 找 1 个不旋转的元件 | matchTemplate + TM_CCOEFF_NORMED | 最简单最快 |
| 找多个相似元件 | matchTemplate + 阈值遍历 | 多目标检测 |
| 抗旋转匹配 | ORB + BFMatcher | 工业首选 |
| 算元件在大图里的旋转角 | ORB + findHomography | 唯一方法 |
| 匹配质量筛选 | 距离 < 2 × minDist | 经验阈值 |
| 可视化匹配 | drawMatches | 调试用 |
| 透视变换对齐 | warpPerspective | 标注用 |
| 找不同尺寸的元件 | 图像金字塔 + matchTemplate | 多尺度 |

---

## 🎓 自我检测

试着口述回答：

1. **`TM_CCOEFF_NORMED` 和 `TM_SQDIFF` 取最值时分别用 maxLoc 还是 minLoc？**
   → CCOEFF 用 maxLoc（越大越像），SQDIFF 用 minLoc（越小越像）

2. **ORB 描述子的大小是？**
   → 32 字节 = 256 位二进制

3. **`findHomography` 至少需要几对匹配点？**
   → 4 对（刚够算 8 个自由度的 H 矩阵）

4. **RANSAC 在 `findHomography` 里做什么？**
   → 剔除错配（外点），只用内点算 H

5. **matchTemplate 不抗什么？**
   → 不抗旋转、不抗缩放（这就是为什么需要 ORB）
