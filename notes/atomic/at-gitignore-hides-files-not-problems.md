# .gitignore 藏起来的是文件，不是问题

## 现象

跑完测试，`git status` 干净，仓库看着很整洁。

但源码树里多了一堆本不该在那儿的东西：

```
W7_VisionMasterLite/images/w7_pcb_inspect.png
W7_VisionMasterLite/images/w7_video_f0000.png
W7_VisionMasterLite/images/_srcframe_00.png
W7_VisionMasterLite/images/_test_src.avi
...
```

之所以 `git status` 干净，是因为 `.gitignore` 里有这么几行：

```gitignore
**/images/w7_*.png
**/images/_test_src.avi
**/images/_srcframe_*.png
```

## 为什么这是治标

**文件还在源码树里。**

治标做到了「不碍眼」，没做到「不存在」。于是：

- 它们会被 `copy_directory` 一起拷进构建目录（所以构建产物里也有一份）
- 它们会被人看到，然后当成「这项目在往 images/ 里写东西」
- 它们会被手动删掉，删完测试再跑又出来
- 最要命的是：**看起来已经处理好了**

那条 ignore 规则本身在撒谎 —— 它说的是
「这里不会有文件」，实际是「这里有文件，但 git 假装看不见」。

## 判据

**想让 `git status` 干净，有两条路：**

| 做法 | 结果 | 性质 |
|---|---|---|
| 往 `.gitignore` 加规则 | 文件**还在** | 藏 |
| 让测试**不往那儿写** | 文件**不在** | 修 |

只有第二条是真的。

## 怎么验自己走的是哪条

把 ignore 规则**删掉**，再跑一遍测试，然后看 `git status`：

- 干净 → 说明测试真的不写了 ✅
- 又脏了 → 说明之前的"干净"是 ignore 给的 ❌

这一步是整个修复里最有价值的一步，
因为它把「规则在生效」和「问题被解决了」这两件事区分开了。

## 什么时候会踩到

- 任何「跑测试会在项目目录里生成文件」的情况
- 任何「构建脚本会在源码目录里生成文件」的情况
- 尤其：已经加了 ignore 规则、于是以为处理好了的时候

## 同一个机制的另一面：ignore 规则也会藏**源码**

`.gitignore` 里一条 `*.cmake`（本来只想挡 CMake 生成的中间文件），
在加 `common/cmake/ThirdParty.cmake` 时把**一个要提交的源码文件**藏了：

```
The following paths are ignored by one of your .gitignore files:
common/cmake/ThirdParty.cmake
hint: Use -f if you really want to add them.
```

**藏产物只是难看，藏源码是文件丢了。**
而且 `git add` 失败这件事很容易被忽略过去 —— 尤其在一次要加十几个文件的时候。

修法是在规则后面显式捞回来：

```gitignore
*.cmake
!common/cmake/*.cmake      # 这里放的是源码,不是生成物
```

（注意：`!` 规则必须写在 `*.cmake` **之后**；
而且如果**父目录**被整体排除了，`!` 是救不回来的 —— git 不会走进被排除的目录。）

**怎么验**：

```powershell
git check-ignore -v <路径>
```

看输出里命中的是哪一条。命中 `!` 开头那条 = 没被忽略 ✅。

## 检查清单

- [ ] 这批文件是**测试产物**吗？（是的话，放进构建目录，不是 ignore）
- [ ] ignore 规则是**为了让 bug 隐形**才加的吗？（是的话，删掉它）
- [ ] 删掉 ignore 规则、跑一遍测试，`git status` 还干净吗？
- [ ] **这条规则会不会误伤源码？**（`git check-ignore -v <要提交的文件>`）
- [ ] 这些产物会不会被 `copy_directory` 顺带拷进构建目录？
      （会 → 构建产物里有一份陈旧的、来源不明的数据）
- [ ] 有人手动从**别的目录**跑过测试吗？
      （工作目录不对，产物就落在别处 —— 这种残留 ignore 也藏不住）

> 删掉 ignore 规则的那一刻，规则的价值才露出来：
> 它不是「保持仓库整洁」，它是「让问题看不见」。
> 而看不见的问题，永远不会被修。
