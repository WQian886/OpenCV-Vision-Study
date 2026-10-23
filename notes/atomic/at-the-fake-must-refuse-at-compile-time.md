# 假件要在**编译期**就拒绝真件会拒绝的东西

## 一句话
> **假 SDK 要是"贴心地"补上了真 SDK 里没有的常量，就把「这行代码在真机上根本编不过」这个事实遮住了。**

## 现象

`image_source_camera` 里有一行：

```cpp
MV_CC_SetEnumValue(s_handle, "TriggerMode",
                   triggerMode == 1 ? MV_TRIGGER_MODE_SOFT : MV_TRIGGER_MODE_OFF);
```

`MV_TRIGGER_MODE_SOFT` —— 这个常量在**真 MVS SDK 里不存在**。
（全网搜这个标识符，零命中；所有资料用的都是
`MV_TRIGGER_MODE_OFF` / `MV_TRIGGER_MODE_ON` +
`MV_TRIGGER_SOURCE_SOFTWARE`。）

而这行代码**从来没有被编译过一次** —— 本机没装 MVS SDK，
CMake 走的是 `#else` 那半。于是：

> **装上真 SDK 的机器上，这个算子编不过。**
> 而这件事永远不会有人发现。

## 假件怎么把这事变成"当场红"

造一套假 MVS SDK（`third_party/fake_mvs/`）时，最省事的做法是
**把算子用到的常量全补齐** —— 补上 `MV_TRIGGER_MODE_SOFT`，
改哪都不会出事，测试还更好写。

**那就把 bug 遮住了。**

所以假件里**故意不提供**它：

```cpp
// 触发模式(GenICam TriggerMode: Off=0, On=1)
//
// ⚠ 这里**没有** MV_TRIGGER_MODE_SOFT —— 见文件头那段说明。
#define MV_TRIGGER_MODE_OFF 0u
#define MV_TRIGGER_MODE_ON  1u
```

于是算子一写回 `MV_TRIGGER_MODE_SOFT`，**编译就红**。

> **假的东西要在该严的地方严。**
> 严的方向是「让真件会拒绝的东西，假件也拒绝」——
> 而且要在**同一个阶段**拒绝：真件是编译期拒绝的，假件也该是编译期。

## 编译期守卫和断言是两种**不同**的验证

这是本轮新加的一条判定：**BUILD-FAIL**。

```
M115  MV_TRIGGER_MODE_ON 改回 MV_TRIGGER_MODE_SOFT   expected=BUILD-FAIL  got=BUILD-FAIL
```

为什么不能把它算成"KILL"：

| 判定 | 含义 |
|---|---|
| KILLED | 代码编过了，**跑起来**有断言红了 |
| SURVIVED | 代码编过了，跑起来**全绿** |
| **BUILD-FAIL** | 代码**压根没编过**，测试一次都没跑 |

BUILD-FAIL **不是**"变异存活"（那是"跑了但没红"），
也**不是**干净的 KILL（那是"跑了而且红了"）。

> 见 `notes/atomic/at-build-failure-looks-like-mutation-survived.md`
> —— 那一轮踩的是"构建失败被当成变异全部存活"。
> 这一次是它的反面：**构建失败本身是一条有价值的信息**，
> 但它得有自己的判定，不能混进"杀/活"里。

所以脚本里是这么写的：

```powershell
if (-not (Build-Suite $m.Suite)) {
    $verdict = 'BUILD-FAIL'
    $asExpected = ($m.Expect -eq 'BUILD-FAIL')
    ...
}
```

一个变异**只能**声明一种预期。声明错了就是脚本说错了话，
`unexpected results` 会把它揪出来。

## 教训

### 1. 假件抄真件的时候，"抄全了"是错的

假件不是真件的**完整替代**，它是真件的**严格子集**：
只实现被测代码用到的那些，而且**在真件会拒绝的地方也拒绝**。

> 补齐一个真件没有的常量，等于替真件说"这行代码没问题"——
> 而真件恰恰会拒绝它。

### 2. "文档"和"头文件"不是一回事

那段代码是照着**文档**写的，从来没和**真头文件**对过账。
文档不会告诉你一个常量叫不叫这个名。

（配套的修法：软触发是两件事 ——
`TriggerMode = MV_TRIGGER_MODE_ON` **加上**
`TriggerSource = MV_TRIGGER_SOURCE_SOFTWARE`。）

### 3. 编译期红，比运行期红更早、更便宜

这条 bug 的特点是：**它根本走不到运行期**。
如果没人在"编译期"就把它拦住，这个 bug 会一直躺在仓库里，
直到某个用户装了 SDK 才炸。

> 「没人编译过的那一半，不算存在过」——
> 见 `notes/atomic/at-a-branch-nobody-compiles-is-not-tested.md`。
> 这一条是它的**加强版**：不只是"没测过"，是"连能不能编过都不知道"。

## 检查清单

- [ ] 假件有没有**故意漏掉**真件会拒绝的东西？
    （不存在的常量、对不上的签名、严格的类型要求）
- [ ] 变异脚本能不能区分 **KILLED / SURVIVED / BUILD-FAIL / CRASHED**？
- [ ] 一个变异是不是只声明了一种预期？
- [ ] 「照文档写的 SDK 调用」有没有和**真头文件**对过账？

参见 `notes/atomic/at-a-branch-nobody-compiles-is-not-tested.md`、
`notes/atomic/at-build-failure-looks-like-mutation-survived.md`、
`notes/atomic/at-the-audit-and-the-product-must-read-the-same-files.md`。
