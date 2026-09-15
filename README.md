# OpenCV 学习路线 (W1-W7)

> 📅 **初始化**：2026-09-15
> 🎯 **目标**：7 周系统学习 OpenCV,从图像处理基础到工业视觉应用
> 🛠️ **技术栈**：C++17 + OpenCV 4.13 + CMake + Qt 6.8.3 + MSVC

## 📚 学习周表

| 周 | 主题 | 进度 | 入口 |
|---|---|---|---|
| **W1** | 图像 I/O + 像素操作 | ✅ 100% | [README](./W1_ImageIO/README.md) |
| **W2** | 色彩空间 + 滤波 + 阈值 | ✅ 100% | [README](./W2_ColorFilter/README.md) |
| **W3** | 边缘 + 轮廓 + 形态学 | ✅ 100% | [README](./W3_EdgeContour/README.md) |
| **W4** | 模板匹配 + ORB 特征 | ✅ 100% | [README](./W4_TemplateFeature/README.md) |
| **W5** | OCR + 二维码 | ✅ 100% | [README](./W5_OCR_QRCode/README.md) |
| **W6** | PCB 缺陷检测综合项目 | ✅ 95% | [README](./W6_PCB_DefectProject/README.md) |
| **W7** | Mini VisionMaster Lite (Qt) | ✅ 95% | [README](./W7_VisionMasterLite/README.md) |

## 🏆 两个综合项目

- **W6 PCB 缺陷检测** — 命令行工具，识别 7 类 PCB 缺陷（缺件/错件/极性反/焊桥/焊少/偏移/短路），输出 CSV 报告
- **W7 Mini VisionMaster Lite** — Qt 流程图编辑器，对标海康 VM EasyBuilder，支持 5 个算子 + 拖拽连线 + DAG 执行 + JSON 持久化

## 📖 变更日志

详细改动记录见 [notes/](./notes/) 目录（共 6 份，96 KB）：

- `notes_Qt_DragDrop_2026-09.md` (16 KB) — W7 S1 拖拽原理
- `notes_Qt_NodeConnection_BugFix_2026-09.md` (18 KB) — W7 S2 连线 + 3 个 bug
- `notes_W6_ComponentRecognition_2026-09.md` (28 KB) — W6 五步完整改动
- `notes_W7_DAG_RunEngine_2026-09.md` (13 KB) — W7 P0 Run 引擎按图执行
- `notes_W7_SaveLoad_2026-09.md` (13 KB) — W7 P1 Save/Load JSON
- `notes_W7_P2P5_Finalize_2026-09.md` (9 KB) — W7 P2-P5 收尾汇总

## 🔨 编译

每个 W 独立 CMake 构建：

```powershell
# 以 W6 为例
cd W6_PCB_DefectProject
cmake --build out/build/x64-Release --target w6_pcb_defect

# 运行
out/build/x64-Release/w6_pcb_defect.exe report images
```

各 W 的具体命令详见各自 README。

## 📂 项目结构

```
OpenCV/
├── W1-W7/                 # 7 个学习项目
├── notes/                 # 变更日志（6 份）
├── datasets/              # 共享数据集（空,占位）
├── images/                # 共享图片（空,占位）
├── .gitignore             # Git 忽略规则
└── README.md              # 本文件
```

## 📊 统计

- 7 个 C++ 项目
- ~6000 行代码
- 96 KB 变更日志
- 1.9 GB 项目体积（其中 .vs/build 占 95%，git 仓库仅 ~5 MB）
