# W5 学习笔记：OCR + 二维码 — 术语速查卡

> 📌 **W5 出现的 6 个核心术语**，每个讲"是什么 / API / 类比 / 用途"。

---

## 1️⃣ QR Code（二维码）

**是什么**：一种 2D 条形码，能存储 100~4000+ 字符。比 1D 条码信息密度高得多。

**结构**：
- 3 个大方框（左上/右上/左下）= 定位标记
- 中间小方块 = 对齐标记
- 黑白模块 = 数据

**PCB 用途**：序列号、批次号、工厂代码。

---

## 2️⃣ cv::QRCodeDetector

**是什么**：OpenCV 4.x **核心模块**自带的 QR 检测+解码器（**不需要 contrib**）。

**API**：
```cpp
cv::QRCodeDetector detector;

// 检测+解码一行搞定
std::vector<cv::Point2f> points;
std::string decoded = detector.detectAndDecode(img, points);
// decoded = 解码内容 (空字符串表示没找到)
// points  = 4 个角点 (左上/右上/右下/左下)
```

**支持格式**：QR Code、Aztec、CODABAR 等（不只 QR）。

---

## 3️⃣ 4 个角点（points）

**是什么**：检测到的二维码在图中的位置，按 **左上 → 右上 → 右下 → 左下** 顺序。

**用途**：画框、计算中心、ROI 裁剪。

```cpp
cv::Point2f topLeft = points[0];
cv::Rect qrRect(topLeft.x, topLeft.y,
                points[2].x - topLeft.x,
                points[2].y - topLeft.y);
```

---

## 4️⃣ 检测 vs 解码（detect / decode）

| | detect | decode |
|---|---|---|
| 做什么 | 找二维码在图里的位置 | 把二维码里的内容读出来 |
| API | `detector.detect(img, points)` | `detector.decode(img, points)`)` |
| 返回值 | 是否成功 + 4 个角点 | 解码的字符串 |

**常用组合**：`detectAndDecode` = 一步完成。

---

## 5️⃣ 与 wechat_qrcode 的区别

| | cv::QRCodeDetector (核心) | cv::wechat_qrcode::WeChatQRCode (contrib) |
|---|---|---|
| 抗模糊 | 一般 | ⭐⭐⭐⭐ 强 |
| 多码识别 | ❌ 单码 | ✅ 同时多个 |
| 距离容忍 | 一般 | ⭐⭐⭐ 大小都能识别 |
| 安装 | OpenCV 自带 | 需 contrib |

**本工程用核心版**——满足 PCB 序列号识别。

---

## 6️⃣ QR 测试图生成（Python 工具）

**qrcode 库**一行代码生成 QR：

```python
import qrcode
qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M,
                   box_size=10, border=4)
qr.add_data("PCB-2026-001-SN:A12345X")
qr.make(fit=True)
qr.make_image().save("qrcode_test.png")
```

---

## 🎯 PCB 检测典型管线（加入 QR）

```
W2 提取金色 mask
   ↓
W3 数焊点 + 找位置
   ↓
W4 模板匹配(识别元件)
   ↓
W5a QR Code 检测 ← 你在这
   │ detector.detectAndDecode(img)
   │ → 拿到 PCB 序列号
   ↓
"PCB-2026-001, 8 个焊点全是 100Ω 电阻, 序列号 X"
```

---

## 📚 一句话总结 W5a

**QRCodeDetector 一行 API 解码,核心模块自带,不需要 contrib。**

---

## 🔧 6 个 API 速查

| API | 用途 |
|---|---|
| `detector.detect(img, points)` | 仅检测位置 |
| `detector.decode(img, points)` | 仅解码 |
| `detector.detectAndDecode(img, points)` | 检测+解码 ⭐ |
| `detector.detectMulti(img)` | 检测多个候选位置 |

---

## 🎓 自我检测

1. **QRCodeDetector 在哪个模块？**
   → objdetect（核心），不需要 contrib

2. **`detectAndDecode` 返回什么？**
   → string（解码内容）+ vector<Point2f>（4 个角点）

3. **QR 码的 3 个大方框有什么用？**
   → 定位标记，告诉扫描器"二维码在哪个方向"

4. **PCB 上为什么用 QR 码？**
   → 信息密度高、容错、可存序列号/批次号

5. **抗模糊该用哪个？**
   → wechat_qrcode（contrib），不是 QRCodeDetector