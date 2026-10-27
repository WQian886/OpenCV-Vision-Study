# 门禁的失败路径，只有在它失败的时候才被验证

## 一句话
> **一条从没跑过的失败路径，就是一条坏路径。**
>
> 而「跑一遍 CI」**永远不会**跑到失败路径上 —— 因为树是绿的。

## 现象（2026-10-27，给文档加第四道门禁那一轮）

新写的 `tools/check-sections.ps1`（CI 的 Step 3e）在真实的那棵树上**跑通了**，绿。

然后**故意**让树红一次（把欠账台账的「第十章」改成「第十一章」，
**只改一个标题**）—— 门禁本该报 **36 条红**。
它报出来的**不是红**，是一串：

```
Cannot convert value "System.Object[]" to type
"System.Management.Automation.SwitchParameter"
```

⇒ **门禁在它唯一该说话的那一秒，死了。**
⇒ 而且**没有人会发现**：那棵树本来是绿的 ⇒ **失败路径从来没被执行过**。

## 为什么它能一直坏着

| | 成功路径 | 失败路径 |
|---|---|---|
| 什么时候跑 | 每次 CI | **只有树红的时候** |
| 谁会跑它 | 所有人 | **只有故意制造红的人** |
| 从没跑过的后果 | —— | **没人知道它坏了** |

⇒ **CI 绿 ≠ 门禁是对的。** CI 绿只说明**成功路径**是对的。

## 第二个坑：测的地方，和出问题的不是同一个地方

第一版隔离测试是这么写的 —— **在函数里**：

```powershell
function Test-SameName() {
    param([switch]$List)
    $list = @(1, 2, 3)      # <- 在这里，它不报错
}
```

**它不报错。** ⇒ 「测的时候看起来是好的」⇒ **一个绿色的假象**。

原因：**在函数里，赋值会新建一个子作用域的变量**，根本碰不到那个参数。
而**在脚本作用域里**，那个参数是**带类型约束的变量**：

```powershell
param([switch]$List)
$list = @(1, 2, 3)
# Cannot convert value "System.Object[]" to type "System.Management.Automation.SwitchParameter"
```

**同一个赋值，两个地方结果相反。**

⇒ 这已经是本项目**第二次**栽在「测的地方和出问题的不是同一个地方」
（上一次是**旧二进制**）。
⇒ **判据必须落在出问题的那一处**（脚本作用域），不是「差不多的那一处」。

## 排查：报错信息指向了错的地方

报错指向 `Sort-Object`，**没有**指向真正的原因。排掉的三个假设：

1. 数据不对？—— 同一份数据在别处跑得好好的。
2. 表达式不对？—— 同一个表达式在别处跑得好好的。
3. 「这个脚本里第一次调用」？—— 排除。

**真正把它分开的判据，是把赋值目标改个名字**：`$warm` 行，`$list` 不行。

⇒ **当报错和原因隔着好几层，先找一个能把它们分开的判据**
（改个名字、换个数、换个作用域），而不是盯着报错本身看。

## 修法

1. **局部的名字不许和带类型的参数同名。**
   （参数改名 `-ShowSkips`，局部改名 `$chapList`）
2. **顺序是装饰，红才是重点。** 那个排序加了保护：排序坏了就退回原顺序 ——
   排序**不该有能力弄死这条消息**。
3. **故意让门禁红一次**，把它当**测试**的一部分，不是当**故障**。

## 教训

> **一个门禁的失败路径，只有在它失败的时候才被验证。**
>
> ⇒ 「CI 是绿的」**不能**当成「门禁是对的」的证据。
> ⇒ 每写一道门禁，都要**故意制造一次它该报的错**，看它**报得出来**。
> ⇒ 用**真的东西**去触发：这一轮改的是**真的**一个标题，
> 报出来的 **36 条**就是**真的**断了 36 个指针 ——
> **红的条数和坏的条数对得上**，才算触发对了。

配套：

- [at-a-lint-that-finds-nothing-looks-green.md](./at-a-lint-that-finds-nothing-looks-green.md)
  —— 扫到 0 个对象的 lint，长得和「通过」一模一样
- [at-zero-violations-is-not-proof-the-rule-works.md](./at-zero-violations-is-not-proof-the-rule-works.md)
  —— 「零违规」不能证明规则在干活
- [at-build-failure-looks-like-mutation-survived.md](./at-build-failure-looks-like-mutation-survived.md)
  —— 构建失败伪装成「变异全部存活」
- [at-cannot-be-tested-is-a-conclusion-not-a-reason.md](./at-cannot-be-tested-is-a-conclusion-not-a-reason.md)
  —— 「测不了」是一句结论，不是一个理由
- [at-powershell-names-ignore-case.md](./at-powershell-names-ignore-case.md)
  —— 变量名不区分大小写（这一次撞上的是**带类型的参数**）
- [at-stale-binary-fake-red.md](./at-stale-binary-fake-red.md)
  —— 旧二进制伪装成假红（同一个形状：错的**东西**长成了**对的**样子）

详见仓库 `notes/notes_SectionPointers_2026-10.md`。
