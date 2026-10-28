# 门禁被指错了树，结论还是绿的

## 一句话
> **门禁说「我查不了 46 个」，然后说「OK」。**
>
> ⇒ **说出来 ≠ 结论跟着变。**
> 一个门禁的**结论**必须跟着它的**盲目**走。

## 现象（2026-10-27，给文档加第四道门禁那一轮）

新写的章节指针门（`tools/check-sections.ps1`）在真实的那棵树上跑通了，绿。

后来有人（我自己）手滑，把 `-RepoRoot` 传成了 `.` 而不是绝对路径。
再跑一次，门禁**还是绿的**：

```
166 pointers seen: 123 resolve (54 cross-file, 69 same-file), 0 do not
named files not in this tree: 117 distinct -- ../notes/notes_CI_2026-10.md,
  ../notes/notes_SectionPointers_2026-10.md, notes/atomic/at-... (80 more)
pointers that could NOT be checked (their target file is not in this tree): 46
SECTIONS OK: 123 of 166 section pointers resolve across 129 files
```

⇒ 它**把每一件不对劲的事都说了出来**，然后**判了个 OK**。

更狠的是：它连**自己量到了什么**都不知道 ——
`notes/atomic/` 下的卡**一张都没找到**（**0 张**），
而真实的树上有 **83 张**（那一轮测量时的数）。

> 一门被指错了树的门禁，量的不是「这棵树上的指针」，
> 是**空气**，然后把空气报成了一份干净的报告。

## 根因：一句「前缀假设」

三道门都靠同一句代码算「这个文件相对于仓库在哪」：

```powershell
$rel = $f.FullName.Substring($RepoRoot.Length)
```

**这只在 `$RepoRoot` 是绝对路径的前缀时成立。**
传 `.` 的时候不成立：

| | 实际的值 |
|---|---|
| `$RepoRoot` | `.`（长度 1） |
| `$f.FullName` | `E:\qianwie\00study\OpenCV\notes\x.md` |
| `$rel` 算出来 | `:\qianwie\00study\OpenCV\notes\x.md` |

⇒ 这是一个**谁都不存在**的路径 ⇒ **每一次查找都不中**。

## 三道门，一个**根**，三种下场

| 门 | 树看错之后 | 结论 |
|---|---|---|
| `check-sections.ps1` | 路径全错，**46 个指针查不了** | **绿的（谎）** |
| `check-doc-numbers.ps1` | **0 张 atomic 卡**、115 张「笔记」 | **红的**（它和现实对照，自己对不上就露馅） |
| `check-links.ps1` | 只影响**打印出来的路径**（链接是按所在文件解析的） | 绿的（**这次是碰巧对的**） |

⇒ **和现实对照的门会自曝；只往前看的门不会。**
「查完了吗」不是自曝，**「查到的数对得上吗」**才是。

## 修法：先认树，再量

三处，一起：

1. **根先解析成绝对路径**，再拿去做前缀运算
   （`Resolve-RepoRoot`，在 `tools/lib/DocScan.ps1` 里，三道门共用一份）。
2. **先认树，再量**：`README.md`、`notes`、`notes\atomic`、`tools\ci.ps1`
   四样缺一样，就**大声拒绝**（`Get-RepoTreeProblems`）——
   **被指错了树，要报错，不要报告。**
3. **查不了的指针 = 失败**，不是「跳过了」——
   和链接门对齐（链接指向不存在的文件，本来就是红的）。

第 3 条顺带修掉一个**不一致**：
链接门对「相对链接指向不存在的文件」是**红**的，
指针门对「指针指向不存在的文件」却只是**数一数**。
⇒ 同一件事，两道门两个态度。
⇒ 现在统一了：**指向不存在的地方的主张，就是坏了的主张。**

> 而**提到**一个不存在的文件仍然是**只数不红** ——
> 提到可能是**建议**（「可以加一个 CHANGELOG.md」）、
> 也可能是**别处的文件**（`Windows2022-Readme.md` 在另一个仓库）。
> **提到**不是主张，**指针**才是。

## 验证：三条路，都量过

| 跑法 | 结果 |
|---|---|
| `-RepoRoot <绝对路径>` | 131 of 179 解析成功，48 个跳过，**0 个查不了** |
| `-RepoRoot .` | **和上面一模一样**（这是修好之前的对照） |
| `-RepoRoot <别的目录>` | **红的**：`is not the OpenCV study tree`，并列出缺了什么 |

⇒ 修好之前，第 2 行和第 1 行**不一样**，而第 3 行是**绿的**。

## 教训

> **一个门禁的结论，必须跟着它的盲目走。**
>
> ⇒ 「查不了 N 个」如果只出现在**日志**里、而**结论**还是绿的，
> 那这 N 个就是**永久的盲区**：每一次 CI 都在说「我查不了」，
> 每一次 CI 都在说「OK」。
> ⇒ **要么把它查了，要么让它红。**
>
> ⇒ 而更靠前的一步是：**别让它看错树**。
> 一个量空气的门禁，**报出来的每个数都是真的** ——
> 只是那些数说的是**另一棵树**。

## 复发第二次（2026-10-27）：这次是**副本**不是树

同一张卡的第二次。这次不是「根传错了」，是
**门禁被指向的那个东西，不是我以为的那个东西**。

普查门禁的失败路径脚本（`tools/mutate-census.ps1`）要跑在**树的副本**上
（真树一个字都不许碰）。第一次上真 runner 就红：

```
UNEXPECTED: mutation D should be GREEN but came out RED
```

读日志，50 条红长这样：

```
FAIL: C,D\notes\atomic\at-a-fix-record-re-creates-the-defect.md:13:
  pointer '第 3.5 节' is bound to 'notes/notes_SectionPointers_2026-10.md',
  which is not in this tree -- so it cannot be checked
```

⇒ **每个文件前面都挂了一层 `C,D\`** ⇒ 副本**不是那棵树** ⇒
门禁量的是**另一棵树**：同文件的指针还能查（69 个全在），
跨文件的全查不了（62 个只剩 12 个）。

> 同一张卡，同一个形状，只是往下一层：
> 上一次是**根**指错了，这一次是**副本**不是树。

### 修法：副本要先**验**，才配**信**

副本用之前先过两道：

1. **文件集一样**：源树和副本的相对路径集合必须**逐个相等**
   ——少一个、多一个、位置不一样，都算不是那棵树；
2. **量出来一样**：在副本上跑一次检查器，它的汇总行必须和真树上**一字不差**。

对不上就**大声拒绝**：`THE COPY IS NOT THE TREE`，
把两边各差哪几个路径**列出来**，然后退出。

⇒ **「门禁说 OK」这句话，得先证明它看的那棵树是对的。**

### 怎么证明这道新门不是摆设

把复制逻辑**故意改坏**，改成 runner 上那个形状（每个文件都塞进一层 `C,D\`），
再跑一次：它**拒绝给结论**，并把「少了的 142 个」和「多出来的 133 个」路径列了出来。

⇒ **新门自己被抓过一次**（不是「它跑过了」，是「它在该红的时候红了」）。

## 复发第三次（2026-10-28）：同一个假设，**第二次**被绕过去

这张卡说「根先解析成绝对路径」。**不够。**
绝对路径**还有两种写法**，而前缀假设只在其中一种下成立。

失败路径脚本**第二次**上真 runner，它喊了：

```
THE COPY IS NOT THE TREE: the copy does not hold the same files
   missing from the copy (142): notes\atomic\at-a-blind-reading-cannot-watch.md, ...
   extra in the copy (142): ine\notes\atomic\at-a-blind-reading-cannot-watch.md, ...
```

⇒ 一边干净，一边每个都多三个字符 `ine\` ——
而 `ine` 是 **`pristine` 的尾巴**（副本目录就叫这个名）。

### 同一个目录，两个字符串

runner 上 `$env:TEMP` 是 **`C:\Users\RUNNER~1\AppData\Local\Temp`**（8.3 短名）。
PowerShell 的 provider 枚举文件时给的是**展开后的**
`C:\Users\runneradmin\AppData\Local\Temp`。

⇒ **`RUNNER~1` 8 个字符，`runneradmin` 11 个字符** ——
**同一个目录，两个字符串，差三个字符。**

而前缀假设是**拿长度切**的（`Substring($Root.Length)`）：
切的长度来自**短名**，切出来的路径来自**长名** ⇒ 每一条相对路径从头就错三个字符。

### 本机量得出来（本机的原话）

```
root string : ...\t8probe\AVERYL~1              (len 47)
Get-Item    : ...\t8probe\AVeryLongDirectoryNameToForceShortNameHere  (len 81)
child       : ...\t8probe\AVeryLongDirectoryNameToForceShortNameHere\f.txt
Substring   : [gDirectoryNameToForceShortNameHere\f.txt]      <-- 垃圾
```

⇒ **`Resolve-Path` 原样返回短名（47），`Get-Item` 才给展开后的（81）。**
⇒ 而上一张卡的修法用的正是 `Resolve-Path`。
⇒ **同一个假设，换了个地方绕过去。**

### 修法要更硬一格

1. 根**问 provider 要**（`Get-Item ... .FullName`），不是自己拼一个绝对路径；
2. 相对路径**不切字符串**：基址取 provider 自己的答案，
   文件不在基址底下就**喊出来**，不切。

⇒ 这不是「多修一个边角」：
**四个检查器共用的那一处**（`Resolve-RepoRoot`）**也是同样的切法**，
只是 runner 的检出路径没有 8.3 成分，所以它**没发作** ——
它发作在**临时目录**上，因为那才是从 `$env:TEMP` 拼出来的。

> ⇒ 「解析成绝对路径」**不等于**「解析成 provider 那个样子的绝对路径」。
> ⇒ **同一层里还有下一层**：修掉一层不等于修掉这一层。

配套：

- [at-an-audit-that-cannot-see-starts-lying.md](./at-an-audit-that-cannot-see-starts-lying.md)
  —— 审计认不出新写法时会开始撒谎（同一族：**说得出 ≠ 判得准**）
- [at-a-lint-that-finds-nothing-looks-green.md](./at-a-lint-that-finds-nothing-looks-green.md)
  —— 扫到 0 个对象的 lint，长得和「通过」一模一样
- [at-a-gate-failure-path-runs-only-when-it-fails.md](./at-a-gate-failure-path-runs-only-when-it-fails.md)
  —— 门禁的失败路径，只有在它失败的时候才被验证
- [at-the-counter-must-not-undercount-silently.md](./at-the-counter-must-not-undercount-silently.md)
  —— 计数器不许悄悄少数

详见仓库 `notes/notes_SectionPointers_2026-10.md`、
`tools/lib/DocScan.ps1`。
