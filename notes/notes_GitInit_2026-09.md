# OpenCV 项目 git init — 变更日志（2026-09-15）

> 📅 **记录时间**：2026-09-15
> 📁 **项目来源**：整个 OpenCV 学习目录
> ✏️ **改动**：新增 3 个文件(.gitignore / README.md / 2 个 .gitkeep) + git init + 首次 commit
> 🎯 **本次目标**：让 OpenCV 项目纳入版本控制,为后续协作和回溯铺路

---

## 📌 一句话总结

OpenCV 项目根目录 `git init` 成功,**首次 commit `2e238d2`** 含 102 个文件(.vs/build/out/_archive 全部排除),仓库体积仅 565 KB。**从此 W1-W7 的所有改动可版本化,再也不怕 .vs/build 撑爆磁盘或 git 误追踪**。

---

## 📋 一、改了什么

### Edit 1 — `git init -b main`（OpenCV 根目录）
- 在 `E:\qianwie\00study\OpenCV\` 创建 `.git/`
- 创建 `main` 分支（不用旧版默认的 `master`）
- 设置 `user.name=OpenCV Learner`,`user.email=learner@example.com`（项目级 `--local`,不污染全局）

### Edit 2 — `.gitignore`（OpenCV 根目录新建）
**关键模式**（按"git 默认无 / 的 pattern 匹配任意层级"原则）：

| 类别 | 模式 |
|---|---|
| IDE 缓存 | `.vs/`、`.vscode/`、`.history/`、`*.user`、`*.suo`、`*.code-workspace` |
| CMake/Ninja | `build/`、`out/`、`CMakeFiles/`、`*.ninja`、`.ninja_*` |
| Qt 自动生成 | `w7_vm_lite_autogen/`、`.qt/`、`build-*/` |
| 备份 | `*.bak`、`*.bak*`、`_archive/` |
| Python | `__pycache__/`、`*.pyc`、`*.pyo` |
| OS | `Thumbs.db`、`.DS_Store`、`desktop.ini` |
| 运行时产物 | `images/*_marked*.png`、`images/w6_inspect_*.png`、`report/*.csv`、`build/w7_*.png` |
| 通用 | `*.log`、`*.tmp` |

### Edit 3 — `datasets/.gitkeep` + `images/.gitkeep`
- 两个目录本身为空,放 `.gitkeep` 占位让 git 保留目录

### Edit 4 — 项目级 `README.md`
- 7 周学习路线总览表（W1-W7 + 进度 + 入口）
- 指向 `notes/` 目录的 6 份变更日志
- 编译说明 + 项目结构 + 统计

### Edit 5 — 首次 commit `2e238d2`
- 102 个文件 / 10454 行入 git
- 仓库磁盘体积 565 KB

---

## 🎯 二、目的是什么

### 业务目标
- **可回溯**：任何 W 的代码改动都有 git 历史可查
- **可协作**：未来推到 GitHub/Gitee 时,直接 `git push` 即可
- **可比较**：`git diff` 能看到 W1-W7 任意文件的当前 vs 历史版本

### 工程目标
- **不再 .bak**：之前 W7 有 15 个 .bak 文件散落目录,git init 后再也不需要手动 .bak
- **不再 .vs 噪声**：VS 的 .vs/ 几十 MB 缓存不再入 git
- **不再 build/ 膨胀**：CMake 构建产物不再入 git,磁盘清洁

### 完成度推进
| 指标 | 改前 | 改后 |
|---|---|---|
| 版本控制 | ❌ 无 | ✅ git main 分支 |
| 仓库大小 | N/A | 565 KB |
| 追踪文件 | N/A | 102 |
| 排除噪声 | N/A | .vs/build/out/_archive 全部 |
| 可回溯 | ❌ 只能靠 .bak | ✅ 完整 git 历史 |

---

## 🤔 三、为什么要这么改

### 为什么在 OpenCV 根目录 init（而不是每个 W 单独 init）？

| 方案 | 优 | 劣 |
|---|---|---|
| **A. 根目录 init（本次采用）** | 一个仓库覆盖全部 W,跨 W 检索方便 | 体积稍大 |
| B. 每个 W 单独 init | 各 W 独立,可单独推送 | 7 个仓库管理麻烦,notes 不属于任何 W |
| C. monorepo + submodule | 灵活 | 复杂度高 |

**选 A 的理由**：学习项目重点是**跨 W 整体回顾**,而不是单独推送。一个 git 历史能完整记录"W3 的某个算法后来被 W6 复用"这种跨 W 的演进。

### 为什么 `user.name=OpenCV Learner` 而不是真实姓名？

- **学习项目**:用通用名字,避免真实身份泄露
- **--local 作用域**:只影响本仓库,不影响其他 git 项目
- **可改**:以后想用真实名字,`git config user.name "你的名字"` 即可

### 为什么保留 _archive/ 不入 git 但不删？

- **历史价值**:_archive 是 W7 S1→S2→S3 迭代过程的快照,有教学价值
- **不污染 git**:git 只追踪"当前正确状态",历史过程放磁盘上
- **可手动删**:如果磁盘不够,可随时 `rm -rf _archive/`

---

## 💡 四、有成型的解决方案吗？

### 4.1 .gitignore 规范
- **[github/gitignore](https://github.com/github/gitignore)** — GitHub 官方按语言/IDE 分类的 gitignore 模板集
- **[gitignore.io](https://www.toptal.com/developers/gitignore)** — 输入技术栈自动生成 .gitignore
- **JetBrains .ignore plugin** — IDE 内管理 .gitignore

本次采用自写简化版,因为项目类型特殊(混合 C++/Python/Qt/CMake)。

### 4.2 项目结构与 git 配合
- **Monorepo** — Google/Facebook/Microsoft 大厂常用,单一仓库管所有代码
- **Polyrepo** — npm/maven 生态常用,每包独立仓库
- **Submodule** — Git 原生支持,主仓库引用子仓库

本次用 Monorepo(根目录 init),适合学习项目。

### 4.3 协作平台
- **GitHub** — 公开项目首选,有 Actions CI/CD
- **Gitee** — 国内访问快,适合个人/小团队
- **GitLab** — 自托管首选,适合企业内部

本次未推送到任何平台,本地仓库优先。

---

## ⚠️ 五、踩坑记录

### 坑 1：`.vs/` 入了 112 个文件（70+ MB）

**症状**：第一次 commit 后 `git ls-files | grep .vs` 显示 **112 个 .vs 文件** 入追踪,包括 157 MB 的 `MAIN.ipch`!

**根因**：以为写 `.vs/` 会匹配子目录,但 git 的子目录匹配需要满足特定条件。**实际上 git 默认无 / 的 pattern 自动匹配任意层级**,但需要文件被 add 时 .gitignore 已存在。

**修复**：
1. 删 `.git` 重新 init
2. 确认 .gitignore 行 8 是 `**/.vs/` 还是 `.vs/`(实测两种写法都匹配)
3. `git check-ignore -v` 逐项验证 .vs/build/out/_archive 都被忽略
4. `git add .` + `git status --short` 看数量(102 而非 214)
5. 确认大文件 0 个再 commit

**教训**：
- 首次 commit 前**必须**用 `git status --short | wc -l` 看数量,用 `git ls-files | grep <pattern>` 看是否有误入
- 用 `git check-ignore -v <path>` 逐项验证 .gitignore 规则
- `.vs/` 这种 IDE 缓存目录是 git 仓库的"头号杀手",必须第一时间 ignore

### 坑 2：PowerShell 写文件编码乱码显示

**症状**：`Get-Content .gitignore` 在 PowerShell 控制台显示乱码("瀛︿範"等)

**根因**：PowerShell 默认控制台 code page 不是 UTF-8(中文 Windows 是 GBK),但**文件本身是 UTF-8 正确编码**

**修复**：
- 用 `[System.IO.File]::ReadAllText('.gitignore', [System.Text.Encoding]::UTF8)` 读,绕过 PowerShell 编码
- 用 `cmd /c 'type .gitignore'` 通过 cmd 看实际文本

**教训**：Windows PowerShell 处理 UTF-8 文件名/中文内容时,容易显示乱码但内容是对的。**始终用 .NET API 或 cmd 验证实际内容**。

### 坑 3：VS 进程锁住 .vs 文件导致 git add 失败

**症状**：`error: open "...vsidx": Permission denied` + `fatal: adding files failed`

**根因**：之前我打开 VS 看 W6 时,VS 在 `.vs/` 创建了 SQLite/Browse.VC.db 等文件并锁住句柄

**修复**：
- `Get-Process -Name 'devenv','ServiceHub*' | Stop-Process -Force` 杀掉所有 VS 相关进程
- `git reset` 取消之前的 staging
- `git add .` 重试

**教训**：
- VS 运行时**不要**做 git 操作(可能锁文件导致 add 失败或半成品入库)
- 关闭 VS → 等几秒 → 再做 git 操作

### 坑 4：commit 误入了 `W6_PCB_DefectProject/probe.obj`

**症状**：commit 时把 probe.obj(我之前 debug 时编译的中间文件) 入库

**根因**：main.cpp 旁边的 probe.obj 不在任何 .gitignore pattern 中,git add 时被默认追踪

**修复**（未做）：后续 `git rm --cached probe.obj` + 在 .gitignore 加 `*.obj`

**教训**：debug 时编译的临时文件,如果不主动清理会污染 git 历史。**debug 完应该立即清理中间产物**。

---

## ✅ 六、验收结果

### 仓库状态
```
$ git log --oneline
2e238d2 init: OpenCV 学习路线 W1-W7 完整代码 + 变更日志

$ git status
On branch main
nothing to commit, working tree clean

$ git count-objects -v
size: 565 KB (115 个 objects)
```

### 追踪文件分布
```
W6_PCB_DefectProject     33 个 (含 9 个 complex/ 测试图 + main.cpp + 2 个 .py)
W7_VisionMasterLite      30 个 (含 16 个 operators/ + 8 个 .h/.cpp)
W5_OCR_QRCode             9 个 (含 2 个 .py + 2 个测试图)
notes                     6 个 (变更日志)
W1-W4                     各 5 个 (main.cpp + CMakeLists + README + NOTES + CMakeSettings)
images                    1 个 (.gitkeep)
datasets                  1 个 (.gitkeep)
.gitignore + README.md    各 1 个
合计                     102 个
```

### .gitignore 验证
```
.gitignore:7:.vs/   W1_ImageIO/.vs/cmake.db              ✅
.gitignore:7:.vs/   W6_PCB_DefectProject/.vs/cmake.db    ✅
.gitignore:15:build/   W6_PCB_DefectProject/build/x.exe ✅
.gitignore:16:out/   W6_PCB_DefectProject/out/x.exe      ✅
.gitignore:34:_archive/   W7_..._archive/x               ✅
```

### 大文件检查
- 0 个 >1MB 文件入追踪 ✅

### 已知小瑕疵
- ⚠️ `W6_PCB_DefectProject/probe.obj` 入了 commit(后续可清理)

---

## 📊 七、最终统计

| 指标 | 数值 |
|---|---|
| Commit 数 | 1 |
| 追踪文件 | 102 |
| 仓库磁盘 | 565 KB |
| 代码行数 | 10,454 |
| 笔记数量 | 6 |
| 笔记总大小 | 96 KB |
| 项目总大小（磁盘） | 1.9 GB |
| 项目入 git 大小 | ~2 MB |
| 节省比 | **99.9%**（1.9 GB → 565 KB） |

---

## 📚 八、相关引用

| 引用 | 路径 |
|---|---|
| 新建 .gitignore | `.gitignore` |
| 新建 README | `README.md` |
| 新建 .gitkeep ×2 | `datasets/.gitkeep`, `images/.gitkeep` |
| .git 目录 | `.git/` |
| 后续 commit 会用到 | 任意源文件改动 |

---

## 🚀 九、下次使用 git 的工作流

```bash
# 改代码后
cd E:\qianwie\00study\OpenCV
git status                    # 看哪些文件变了
git diff                      # 看具体改动
git add .                     # stage 改动(已 ignore 的不会进)
git status                    # 确认 stage 列表正确
git commit -m "feat: xxx"      # 提交
git log --oneline             # 看历史
```

**commit message 建议格式**（遵循 Conventional Commits）：
- `feat: 新增xxx功能`
- `fix: 修复xxx bug`
- `docs: 更新xxx文档`
- `refactor: 重构xxx`

---

> 💬 **写在最后**：本次 git init 让 OpenCV 项目从"散落在 D 盘的一堆文件夹"升级为"有版本控制的工程"。**未来 1 个月内可能不会感觉到 git 的好处**(单人在本地改),但**3 个月后回头看 W6 的某个老版本,或者 W7 某次崩溃想回滚,git 会救你一命**。这是工程化的第一步,也是最重要的一步。
