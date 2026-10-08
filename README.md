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
| **W7** | Mini VisionMaster Lite (Qt) | ✅ 最终版 | [README](./W7_VisionMasterLite/README.md) |

## 🏆 两个综合项目

- **W6 PCB 缺陷检测** — 命令行工具，识别 7 类 PCB 缺陷（缺件/错件/极性反/焊桥/焊少/偏移/短路），输出 CSV 报告
- **W7 Mini VisionMaster Lite** — Qt 流程图编辑器，对标海康 VM EasyBuilder。14 个算子 + pin 类型检查 + 每步结果查看 + 帧循环 + JSON 持久化 + 撤销重做，11 个无头回归测试共 484 条断言

### W7 最终版补上的东西

| 能力 | 说明 |
|---|---|
| 4 种图像源 | 本地 / 文件夹 / 视频 / 工业相机（对标 VM 四种） |
| 帧循环 | 源输出 `frame_count`，引擎按 `frame_index` 逐轮跑整条链 + 批量 PASS/FAIL 汇总 |
| W6 深度联动 | `pcb_inspect` + `pcb_defect_judge` 把 PCB 算法挂进画布，7 类缺陷 9 个阈值可调 |
| DL 算子 | `dl_postprocess`（letterbox/NMS/YOLOv5-v8 解析）+ `dl_infer`（ONNX Runtime，条件编译） |
| 测量类 | `line_fit`（长度+角度）/ `circle_fit`（圆心+半径+圆度）/ `caliper_dist`（点到线距离），补齐 VM 四大模块最后一块 |
| 每步结果查看 | 节点状态点 + 右键回看任意一步的图和全部字段 |
| pin 类型检查 | 算子声明吃/吐什么，连错时立刻说明原因 |
| 最近工程 | 记最近 5 个 `.sol.json`，自动剔掉已删除的 |

## 🧪 W7 回归测试

```powershell
cd W7_VisionMasterLite
cmd /c '"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "D:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build --output-on-failure'
```

11 个套件 / 484 断言，`QT_QPA_PLATFORM=offscreen` 无需显示器。
其中 `pcb_operators` 直接把 W6 当时的肉眼验收清单（8 张样本图）变成可重复执行的测试。

> 测试失败时想看是哪条断言挂了，CMakeLists 里已经给 11 个套件都设了
> `QT_FORCE_STDERR_LOGGING=1`。不加的话 Qt 6 在 Windows 上会把 `qInfo`
> 送到调试通道而不是 stderr，`--output-on-failure` 只能看到一个非零退出码。

## 📖 变更日志

详细改动记录见 [notes/](./notes/) 目录（共 9 份，~150 KB）：

- `notes_Qt_DragDrop_2026-09.md` (16 KB) — W7 S1 拖拽原理
- `notes_Qt_NodeConnection_BugFix_2026-09.md` (17 KB) — W7 S2 连线 + 3 个 bug
- `notes_W6_ComponentRecognition_2026-09.md` (27 KB) — W6 五步完整改动
- `notes_W7_DAG_RunEngine_2026-09.md` (13 KB) — W7 P0 Run 引擎按图执行
- `notes_W7_SaveLoad_2026-09.md` (13 KB) — W7 P1 Save/Load JSON
- `notes_W7_P2P5_Finalize_2026-09.md` (9 KB) — W7 P2-P5 收尾汇总
- `notes_GitInit_2026-09.md` (10 KB) — git init 完整记录 + 踩坑
- `notes_W7_ImageSourceCamera_2026-09.md` (10 KB) — 工业相机图像源（MVS SDK 条件编译）
- `notes_W7_UndoRedo_2026-09.md` (17 KB) — M6 撤销重做 + 节点 UUID + P0 悬垂指针修复

W7 收官期的改动（PCB/DL 算子、视频源、帧循环、每步结果查看、pin 检查、最近工程）
逐条记在 git 提交信息里，每条都写清了"为什么"和"测试抓到了什么"。

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
├── notes/                 # 变更日志（9 份）
├── datasets/              # 共享数据集（空,占位）
├── images/                # 共享图片（空,占位）
├── .gitignore             # Git 忽略规则
└── README.md              # 本文件
```

## 📊 统计

- 7 个 C++ 项目
- 68 个 C++ 文件 / ~8600 行代码
- 9 份变更日志 / 131 KB
- W7 单项 11 个无头回归测试 / 484 条断言
- 1.9 GB 项目体积（其中 .vs/build 占 95%，git 仓库仅 ~5 MB）
