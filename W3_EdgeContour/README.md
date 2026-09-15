# W3: 边缘 + 轮廓 + 形态学

## 🎯 学习目标
1. 理解"边缘"和"轮廓"的区别
2. 掌握 `cv::Canny` 边缘检测
3. 掌握 `cv::findContours` 找轮廓
4. 掌握轮廓属性：面积、外接矩形、中心点
5. 掌握形态学操作：开运算 / 闭运算
6. **实战：用 W2 的 mask 数清楚 PCB 上有几个焊点**

## 📁 工程结构
```
W3_EdgeContour/
├── CMakeLists.txt
├── main.cpp
├── README.md
└── images/
    ├── pcb_test.jpg          ← 自动生成
    ├── w3_morph_mask.png     ← 形态学清理后的 mask
    ├── w3_edges.png          ← Canny 边缘
    └── w3_marked.png         ← 带红框+编号的 PCB
```

## 🛠️ 构建（VS 方式）
1. VS → 打开文件夹 → `E:\qianwie\00study\OpenCV\W3_EdgeContour`
2. CMake 自动配置 → F7 编译 → Ctrl+F5 运行
3. **配置必须是 Release**

## 📚 知识点

### 边缘 vs 轮廓

| | 边缘 (Edge) | 轮廓 (Contour) |
|---|---|---|
| 是什么 | 单个像素（梯度大的位置）| 连成线的像素集合 |
| 输出 | 二值图，每个白色像素是边缘 | 闭合曲线的点列表 |
| 类比 | 示波器上电压突变点 | 把突变点连起来形成波形线 |

### Canny 边缘检测（工业首选）

```cpp
cv::Canny(src, dst, lowThreshold, highThreshold);
```

**双阈值原理**：
- 强梯度（> highThreshold）→ 一定是边缘
- 弱梯度（< lowThreshold）→ 一定不是边缘
- 中间梯度 → 跟边缘连通才算

**经验**：低:高 ≈ 1:3（比如 50:150）

### findContours

```cpp
cv::findContours(image, contours, hierarchy, mode, method);
```

| 参数 | 常用值 | 含义 |
|---|---|---|
| mode | `RETR_EXTERNAL` | 只找最外层（焊点用这个）|
| mode | `RETR_TREE` | 找所有层级（嵌套轮廓用）|
| method | `CHAIN_APPROX_SIMPLE` | 压缩冗余点（推荐）|

### 轮廓属性

| 属性 | API | 用途 |
|---|---|---|
| 面积 | `cv::contourArea(c)` | 过滤小噪点 |
| 外接矩形 | `cv::boundingRect(c)` | 标注/裁剪 |
| 最小旋转矩形 | `cv::minAreaRect(c)` | 倾斜物体 |
| 中心点 | `cv::moments(c)` + m.m00 | 定位/对齐 |
| 周长 | `cv::arcLength(c, true)` | 形状判断 |

### 形态学操作

| 操作 | 公式 | 用途 |
|---|---|---|
| 腐蚀 erode | 缩小亮区 | 去小白点 |
| 膨胀 dilate | 扩大亮区 | 补小黑洞 |
| 开运算 open | 先 erode 后 dilate | **去小噪点**（保形状）|
| 闭运算 close | 先 dilate 后 erode | **补小洞**（保形状）|
| 形态学梯度 | dilate - erode | 边缘提取 |

**电气类比**：
- 开运算 = RC 低通滤波（去毛刺）
- 闭运算 = 施密特触发器回滞（去抖动）

## ✏️ 课后练习

1. **改最小面积**：把 `MIN_AREA=1000` 改成 `100`，看有什么小噪点漏过来
2. **改形态学**：把 kernel 从 3×3 改成 5×5，去噪更强但焊点边缘会被吃掉
3. **加闭运算**：在开运算后再做一次闭运算，看能否修复"虚焊"（部分像素缺失的金色焊点）
4. **改 Canny 阈值**：把 50/150 改成 30/100，检测更多弱边缘
5. **找最小焊点**：用 `std::min_element` 找面积最小的焊点
6. **统计信息**：把每个焊点的中心点坐标 (x, y) 也打印出来

## 🎓 下一节

W4: **模板匹配 + ORB 特征点**
- 已知元件长啥样 → 在 PCB 上找它
- 处理旋转、缩放、光照变化
- 实战：识别 PCB 上的电阻、电容、IC