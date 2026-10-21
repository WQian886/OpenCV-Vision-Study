# 第三份抄本在面板上：审计只查一个地方，等于没审计

## 核心
2026-10-21，补「旋钮审计」的第二轮。

上一轮审计查的是 `defaultParams()` ——
"这个旋钮声明的默认值，是不是手抄的字面量"。

但同一个默认值在源码里有**三个位置**：

| 位置 | 写的是什么 | 它决定的是 |
|---|---|---|
| `defaultParams()` | 写进流水线 JSON 的那份 | 参数 JSON 里的值 |
| `run()` 兜底 | `params.value("x").toInt(EXPR)` 里的 `EXPR` | key 缺了时算子按什么跑 |
| 面板 `buildUI()` | `setValue(EXPR)` 里的 `EXPR` | 界面上写着的那个数 |

上一轮只查了第一个。
第二、三个当时**没人看** ——
它们抄了副本，审计不会红，测试也照样全绿。

## 实测后果
第二轮把审计扩到三处之后，当场抓出 6 处：

| 算子 | 面板上的初值 | 声明的默认值 | 病 |
|---|---|---|---|
| `pcb_inspect` 金层区间 | `setValue(85/80/115/0.8)` | `pcbcolor::goldBand()` | 数字抄本 |
| `dl_infer.class_names` | placeholder `"person,bicycle,car"` | COCO 六个类名 | 抄本，**而且只抄了前三个** |
| `image_source_camera.camera_ip` | placeholder `"如 192.168.1.10"` | `"192.168.1.10"` | 抄本，而且框是空的 |
| `image_source_dir.dir_path` | placeholder `"如 images 或 D:/test_imgs"` | `"images"` | 抄本，而且框是空的 |
| `image_source_video.video_path` | placeholder `"如 images/sample.avi"` | `"images/sample.avi"` | 抄本，而且框是空的 |
| `pcb_inspect.mode` | `addItem(..., "known_site")` | `PcbInspectDefaults::kMode` | 抄本 |

金层那处最值得说：它和 `pcbcolor::goldBand()` 的数值**完全一致**，
所以从任何一次运行的结果上，**看不出任何毛病**。

但它就是抄本。校准值一改，面板就悄悄对不上了，
而界面上不会有任何提示。

## 教训
> **一个只查了一处的审计，等于没审计 ——
> 它只保证了你记得去查的那一处。**

推论：定规则要问的不是"我想查什么"，
而是"**这个值一共出现在几个地方**"。

先数位置，再定规则。
本轮数出来是三个位置，那就得查三个。

数的时候别漏掉**字符串**：
`known_site` / `"*.png"` / `"192.168.1.10"` 这些也是默认值，
只是不像 `0.8` 那样一眼看出是"一个阈值"。

参见 `notes/atomic/at-a-placeholder-is-not-a-default.md`。
