# 状态和画面对不上，是最难查的那类 bug

## 核心

2026-10-25，W7 把 `mainwindow.cpp` 拉进测试目标时遇到的。

主窗口的参数面板是一个 `QStackedWidget`：第 0 页是占位页（"选中一个节点查看参数"），
后面每一页是某个节点的参数表单。

「算子没装」的节点现在会**留在画布上**（上一笔的改动），
所以用户**点得到它**。这时候面板要切回占位页，并且说清是哪个算子没装。

那两条分支（"没选中节点" / "算子没装"）当时是这么写的：

```cpp
// mainwindow.cpp（2026-10-25 第六笔之前）
m_currentParamWidget = nullptr;      // ← 只把指针置空
m_currentParamNode   = nullptr;
m_paramPanel->setCurrentIndex(0);    // ← 切回占位页
```

看着已经"处理好了"：面板上确实显示占位页了。

## 但**内部状态**和**画面对不上**

那个表单 widget **还留在堆栈里**。

```
面板上显示:  占位页（"算子 X 未安装"）
堆栈里躺着:  上一个节点的表单（没人指着它）
主窗口记着:  m_currentParamWidget → 那个表单
```

三个东西对不上。两个后果：

| 后果 | 症状 |
|---|---|
| **孤儿表单** | 每选一次"算子没装"的节点，堆栈里就多一个死表单 —— 面板**越点越胖** |
| **状态和画面对不上** | 面板上明明是占位页，主窗口却还记着"当前显示的是那个表单" |

第二个尤其要命：**界面看着完全正常**。

> 这就是「错了用户会不会自己发现」里最隐蔽的那一类：
> **画面对，但**内部**对不上**。
>
> 用户看得见的东西全是对的（面板是占位页），看不见的东西全错了。
> 而错的那部分**会影响下一次操作**——
> 比如「点『应用参数』时把值写给哪个节点」。

## 修法：拆掉，不是藏起来

```cpp
void MainWindow::dropParamForm() {
    QPointer<QWidget> w = m_currentParamWidget;
    m_currentParamWidget = nullptr;      // 先清指针
    m_currentParamNode   = nullptr;
    if (!w) return;
    w->hide();
    if (m_paramPanel) m_paramPanel->removeWidget(w);   // 再从堆栈里拿掉
    w->deleteLater();
}
```

**三条分支都走它**：
「没选中节点」/「算子没装」/「换了一个节点」。

> 「不显示」和「不存在」是两件事。
> 藏起来（只切页）= 东西还在，只是看不见。
> 拆掉（removeWidget + deleteLater）= 它真的不在了。
>
> 而**指针**要清 —— 因为指针表达的是"**当前显示的是哪个**"，
> 那是**状态**，不是"曾经显示过什么"。

## 教训

> **「界面上不显示」不等于「内部不存在」。**
>
> 一个只做到前半句的修复，会留下：
>   · 一个没人管的孤儿（泄漏、越积越多）
>   · 一份和画面对不上的状态（下一个操作会用错它）
>
> 而这两个后果**都不报错、不崩溃、界面看着正常**。

### 怎么让它可测

光靠"盯着代码看"抓不到 —— 那个分支长得太合理了。

这一轮靠的是**一个可观测面**：

```cpp
struct MainWindowWiring {
    int  paramPanelPages = 0;      // 堆栈里一共几页  ← 孤儿看得见了
    bool paramShowsForm = false;  // 当前页是**真表单**吗
    ...
};
```

- `paramPanelPages` 让**孤儿**看得见（切到占位页后应该只剩 1 页）
- `paramShowsForm` 让**状态和画面对不上**看得见

> 快照的价值不在"能读到值"，在**它读的是不是同一个真相**。
> 一份只读"当前页下标"的快照会说谎 ——
> 因为占位页**也是**一个 widget，下标根本区分不了"表单"和"占位页"。

## 配套

- `notes/atomic/at-programmatic-calls-are-not-user-actions.md` —— 程序化调 API 不等于用户操作
- `notes/atomic/at-the-dropped-node-hides-the-broken-chain.md` —— 丢一个节点会把整条链藏起来
- `notes/atomic/at-a-limit-you-keep-needs-a-boundary.md` —— 有意留下的限制要写清边界

参见 `notes/notes_W7_MainWindow_2026-10.md`、
`notes/notes_W7_Knob_Audit_2026-10.md` 第十节（欠账 17）。
