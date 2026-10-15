# 恢复文件后忘了碰 mtime，ninja 不重建 → 你测的是旧二进制

## 现象
做变异测试：改源码 → 构建 → 跑测试。
脚本会先"恢复原文件"，再"打上变异"。

某轮结果：**9 个变异里有 4 个活下来了**。
但那 4 个的"活下来"数值，和上一轮测的**完全一样**。

## 根因
`Copy-Item` 保留原文件的 `LastWriteTime`。

于是恢复后的文件，mtime **比新构建产物还旧** →
ninja 判定"这个 obj 已是最新" → **不重新编译**。

下一步"打上变异"如果也是 `Copy-Item` 干的，同样保留 mtime →
ninja 又说"no work to do"。

> **你测的是一个根本没被修改的版本，而结果看起来完全正常。**

## 代价
整整一轮变异验证作废 ——
4 个"存活"里至少 3 个是假的，实际代码早就是坏的。

更糟的是它**不产生任何可疑信号**：不报错、不警告，
就是给你一个看起来正常的数字。

## 修法
恢复/写入之后**显式更新时间戳**：

```powershell
function Restore-All {
    foreach ($f in $orig.Keys) {
        [IO.File]::WriteAllText($p, $orig[$f], $utf8)
        (Get-Item $p).LastWriteTime = Get-Date      # ← 关键
    }
}
```

用 `[IO.File]::WriteAllText` 而不是 `Copy-Item`，
可以顺手把编码也定死（见下一条）。

## 通用教训
### 1. 构建系统的"没活干"要当真
ninja/msbuild 说 "no work to do" 时，
先确认**输入文件的 mtime 确实比产物新**，再相信它。

### 2. 自动化脚本里，任何"写文件"都要审计时间戳
写完文件 → 显式 `LastWriteTime = Get-Date`。
这在临时脚本、测试夹具、代码生成里都一样。

### 3. 连续多轮实验尤其危险
单轮跑完看不出问题；**第二轮拿到第一轮的旧产物**就开始错。
每轮之间是唯一的安全边界。

## 附带：Windows 上的文本写入三连坑
同一次事故里还踩到（都在 PowerShell 5.1）：

| 坑 | 规避 |
|---|---|
| `Copy-Item` 保留 mtime | 显式设 `LastWriteTime` |
| `Set-Content` 默认写系统 ANSI 码页，中文变乱码 | 用 `[IO.File]::WriteAllText($p, $s, $utf8)` |
| 写完有 U+FFFD 替换字符但**不报错** | 提交前扫一遍（见下） |

### 扫替换字符（PowerShell 5.1 有效）
```powershell
$t = [IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)
$n = 0
foreach ($c in $t.ToCharArray()) { if ([int]$c -eq 0xFFFD) { $n++ } }
```
编码损坏会**静默进 git 历史**，事后极难定位 —— 扫一遍成本极低。

## 检查清单
批量改文件跑实验时：
- [ ] 写文件后显式更新 mtime？
- [ ] 构建输出里确认真的有 compile 行，不是只有 "no work to do"？
- [ ] 每轮之间不复用上轮的产物？
