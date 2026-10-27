# PowerShell 的变量名不区分大小写

## 核心

2026-10-26，改 `tools/check-doc-numbers.ps1` 时踩的。

```powershell
$Measured = @{ 'W7_VisionMasterLite' = @{...}; 'W6_PCB_DefectProject' = @{...} }
...
foreach ($pt in $SuiteProjects) {
    $measured = $Measured[$proj]      # <- 和 $Measured 是**同一个变量**
    ...
}
```

**`$measured` 和 `$Measured` 是同一个变量。**

于是：

1. 第一次循环（W7）：`$Measured`（外层表）被**整个覆盖**成 W7 的内层表。
2. 第二次循环（W6）：`$Measured['W6_PCB_DefectProject']`
   → 在 **W7 的套件表**里找 `'W6_PCB_DefectProject'` → **`$null`**。
3. `$null.Count` → 崩。

## 报错完全不指向原因

```
The property 'Count' cannot be found on this object.
Verify that the property exists.
```

**它不会说「你的变量名撞了」**，
只会说「这个对象没有 Count」——
而你盯着的是 `$measured` 那个变量，觉得它明明是 hashtable。

**报错现场和出错原因隔了三层。**

## 为什么难发现

因为在**大多数语言**里 `measured` 和 `Measured` 是两个变量。
C++ / C# / Java / JS 都是大小写敏感。
**只有 PowerShell（和 VB）不敏感。**

从 C++ 过来写 PowerShell，**这个习惯会被带过来**，
而语法上**完全合法** —— 编译器/解释器不给你任何提示。

## 改法

**别让两个变量的名字只差大小写。**

```powershell
$projMeasured = $Measured[$proj]     # 一眼能看出是"某个项目的"
```

外层的「所有项目」和内层的「某一个项目」，
**名字要能一眼分开**，不是靠大小写。

## 教训

> **换语言写脚本时，先确认这门语言的名字规则。**
>
> 大小写不敏感意味着：
> `x` 和 `X` 不是两个变量，是**一个变量的两种写法**。
>
> ⇒ 在这种语言里，**变量名的大小写是语义的一部分**，
> 不能随手换。

## 顺带

同一次改动里还踩了 **StrictMode** 的一个表现：

`Set-StrictMode -Version Latest` 下，访问 `$null` 的不存在属性
报的是 `PropertyNotFoundStrict`，
**而不是**「$null 上没有这个属性」——
**错误消息在说对象，不在说变量**。

⇒ 严格模式是好东西，但它把「这个变量是空的」
和「这个属性不存在」**报成了同一种错**。

## 配套

- [at-ninja-mtime-stale-binary.md](./at-ninja-mtime-stale-binary.md)
  —— 同一轮里的另一个「现象不指向原因」的坑：
  文件内容是对的，跑的却是旧二进制

参见 `notes/notes_W1W2_Tests_2026-10.md`（这一笔的完整记录）。
