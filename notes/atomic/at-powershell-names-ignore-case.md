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

## 复发（2026-10-27）：这次撞上的是**带类型的参数**

同一个坑，**第二次**。这次不是两个普通变量撞名，
是**局部变量撞上了一个带类型的参数**：

```powershell
param([switch]$List)
...
$list = @($set)     # <- 和 $List 同名
```

PowerShell 里**带类型的参数是带类型约束的变量**，
所以 `$list = @(...)` 不是「新建一个数组」，
是「把数组**塞进那个 switch 参数**」—— 数组转不成 switch：

```
Cannot convert value "System.Object[]" to type
"System.Management.Automation.SwitchParameter"
```

**报错指向 `Sort-Object`，没有指向真正的原因。**

和第一次的差别，有一条**更要命**：

| | 第一次（`$measured` / `$Measured`） | 这一次（`$list` / `[switch]$List`） |
|---|---|---|
| 撞上的是什么 | 另一个**普通变量** | **一个带类型约束的参数** |
| 后果 | 数据被覆盖，**结果是错的** | **直接抛**，脚本当场死 |
| 什么时候发现 | 跑起来就看见了 | **只在失败路径上** —— 树是绿的就永远不发现 |

⇒ 后者比前者**更难发现**：前者的错在**成功路径**上也会出现；
后者**只在它该报错的时候才出现**，而那个时刻**没人在场**。

⇒ 所以「别让两个变量的名字只差大小写」还要再加一条：
**局部的名字不许和带类型的参数同名。**

⇒ 而「失败路径上的错，没人在场时发现不了」是**另一条**教训，
在 `at-a-gate-failure-path-runs-only-when-it-fails.md`。

## 配套

- [at-ninja-mtime-stale-binary.md](./at-ninja-mtime-stale-binary.md)
  —— 同一轮里的另一个「现象不指向原因」的坑：
  文件内容是对的，跑的却是旧二进制
- [at-a-gate-failure-path-runs-only-when-it-fails.md](./at-a-gate-failure-path-runs-only-when-it-fails.md)
  —— 这次的完整教训：门禁的失败路径，只有在它失败的时候才被验证

参见 `notes/notes_W1W2_Tests_2026-10.md`（这一笔的完整记录）。
