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
├── main.cpp          # 模板匹配 + 特征点主程序
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

## ⚠️ 常见踩坑

| 现象 | 原因 | 修复 |
|---|---|---|
| `Debug` 编译失败 | CMakeLists 硬阻止 Debug | 切到 x64-Release |
| 模板匹配错位 | 模板是从大图抠的,匹配分数全 1.0 | 用独立拍的小图 |
| ORB 找不到特征点 | 图像太模糊或太小 | 先 `GaussianBlur` + 放大 |
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
