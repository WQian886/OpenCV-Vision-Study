# 没人检查的表会漂

## 核心

2026-10-26，给 W1 / W2 补测试时，**顺手把检查器改了**，
当场逮到一张**漂了几个月的表**。

`tools/check-doc-numbers.ps1` 原来**只查 W7 的套件表**。
这一轮改成**每个项目都查自己的表**。

一改就红了：

| | `pcb_core` | `qr_ocr` | `w6_cli` | 合计 |
|---|---|---|---|---|
| W6 README 的表（错的） | 180 | 36 | 163 | **379** |
| 实测（对的） | **279** | **36** | **167** | **482** |

## 最扎人的地方

**同一份 README 上面那段正文写着「3 个 ctest 套件、482 条断言」。**

```
第 122 行（正文）：  W6 有 3 个 ctest 套件、482 条断言
第 133 行（表格）：  180 / 36 / 163  =  379
```

**两份数字就摆在同一个文件里，隔了十行，谁也没对一眼。**

这比「文档和现实对不上」更难看 ——
这是**文档和自己对不上**。

## 为什么能漂这么久

因为**没人检查那张表**。

检查器只查 W7 的表（当时 W7 是唯一有逐套件表的成熟项目），
W6 有表，但**没人看**。

> **一个没人检查的表，就是一个会漂的表。**
>
> 不是「可能会漂」，是**一定会** ——
> 只要有人改了一次断言数而忘了改表，它就漂了。
>
> 而且漂了**没人知道**，因为**没有第二双眼睛**。

## 改法

**把「谁有套件表」写成一张显式的清单**，
每个项目查**自己那份**：

```powershell
$SuiteProjects = @(
    @{ Project = $W7Proj; Readme = 'W7_VisionMasterLite/README.md' }
    @{ Project = $W6Proj; Readme = 'W6_PCB_DefectProject/README.md' }
    @{ Project = $W2Proj; Readme = 'W2_ColorFilter/README.md' }
    @{ Project = $W1Proj; Readme = 'W1_ImageIO/README.md' }
)
```

**表放在谁的 README 里，就由谁自己负责对得上。**
不集中到一个地方 —— 集中了就得**同步**，
而同步就是「抄副本」的另一种形态。

## 顺带的一条

检查器里那个循环还踩了 **PowerShell 变量名不区分大小写**的坑
（`$measured` 和 `$Measured` 是同一个变量），
见 [at-powershell-names-ignore-case.md](./at-powershell-names-ignore-case.md)。

## 教训

> **写下来的东西，只有在被检查时才算数。**
>
> 一张表、一段注释、一个数字 ——
> **没有人会看它，它就只是文件里的字**。

推论：

> **每加一个「被记录的事实」，就得同时加一个「检查它」的门。**
>
> 否则它就是**下一个会漂的东西**，
> 而漂了没人知道。

## 配套

- [at-the-total-can-hide-a-wrong-part.md](./at-the-total-can-hide-a-wrong-part.md)
  —— 总数对得上，分部可以对不上
- [at-a-branch-nobody-compiles-is-not-tested.md](./at-a-branch-nobody-compiles-is-not-tested.md)
  —— 同一个形状的另一个对象：**没人编译过的那一半，不算存在过**
- [at-a-link-does-not-know-its-target-is-gone.md](./at-a-link-does-not-know-its-target-is-gone.md)
  —— 同一个原则的另一个对象：**没人跟着走的链接，会指向一个已经没了的文件**
- [at-unverified-number-in-comment.md](./at-unverified-number-in-comment.md)
- [at-four-copies-of-one-number.md](./at-four-copies-of-one-number.md)

参见 `notes/notes_W1W2_Tests_2026-10.md`（这一笔的完整记录）。
