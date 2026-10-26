# 运行期按名字找 DLL，是这台机器的事实

## 核心

> **构建期能找到的库，和运行期能找到的库，是两套机制。**
>
> 构建期用**绝对路径**（`find_package` 给的），
> 运行期用**按名字搜**（`PATH` / exe 同目录）。
>
> ⇒ **编译链接成功，不等于 exe 跑得起来。**

## 现象（2026-10-26，CI 第一次在真 runner 上跑）

W7 链接 `Qt6::Core / Gui / Widgets`。
七个项目里**没有一个拷 Qt 的 DLL** —— 只有 OpenCV 的拷了：

```cmake
# 每个项目都有这一条：
add_custom_command(TARGET w1_imageio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${OpenCV_BIN_DIR}          # ← OpenCV 的 DLL 被拷了
        $<TARGET_FILE_DIR:w1_imageio>
)

# Qt 的 DLL：没有这一步
```

于是 exe 找 `Qt6Core.dll` 只能走 Windows 的老规矩：**PATH**。

| | Qt 装在哪 | `bin` 在 PATH 上吗 |
|---|---|---|
| 本机 | `D:\Qt\6.8.3\msvc2022_64` | ✅ **在** |
| runner | `D:\a\_temp\Qt\6.8.3\msvc2022_64` | ❌ **不在** |

> **链接过了，exe 一启动就找不到 DLL。**
> 而在开发机上，它**跑得好好的**。

## 为什么本地彩排也救不了

彩排跑在**同一个 shell** 里，
而那个 shell 的 PATH 恰好带着 Qt（装 Qt 时写进机器 PATH 的）。

⇒ 彩排测的是「**这个 shell 能不能跑**」，
不是「**任何 shell 能不能跑**」。

> 这是**上一条**（彩排被跳过）的一个变种：
> 彩排的被测对象是**一段代码**；
> 这一次被测对象是**一段环境**，
> 而环境**默认继承自彩排自己**。

## 修法：PATH 交给**拿到路径的那份脚本**

`ci.ps1` 已经收了 `-QtRoot`（构建时就是用它找头文件和库的）。
所以**同一个值**顺手也用来找运行期的 DLL：

```powershell
$qtBin = Join-Path $QtRoot 'bin'
if (-not (Test-Path $qtBin)) { Fail (...) }        # 找不到 = 硬失败
if ($pathEntries -notcontains $qtBin) {
    $env:PATH = "$qtBin;" + ($pathEntries -join ';')
}
```

**一个来源，不是两个。**

### 顺带：tesseract 的 PATH 也在搬家

原来 workflow 的 YAML 里有一段：

```yaml
if ($env:TESSERACT_EXE) {
    $tess = Split-Path -Parent $env:TESSERACT_EXE
    $env:PATH = "$tess;$env:PATH"
}
```

那是**同一件事的第二份副本** ——
后果是**本地彩排根本没做这件事**。

现在搬进 `ci.ps1`（由 `TESSERACT_EXE` 驱动），YAML 里那份**删掉**。

> 副本的害处不一定是「值会漂」，
> 还可以是「**只有一边在做**」——
> 那一边的机器恰好有，于是**永远没人发现**。

## 推广：凡是运行期按名字找的东西

判据：

> **exe 在运行期要按名字找的东西**（`Qt6Core.dll`、`tesseract.exe`），
> 那个东西的目录要么
> **① 在 PATH 上**（由脚本从**同一个来源**加进去），
> 要么
> **② 被拷到 exe 旁边**。
>
> **「我机器上装了」不是这两者中的任何一个。**

一份清单（这个仓库的）：

| 东西 | 怎么找的 | 谁负责 |
|---|---|---|
| `opencv_*.dll` | ① 拷到 exe 旁 | 每个项目的 `add_custom_command` |
| `Qt6Core.dll` 等 | ① **PATH**（`ci.ps1` 加） | `tools/ci.ps1` |
| `tesseract.exe` | ① **PATH**（`ci.ps1` 加） | `tools/ci.ps1` |
| `recipes/*.json` | ① 拷到 exe 旁 | W6/W7 的 `add_custom_command` |
| `images/*.png` | ① 拷到 exe 旁 | W6/W7 的 `add_custom_command` |

> **清单里每一行都要有"谁负责"。**
> 没有主人的那一行，就是**下一次换机器会炸的那一行**。

## 教训

推论一：

> **「链接成功」和「能启动」之间隔着运行期。**
>
> 编译期用**路径**，运行期用**名字**。
> ⇒ **跑一次**才知��运行期的事，
> 而「跑一次」必须在**目标环境**里做。

推论二：

> **PATH 是环境的状态，不是代码的状态。**
>
> ⇒ 谁负责把某个目录放进 PATH，
> **必须是那个同时知道「这个目录在哪」的地方** ——
> 通常就是**拿到那个路径参数的那份脚本**。
>
> 否则就会长出第二份副本，
> 而副本的其中一边**往往没人跑**。

推论三：

> **「本地彩排通过」这句话，要问一句：**
> **在这个 shell 里通过，还是在任何 shell 里通过？**
>
> 环境类的东西（PATH、装了什么、注册了什么）
> **默认继承自彩排自己** ——
> 所以彩排天生看不见这一类问题。

推论四：

> **找不到就硬失败。**
>
> `if (-not (Test-Path $qtBin)) { Fail ... }` 那一行不是啰嗦 ——
> 它把「Qt 装歪了」这件事从
> **「exe 启动时弹一个看不懂的框」**
> 换成
> **「脚本说：Qt bin 不在这」**。

## 检查清单

- [ ] 这个 exe 运行期要找的东西，**清单列全了吗**？
- [ ] 每一行，**谁负责**（PATH 还是拷贝）？
- [ ] 负责的那份脚本，**和拿到路径的那份是同一份吗**？
- [ ] 缺一个的时候，**是硬失败还是静默跳过**？

配套：`at-an-empty-dir-is-not-in-git.md`（空目录不在 git 里）、
`at-a-rehearsal-can-pass-because-it-was-skipped.md`（被跳过的那段代码 = 还没写过的代码）、
`at-the-fallback-must-say-which-one-it-picked.md`（兜底必须说清选了哪个）、
`at-silent-fail-shape.md`（静默失败的形状）。

参见 `notes/notes_CI_FirstRunnerRun_2026-10.md`（这一笔的完整记录）。
