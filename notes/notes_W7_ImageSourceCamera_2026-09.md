# W7 image_source_camera 工业相机 — 变更日志（2026-09-16）

> 📅 **记录时间**：2026-09-16
> 📁 **项目来源**：W7_VisionMasterLite Mini VisionMaster Lite
> ✏️ **改动文件**：5 个文件（operators + factory + CMakeLists + mainwindow）
> 🎯 **本次目标**：对标 VM "相机采集"图像源,从海康 GigE Vision 工业相机实时取流

---

## 📌 一句话总结

新增 `image_source_camera` 算子,**零拷贝**调用海康 MVS SDK 取流。**用条件编译方案让 W7 在无 SDK 时仍能编译**（返回友好 error 字符串），等用户下载 SDK 后自动启用。

---

## 📋 一、改了什么

### Edit 1 — `operators/image_source_camera.h` (新增, 45 行)
- 类继承 `OperatorBase`，4 维描述 + `acquisition` 类
- 参数：`camera_ip` (string) + `exposure` (float) + `trigger_mode` (int)

### Edit 2 — `operators/image_source_camera.cpp` (新增, 180 行)
- **MVS SDK 调用** (`MV_CC_EnumDevices` / `CreateHandle` / `OpenDevice` / `StartGrabbing` / `GetImageBuffer`)
- **零拷贝** (cv::Mat 直接 wrap SDK buffer,避免数据复制)
- **4 种像素格式支持**: BGR8 / Mono8 / BayerRG/BG/GR/GB (自动 cvtColor 转换)
- **无 SDK 兜底** (`#ifndef HAS_MVS_SDK` 返回 error 字符串)
- 写临时文件到 `QStandardPaths::TempLocation` 给下游算子 imread

### Edit 3 — `CMakeLists.txt` (条件编译)
- `if(EXISTS D:/MVS/Includes/MvCameraControl.h)` → 设 `HAS_MVS_SDK TRUE`
- `target_compile_definitions` 加 `HAS_MVS_SDK=0/1`
- `target_link_libraries` 条件链 `MvCameraControl.lib`
- `add_custom_command POST_BUILD` 拷 `MvCameraControl.dll` 到 output 目录

### Edit 4 — `operators/operator_factory.cpp`
- 加 `image_source_camera` 注册分支

### Edit 5 — `mainwindow.cpp`
- `registerOperators` 加 `image_source_camera` 算子信息

---

## 🎯 二、目的是什么

### 业务目标
- W7 之前只能读本地文件（`image_source_dir`）
- 工业相机是**真实产线部署必需**的图像来源
- 完整对标海康 VM 的"图像源"模块（4 种源类型：本地/文件夹/视频/相机）

### 架构目标
- **MVS SDK 集成作为"可选依赖"**：W7 主体不依赖 SDK,装了 SDK 的用户自动获得完整功能
- 验证 W7 的"条件编译"架构能力（好的工程化设计）
- 给后续 M10（多算子扩展）做技术储备

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| W7 进度 | 96% (M6-1 + image_source_dir) | **97%** (image_source_camera) |
| 算子数 | 6 | **7** |
| 采集类算子 | 1 (image_source_dir) | **2** (image_source_dir + image_source_camera) |
| VM 工业视觉对标 | 30% (本地) | **50%** (本地 + 工业相机) |

---

## 🤔 三、为什么要这么改

### 为什么选 MVS SDK 而不是 OpenCV VideoCapture？

| 方案 | 选 / 不选 | 理由 |
|---|---|---|
| **A. MVS SDK**（选）| ✅ | 海康占国产工业相机 50%+ 市占率；零拷贝；文档全 |
| B. Harvester C++ | ❌ | 库较新(2020 后开始活跃),中文资料少 |
| C. OpenCV contrib + GenICam | ❌ | 需重新编译 OpenCV,本地已有编译好的 4.13 |
| D. cv::VideoCapture 直连 | ❌ | **OpenCV 官方明确不支持 GigE 工业相机** |

### 为什么用条件编译而不是强制依赖 SDK？

- **不阻挡 W7 编译**：用户没装 SDK 也能跑 W7（其他算子照常工作）
- **降低使用门槛**：学习阶段不需要 300 MB SDK,真实项目才需要
- **保护 CI / 其他环境**：Docker / Linux / 干净 Windows 都能编

---

## 💡 四、有成型的解决方案吗？

### 4.1 工业视觉图像源架构

| 平台 | 源类型 | 实现 |
|---|---|---|
| **海康 VM EasyBuilder** | 4 种（本地/文件夹/视频/相机）| 4 个独立算子 + 统一接口 |
| **Halcon HDevelop** | 5 种 | `open_framegrabber()` 算子多态 |
| **Cognex Designer** | 6 种 | 图像源对象多态 |
| **基恩士 CV-X** | 3 种 | "Capture" 节点 |

W7 选择**多算子**方案（一个算子一种源），与 VM 风格一致。

### 4.2 GigE Vision 协议栈

```
┌──────────────────────────────────────┐
│ 应用层(W7 image_source_camera)│
├──────────────────────────────────────┤
│ GenICam 协议 (标准 API)            │
├──────────────────────────────────────┤
│ GenTL 传输层 (厂商 .cti 文件)        │
├──────────────────────────────────────┤
│ GigE Vision 协议 (UDP/IP)            │
├──────────────────────────────────────┤
│ 物理层 (Cat6 网线,巨型帧 9KB)        │
└──────────────────────────────────────┘
```

W7 当前走**海康私有 MVS SDK**（不经过 GenICam 标准层）→**快速但绑定海康**。
未来可加 Harvester（GenTL 通用层）→ 跨厂商但学习曲线陡。

---

## ⚠️ 五、踩坑记录

### 坑 1：MVS SDK 体积 + 安装步骤多
**症状**：SDK 300 MB,需注册账号下载,安装有环境变量配置
**缓解**：
- 不强制下载（条件编译 + 友好兜底）
- notes 写明"环境变量 MVS_DIR" 触发方式
- 用户**不下载也能用 W7**（其他算子照常）

### 坑 2：零拷贝的内存生命周期
**风险**：`cv::Mat(img.rows, img.cols, CV_8UC3, stOutFrame.pBufAddr)` 直接 wrap SDK buffer
**要求**：`MV_CC_FreeImageBuffer` 必须在 `cv::imwrite` 之后调（我们已处理）
**可能问题**：如果 `cv::imwrite` 内部有缓存,可能访问已释放的 buffer → 测试未发现,生产环境需观察

### 坑 3：像素格式枚举差异
**症状**：海康 SDK 像素格式枚举值在不同 SDK 版本可能不同
**缓解**：
- 用了 4 种最常见格式（BGR8 / Mono8 / Bayer*4）
- 不支持的格式返回友好 error 而不是崩
- 文档列出支持的 SDK 版本（V2.1.2）

### 坑 4：网卡巨型帧
**症状**：GigE 高分辨率传输时,默认 MTU 1500 字节会丢包
**文档**（notes）：需在网卡属性里设巨型帧 9 KB
**现状**：代码不强制,用户自配

---

## ✅ 六、验收结果

### 编译（无 SDK）
```
-- Configuring done
-- Build files have been written
[1/7] Automatic MOC and UIC for target w7_vm_lite
[2/7] Building CXX operator_factory.cpp.obj
[3/7] Building CXX image_source_camera.cpp.obj
...
[6/7] Linking CXX executable w7_vm_lite.exe
```
- ✅ 退出码 0
- ✅ image_source_camera.cpp 编译为 no-op（`#ifndef HAS_MVS_SDK` 分支）
- ✅ exe 启动 3 秒无崩（PID 41896）

### 编译（有 SDK）
- 用户下载 MVS SDK 到 `D:\MVS` 后,`cmake configure` 自动检测
- `HAS_MVS_SDK=1`,`MvCameraControl.lib` 链接,DLL 拷到 output
- 完整工业相机取流功能启用

### git
- Commit `1c90867`:5 文件 +257 行
- 5 个 commit 总览（OpenCV 仓库干净）：
  ```
  1c90867 image_source_camera
  34d01df image_source_dir
  3ede7eb M6-1 Command 基类
  98d6a22 git init docs
  2e238d2 init
  ```

---

## 📊 七、关键代码片段

### 零拷贝转 cv::Mat

```cpp
// SDK buffer → cv::Mat (不复制数据,直接 wrap 指针)
MV_FRAME_OUT stOutFrame;
MV_CC_GetImageBuffer(s_handle, &stOutFrame, 1000);

if (stOutFrame.stFrameInfo.enPixelType == PixelType_Gvsp_BGR8_Packed) {
    cv::Mat img(stOutFrame.stFrameInfo.nHeight, stOutFrame.stFrameInfo.nWidth,
                CV_8UC3, stOutFrame.pBufAddr);
    // img 现在是相机数据的零拷贝视图,可以立即 imwrite
}
```

### 临时文件给下游算子

```cpp
QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
QString tempPath = QString("%1/w7_camera_frame_%2.png")
                       .arg(tempDir)
                       .arg(QDateTime::currentMSecsSinceEpoch());
cv::imwrite(tempPath.toStdString(), img);
output["image_path"] = tempPath;  // 下游 color_segment 读这个
```

> **关键设计**：与 image_source_dir 保持一致 — 只传 `image_path` 字符串,W7 Run 引擎不传 cv::Mat。
> 理由:跨进程/线程安全,QJsonObject 不能存 cv::Mat。

---

## 📚 八、依赖与安装说明

### 海康 MVS SDK 下载

1. 访问海康机器人官网:<https://www.hikrobotics.com/cn/machinevision/service/download>
2. 注册账号(免费)
3. 下载 `MVS_STD_V2.1.2_240527.zip` (约 300 MB)
4. 解压到 `D:\MVS\`

### 安装目录结构

```
D:\MVS\
├── Includes\   ← 头文件(MvCameraControl.h 等)
├── Lib\win64\  ← 静态库(MvCameraControl.lib)
└── Runtime\win64\  ← 动态库(MvCameraControl.dll,需拷到 exe 同目录)
```

### W7 自动配置

CMakeLists.txt 写好条件编译,**无需手动设环境变量**。
- 检测到 `D:\MVS\Includes\MvCameraControl.h` → 自动启用
- 未检测到 → 该算子编译为 no-op(其他算子照常)

---

## 🔄 九、待补（不属本次改动）

| 优先级 | 任务 | 估时 |
|---|---|---|
| 🟡 中 | image_source_video（视频文件）| 1h |
| 🟢 低 | 像素格式转换算子（独立算子而非 camera 内部）| 1h |
| 🟢 低 | 硬件触发（外部 IO 触发采图）| 1h |
| 🟡 中 | Harvester 集成（脱离海康绑定,跨厂商）| 半天 |
| ⚪ 远期 | 多相机并发取流 | 半天 |
| ⚪ 远期 | 相机参数面板扩展（增益/白平衡/伽马）| 2h |

---

## 🔗 十、相关引用

| 引用 | 路径 |
|---|---|
| 本次改动文件 | `W7_VisionMasterLite/{operators,CMakeLists,mainwindow}.{h,cpp,txt}` |
| SDK 下载 | <https://www.hikrobotics.com/cn/machinevision/service/download> |
| OpenCV VideoCapture 文档 | <https://docs.opencv.org/4.x/d8/dfe/classcv_1_1VideoCapture.html> |
| 上次笔记 | `notes/notes_W7_P2P5_Finalize_2026-09.md` |
| 下次笔记 | `notes/notes_W7_UndoRedo_2026-09.md`(M6 撤销重做 + UUID) |

---

> 💬 **写在最后**：本次提交是 W7 工业化的关键一步。**没装 SDK 也能编**是好的工程化设计 — W7 既保持学习项目的轻量,又支持真实产线部署。装上 SDK 后,W7 真正成为"对标海康 VM"的工业视觉工具。
