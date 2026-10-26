# 断言量的是库，测的不是你

## 核心

2026-10-26，给 W2 补测试时**变异验证**抓到的。

做法是**故意把实现改坏**，看测试会不会转红。
三个变异：

| # | 改了什么 | 结果 |
|---|---|---|
| 1 | W1 判据 `px[0]` → `px[2]`（BGR 当成 RGB） | 8 条断言转红 ✓ |
| 2 | W2 金色窗口上限 30 → 60 | 2 条断言转红 ✓ |
| 3 | W2 `bilateralDenoise` 内部换成高斯 | **0 条转红** ✗ |

第 3 条**不该**是 0。

## 那条断言长这样

```cpp
cv::Mat sg, sb;
cv::GaussianBlur(step, sg, cv::Size(9, 9), 0);     // <- 直接调 OpenCV
cv::bilateralFilter(step, sb, 9, 75, 75);           // <- 直接调 OpenCV
...
check(bandB <= 1, "双边保住阶跃边缘");
```

它量的是 **`cv::bilateralFilter` 的输出**。

而**被测的东西**是 **`w2::bilateralDenoise`** ——
那是**我们的函数**，它内部调 `cv::bilateralFilter`。

把 `w2::bilateralDenoise` 的**整个实现**换成
`cv::GaussianBlur(src, out, Size(d,d), 0)` ——
**这条断言还是绿的。**

> 它证明的是「**OpenCV 的双边滤波保边缘**」，
> 它要证明的是「**我们的双边滤波保边缘**」。
>
> 这两件事**长得一模一样**，但**根本不是一件事**。

## 为什么这不是小事

这类断言有一个很舒服的性质：**它永远绿**。

不管我们的实现坏成什么样，只要 OpenCV 本身没变，它就绿。
它是**装饰**，但它**看起来像**一道关卡 ——
而且它测的偏偏是**这个项目里最核心的那条性质**
（高斯糊边缘、双边不糊 —— 这是 W2 整条链的卖点）。

⇒ **一个永远绿的断言，比没有断言更坏**：
没有断言，你知道那里没测；
永远绿的断言，你会以为那里测过了。

## 怎么改

**测试调 `w2::` 的函数，不调 `cv::` 的：**

```cpp
const cv::Mat sg = w2::gaussianBlur(step, 9);
const cv::Mat sb = w2::bilateralDenoise(step, 9, 75, 75);
```

改完再跑同一个变异 → **1 条断言转红** ✓。

## 什么时候会犯

**测试里「为了造一个干净的对象」而调库函数的时候。**

比如造一张阶跃边缘图、造一张椒盐噪声图 ——
造图用 `cv::Mat` + `setTo` 是**对的**（那是**输入**），
但**拿来被测量的那个量，必须来自被测函数**。

区分方法很简单：

> **这个量是「输入」还是「输出」？**
>
> 造输入 → 用库函数，没问题。
> 量输出 → **必须走被测的那一层**。

## 教训

> **变异验证不只是「证明测试有效」，
> 它还是「找出测试测错了对象」的唯一手段。**
>
> 因为「测试没反应」这个现象，
> 只有靠**故意改坏**才看得见 ——
> 你没法靠「读一遍测试」看出它量错了对象：
> 读起来完全通顺。

⇒ 推论：**没做过变异验证的测试套件，
只能算「跑过了」，不能算「验过了」。**

## 配套

- [at-zero-violations-is-not-proof-the-rule-works.md](./at-zero-violations-is-not-proof-the-rule-works.md)
  —— 零违规不等于规则有效
- [at-the-test-data-must-make-the-behavior-happen.md](./at-the-test-data-must-make-the-behavior-happen.md)
- [at-the-third-copy-was-on-the-panel.md](./at-the-third-copy-was-on-the-panel.md)
  —— 抄副本的另一种形态

参见 `notes/notes_W1W2_Tests_2026-10.md`（这一笔的完整记录）。
