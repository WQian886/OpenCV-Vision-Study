# 键名长得像值 —— `d.value("dark_min_pct")` 里的字符串不是抄本

## 核心
2026-10-21，第五轮给旋钮审计的 3g 规则加"裸字符串"检查时踩的。

3g 那条规则本来只查一件事：
**面板读回控件时的兜底，不许是另一份默认值**。

它查的是数字旋钮的 `toInt(...)` / `toDouble(...)`。
而文本旋钮的 `toString(...)` 被解析器跳过了（注释写着"文本旋钮另说"）——
于是文本旋钮的面板兜底**一条规则都没管**：
`toString()` 退成空串没人管，`toString("recipes/demo.json")` 抄一份路径也没人管。

第五轮把 `toString()` 收了回来，顺手加了一条"兜底不许是裸字符串"。
第一条跑出来就红了 10 条：

```
[FAIL] pcb_defect_judge 的 setParams() 里 params.value("dark_min_pct")
       的兜底写的是裸字符串(d.value("dark_min_pct").toDouble())
```

## 这 10 条是误报

真实代码长这样：

```cpp
void PcbDefectJudgeForm::setParams(const QJsonObject& params) {
    const QJsonObject d = PcbDefectJudge().defaultParams();   // 声明值
    m_darkMinPct->setValue(params.value("dark_min_pct").toDouble(
                               d.value("dark_min_pct").toDouble()));   // ← 报红的这行
```

兜底的值是 `d.value("dark_min_pct")` —— `d` 装的就是**声明值**，
所以这一行是**完全正确**的写法：面板显示的和算子跑的是同一个数。

而 `"dark_min_pct"` 是**键的名字**，不是值。
它长得和"抄了一份路径"一模一样，但它不是。

## 为什么这条误报比漏报更值得写下来

1. **它会让人去改正确的代码。** 那 10 处的写法是对的，
   按报错的提示去"修"，只能改成 `d.value("dark_min_pct")` 换成别的写法 ——
   越修越绕，最后把对的改成错的。

2. **它会让守卫被关掉。** 本项目的一条硬规矩：
   > **一个对自家代码误报的守卫，下一个人只会把它关掉。**
   而守卫被关掉之后，连**原来能抓到的那一半**也没了 ——
   误报的代价不是"多修一次"，是"这条规则从此不存在"。

3. **它会让审计整体失去信任。** 一条规则误报十次，
   别人看到它变红的第一反应是"这条又发神经了"，
   而不是"我代码里有真问题"。真问题出现时，也一样被当成噪声跳过。

## 收法：判据要分清「这个名字」和「这个值」

不要问「这个表达式里有没有字符串」，要问「**这个字符串是名字还是值**」。

键名有固定的样子：**它紧跟在 `value(...)` 或 `[...]` 后面**。
所以先把键名抹掉，再看还剩不剩字符串：

```cpp
// 抹掉 d.value("dark_min_pct") —— 里面的字符串是**键的名字**,不是值
t.remove(QRegularExpression("\\.\\s*value\\s*\\(\\s*\"[^\"]*\"\\s*\\)"));
```

⚠ **只抹这一种形态。**
`.fromUtf8("recipes/demo.json")` 里的字符串**是值** ——
抹了就看不见"文本旋钮抄了一份路径"，那正是要抓的。
（抹 `.value(...)` 之外的任何 `xxx("...")` 都会漏掉真问题。）

## 教训

> **判据里出现的字符串，不一定是值 —— 先问它是什么。**
>
> 名字和值在源码里长得**一模一样**（都是双引号包起来的字面量），
> 区分它们的唯一办法是看**它在语法上的位置**，不是看它长什么样。
>
> 推论：**宁可漏报，也不要误报。**
> 漏报的代价是少抓一处；误报的代价是这条守卫被关掉，
> 而且是连同它本来能抓的那一半一起没。

参见 `notes/atomic/at-the-verdict-must-be-about-the-value-not-the-name.md`、
`notes/atomic/at-an-audit-that-cannot-see-starts-lying.md`。
