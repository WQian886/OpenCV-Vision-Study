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
