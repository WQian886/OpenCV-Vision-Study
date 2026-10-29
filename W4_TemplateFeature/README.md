# W4: 模板匹配 + ORB 特征点

## 🎯 学习目标
1. 理解"模板匹配"和"特征点匹配"的区别
2. 掌握 `cv::matchTemplate` 单目标滑动匹配
3. 掌握 `cv::minMaxLoc` 找最相似位置
4. 理解 `cv::ORB` 特征点 + 描述子
5. 掌握 `cv::BFMatcher` 暴力匹配 + `cv::findHomography` 单应性矩阵
6. **实战：在 PCB 上识别电阻、电容、IC**

## 📁 工程结构
```
W4_TemplateFeature/
├── CMakeLists.txt    # 找 OpenCV + 编译
├── main.cpp          # 只做 调它 + 给人看（存图/显示）
├── feature_pipeline.{h,cpp}  # 处理链（可测的那一层）+ 全部常量
├── tests/
│   └── test_w4_template_feature.cpp   # 无头回归测试
├── images/           # 测试图片 + 模板
└── README.md         # 本文件
```

## 🛠️ 如何构建（VS 方式）

1. VS 2022/2026 → **打开本地文件夹** → 选 `E:\qianwie\00study\OpenCV\W4_TemplateFeature`
2. VS 自动识别 CMakeLists.txt，弹出"已为此文件夹配置 CMake"的提示
3. 等 CMake 配置完成（右下角状态栏会显示）
4. **顶部配置切到 x64-Release**（必须 Release！Debug 会触发 OpenCV ABI 不匹配）
5. 按 **Ctrl+Shift+R** 重新 configure
6. 按 **F7** 编译
7. 按 **Ctrl+F5** 运行（不要 F5）

> 💡 必须用 **Ctrl+F5** 而不是 F5，否则调试器启动慢且会停在断点。

## 🛠️ 命令行构建（备选）

如果你装了 cmake 命令行工具：
```powershell
cd E:\qianwie\00study\OpenCV\W4_TemplateFeature
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\w4_template_feature.exe
```

## 📚 知识点笔记

### 模板匹配 vs 特征点匹配

| | 模板匹配 (matchTemplate) | 特征点 (ORB) |
|---|---|---|
| 输入 | 1 张小图(模板) | 1 张小图(模板) |
| 算法 | 滑动窗口 + 像素对比 | 提取关键点 + 描述子 |
| 抗旋转 | ❌ 不抗 | ✅ 抗 |
| 抗缩放 | ❌ 不抗 | ✅ 抗 |
| 速度 | 快 | 慢 |
| 适合 | 固定视角、目标不旋转 | 倾斜/缩放拍摄 |

**电气类比**：
- 模板匹配 = 用"信号模板"做相关检测（需要模板和信号对得上）
- 特征点 = 用"指纹"做匹配（旋转/缩放不影响指纹）

### matchTemplate 6 种 method

| Method | 含义 | 推荐 |
|---|---|---|
| `TM_SQDIFF` | 平方差,越小越相似 | — |
| `TM_SQDIFF_NORMED` | 归一化平方差 | — |
| `TM_CCORR` | 相关性,越大越相似 | — |
| `TM_CCORR_NORMED` | 归一化相关性 | ⭐ 简单场景 |
| `TM_CCOEFF` | 相关系数 | — |
| **`TM_CCOEFF_NORMED`** ⭐ | 归一化相关系数,范围 [-1, 1] | **工业首选** |

**返回值**：result 图，每个像素表示模板在对应位置与图像的相似度。

> ⚠ **「工业首选」是一条关于「目标 × method」的话，不是关于目标的话。**
> `TM_CCOEFF_NORMED` **去均值**，所以它比的是**起伏**。
> 拿**纯色**模板去试，它**几乎是瞎的**（实测 0.030943，位置跑到背景上去了），
> **而且不报错**。
> 实测对照见下面「跑测试」那一节。

### minMaxLoc 找最相似位置

```cpp
cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
```
- 对 `TM_CCOEFF_NORMED`：用 `maxLoc`（最相似的位置）
- 对 `TM_SQDIFF_*`：用 `minLoc`（最相似的位置）

### ORB 特征点 4 步流程

```
1. cv::ORB::create(nfeatures)        创建检测器
2. detector->detectAndCompute(img, mask, keypoints, descriptors)
                                         检测关键点 + 描述子
3. cv::BFMatcher matcher(NORM_HAMMING) 创建匹配器
4. matcher.match(descriptors1, descriptors2, matches)
                                         暴力匹配
5. cv::findHomography(srcPoints, dstPoints, RANSAC)
                                         单应性矩阵(去错配)
```

### findHomography + warpPerspective

- `findHomography`：算一个 3×3 矩阵,把模板的 4 个角点映射到大图
- `warpPerspective`：用这个矩阵把模板"扭"到大图位置（可视化用）

## 🧪 跑测试

W4 有 **1 个 ctest 套件、57 条断言**（2026-10-29 之前是 **0 个**）。

```powershell
cd E:\qianwie\00study\OpenCV\W4_TemplateFeature
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# ⚠ 必须带 -C Release,否则套件全报 Not Run
ctest --test-dir build -C Release --output-on-failure
```

| 套件 | 断言 | 测什么 |
|---|---|---|
| `w4_template_feature` | 57 | **整条链自己的测试**：**生成图**（640×480、8 个焊点、每个焊点那块确实是金色的、板面是深绿的、**同一个 seed 逐像素相同**、不同 seed 确实不同）、**两种模板**（抠出来的和原图那块**逐像素相同**、独立模板**不是同一张**但平均颜色一样、**裁剪越界返回空图**）、**六种 method 的差别**（**极性各断一次**、CCORR_NORMED 看得见纯色模板而 **CCOEFF_NORMED 几乎是瞎的（0.030900）**、**两种 method 的分数差了 0.950000**、**8 个焊点的分数散得只有 0.000580**、焊点确实比背景好）、**多目标匹配**（**朴素数像素数出 12376 个**、NMS 之后是 8 个且全落在焊点上、**阈值 0.90~0.98 个数都是 8**、0.99 一个不剩、**半径 40~100 都是 8**、半径 10 拆成 104 个）、**退化情形**（**纯色模板 + CCOEFF_NORMED 分数图每一处都是 1.0** ⇒「最相似位置」是 (0,0)、**从大图上抠的模板分数是白送的 1.0**）、**ORB**（关键点不超过 nfeatures、**100 个关键点在 500 那遍里也都在（100/100，容差 1 像素）**、**模板高 60 一个关键点都找不到且不报错**、文字模板 127 对好匹配、**单应性把四角还原回原位（0.76 像素）**、**拿金块当模板好匹配 0 对**、转 30° 照样认得出、**好匹配不足 4 对返回空矩阵**）、**整条链**（分数图尺寸 = 图 − 模板 + 1、红框真的画在匹配位置上、**跑两遍结果完全一样**） |

> 🔧 **为什么处理链被拆出去了**：
> 它原本整段写在 `main.cpp` 里，中间隔着 `imshow` / `waitKey` / `imwrite`
> —— **一个要开窗口的函数没法做无头测试**。
> 现在处理链在 `feature_pipeline.cpp`，**demo 和测试调的是同一份源码**
> （同一个 `feature_pipeline.cpp` 链进两个目标），
> **一条都不许在 `tests/` 里再重写一遍** —— 重写就是"抄副本"。
>
> 顺带解决了另外两个问题：
> **裁剪越界现在返回空图**（原来悄悄裁成能用的那一半 ——
> 半个模板照样能匹配，而且分数还挺高：那比"抠不到"糟得多）；
> **生成器现在带种子**（原来用 `cv::randn` 却没设种子，两次跑出来是两张不同的图）。
>
> ⚠ **「合成 PCB 图的事实」只有一份**：`common/pcb_sim/pcb_sim.h`
> （焊点颜色、板面底色、丝印颜色、噪声强度、图的尺寸、焊点位置）。
> W2 / W3 / W4 都从它取 —— 起因是 W4 原来的 `main.cpp` 里
> **内联抄了一份**生成器，而那行注释偏偏写着「**W3 同款** 8 个金色元件」。
> **注释说同款，代码是抄的。**

> ⚠ **这一轮最值钱的一条**：`TM_CCOEFF_NORMED` 是「工业首选」——
> **那话对有纹理的模板成立，对纯色模板不成立**。
> 实测（同一个金块模板，同一张图）：
>
> | method | 最佳分数 | 位置 |
> |---|---|---|
> | `TM_CCORR_NORMED` | **0.985996** | (200,100) 焊点上 ✅ |
> | `TM_CCOEFF_NORMED` | **0.030943** | (386,373) **背景上** ❌ |
>
> 原因：`CCOEFF` **去掉均值**，纯色模板只剩噪声可比 ——
> **「形状一样」这件事，在 CCOEFF 的眼里根本不存在**。
> 而它**不报错**：照样给你一张分数图、一个位置、一个看起来很正常的分数。
> ⇒ 「像」不是一个客观事实，是一个被 **method + 输入** 定义出来的量。
> 详见 [notes_W4_Tests_2026-10.md](../notes/notes_W4_Tests_2026-10.md)。

## ⚠️ 常见踩坑

| 现象 | 原因 | 修复 |
|---|---|---|
| `Debug` 编译失败 | CMakeLists 硬阻止 Debug | 切到 x64-Release |
| 模板匹配错位 | 模板是从大图抠的,匹配分数全 1.0 | 用独立拍的小图 |
| 分数很低但形状明明一样 | 用 CCOEFF 去匹配纯色模板（它去均值,只剩噪声可比） | 换 `TM_CCORR_NORMED`,或换有纹理的模板 |
| ORB 找不到特征点 | 图像太模糊或太小 | 先 `GaussianBlur` + 放大 |
| ORB **一个关键点都没有** | 模板比 `edgeThreshold=19`/`patchSize=31` 还矮（实测高 60 → 0 个） | 把模板裁高一点（实测高 80 → 162 个） |
| ORB 好匹配全是 0 | 目标**长得一模一样**（实测 8 个焊点 ⇒ ratio test 全毙掉） | 拿**唯一**的目标当模板 |
| `findHomography` 失败 | 匹配点对太少 | 至少 4 对匹配点 |

## ✏️ 课后练习

### 基础（W4a 模板匹配）
1. **实现 matchTemplate**：在 PCB 大图里找出模板位置，画红框
2. **试 6 种 method**：对比 `TM_SQDIFF` / `TM_CCOEFF_NORMED` 的 result 图
3. **多目标匹配**：设置阈值,找出所有相似度 > 0.9 的位置（不止 1 个）
4. **多尺度匹配**：用图像金字塔（缩小 0.8x、0.9x、1.1x、1.2x）找不同尺寸的目标
5. **替换真实模板**：用手机拍一个元件做模板,在 PCB 图里找

### 进阶（W4b ORB 特征点）
6. **ORB 单目标匹配**：把模板旋转 30°,看 ORB 能不能匹配
7. **画匹配连线**：用 `cv::drawMatches` 可视化两图的匹配点
8. **算单应性矩阵**：用 `findHomography` + `warpPerspective` 把模板对齐到大图
9. **多目标 ORB**：从 PCB 抠 3 个不同的元件做模板,在图里分别识别
10. **实战：识别电阻/电容/IC**：3 种元件各拍一张,在 PCB 图里找

## 🎓 下一节预告

W5: **OCR + 二维码**
- Tesseract OCR 读图中文字
- `wechat_qrcode` 解二维码
- 实战：PCB 上的元件丝印文字 + 二维码序列号
