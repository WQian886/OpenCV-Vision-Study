# 检测器不能被它要检测的东西杀死

## 核心

> **一个只在「正常情况」下被验过的检测器，等于没有检测器** ——
> 而「不正常」恰恰是它唯一该开口说话的那一种情况。
>
> 检测器在**它要报的那种情况**下死掉，等于：
> **它只学会了不报。**

## 现象（2026-10-26，推上 GitHub 之前）

`tools/ci-deps.ps1` 里要检查两个 python 模块在不在
（`aqtinstall` 和 `py7zr` —— 新 runner 上都没有，脚本得自己装）。

第一版这么写：

```powershell
function Test-PythonModule([string]$module) {
    $null = & python -c "import $module" 2>$null
    return ($LASTEXITCODE -eq 0)
}
```

看着没问题。**第一次跑就炸了** —— 而且炸在**最不该炸的地方**：

```
模块不在  →  python 打印 traceback 到 stderr
          →  2>$null 把 stderr **捕获**了
          →  PS 5.1 把捕获到的 stderr 包成 ErrorRecord
          →  脚本全局 $ErrorActionPreference = 'Stop'
          →  终止性错误，脚本当场结束
```

## 根因：检测器把它要检测的东西给杀了

「模块不在」这个**唯一该被报告的情况**，
恰恰是让这个检查函数**自己死掉**的情况。

```
检查模块在不在  ──▶  模块不在  ──▶  检查函数把脚本带走了
     ▲                                              │
     └──────────────  它从来没机会说「不在」  ◀───────┘
```

> 换句话说：这不是「检查写得不够细」，
> 是**检查在最关键的那一刻变成了终止错误**。
>
> 而这种错**极难发现**，因为：
> **在模块都在的机器上，它永远是对的。**

## 修法：调用期间把「捕获到的 stderr 就致命」这件事关掉

```powershell
function Test-PythonModule([string]$module) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'      # ← 关键
    try {
        $null = & python -c "import $module" 2>$null
        return ($LASTEXITCODE -eq 0)
    } finally {
        $ErrorActionPreference = $prev       # ← 复原
    }
}
```

要点：

- 只在**这一次调用**期间降级，别把整个脚本的严格性放松掉；
- 用 `try/finally` 复原 —— 漏复原的话，后面所有的错都会变成不终止的，
  那是**另一种**更贵的错（脚本带着错误继续跑）；
- 判据还是 `$LASTEXITCODE`，不靠解析 stderr。

## 怎么验它（这是本卡最值钱的一段）

**别在正常情况下验它。** 办法：写个 harness，**抽取脚本里那段函数文本**
（不抄一份 —— 抄一份就测不到真代码），然后**把模块卸掉**再跑。

2026-10-26 实测的三条分支：

| 分支 | 期望 | 实测 |
|---|---|---|
| 模块不在 | 认得出，**不崩** | ✅ `Test-PythonModule 'py7zr' = False` |
| 模块不在 | 装上 → 验活（pip 名 ≠ 模块名也算） | ✅ 装了 aqtinstall 3.3.0 / py7zr 1.1.4，均验活 |
| pip 报成功但模块仍不在 | **报错**，不是放行 | ✅ `pip install six reported success but 'module_that_does_not_exist_xyz' is still not importable` |

第三条是**故意**加的：
> 「装好了」这句话**自己会撒谎**，
> 所以装完必须**再问一次**（真的 import 一次），
> 问不出来就**报错**，不能放行。

## 教训

推论一：

> **检测器必须在「它要报的那种情况」下被验过。**
>
> 验它的办法不是「再读一遍代码」，
> 是**让它真的面对那种情况**——
> 把模块卸掉、把输入删掉、把文件拿走。

推论二：

> **「没报错」和「跑过了」不是一回事。**
>
> 一个检查函数如果会因为「被检查的东西不在」而终止，
> 那么它**只被验过正常路径**，
> 而正常路径恰好是它**不需要说话**的那条路。

推论三：

> **严格性（`$ErrorActionPreference='Stop'`）是全局的，
> 而「这次调用该不该严格」是局部的。**
>
> ⇒ 该局部的地方就局部，别用全局的严格去覆盖一次
> **故意要失败**的探测。

推论四（跟这一笔的另一半有关）：

> **写完检测器就把它放进一个「失败路径」里跑一遍** ——
> 这是最便宜的一类测试，因为它**不需要任何测试数据**，
> 只要**把环境还原成坏的那一半**。

## 检查清单

- [ ] 这个检查函数在「被检查的东西**不在**」时，会返回 False，还是会把程序带走？
- [ ] 它有没有在**失败路径**上被跑过至少一次？
- [ ] 「装好了 / 配好了 / 找到了」这种话，**有没有再问一次**？
- [ ] 局部放宽的严格性，**复原了吗**？

配套：`at-a-lint-that-finds-nothing-looks-green.md`（查不出东西的 lint 看着是绿的）、
`at-no-runner-is-not-a-reason.md`（没跑过不算理由）、
`at-the-guard-can-be-fed-by-a-comment.md`（守卫能被一句注释喂饱）、
`at-cannot-be-tested-is-a-conclusion-not-a-reason.md`（测不了是结论不是理由）。

参见 `notes/notes_CI_2026-10.md`（CI 这一条的完整记录）。
