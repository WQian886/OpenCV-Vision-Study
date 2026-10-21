# 提示语不是默认值：空着的输入框会把声明的默认值吃掉

## 核心
2026-10-21，补旋钮审计第二轮时发现的。

`image_source_dir` 的参数面板长这样：

```cpp
m_dirPath = new QLineEdit;                               // 空
m_dirPath->setPlaceholderText("如 images 或 D:/test_imgs");
```

看起来"默认值 `images` 写在那儿呢"。
其实那是个 **placeholder** —— 框为空时才显示的灰色提示，
**不是控件的初值**。

于是这条链子是断的：

```
defaultParams() 声明   dir_path = "images"
        ↓
面板 buildUI()         m_dirPath 是空的          ← 初值没给
        ↓
collect()              p["dir_path"] = ""        ← 把空串写回参数
        ↓
run()                  params.value("dir_path").toString("images")
                       → key 在,值是空串 → 返回空串,不是 "images"
        ↓
                       报错"目录不存在: "
```

**声明的默认值被面板吃掉了。**

同一个毛病还有 `image_source_video.video_path`
和 `image_source_camera.camera_ip`。

## 为什么它不吵
它不是静默失效 —— 它**会报错**。

但报的是"目录不存在: "，不是"你没配路径"。
人会往**路径写错了**的方向去查，
而真实原因是"这个 key 压根没有默认值可用"。

> 本项目的规矩是「静默失效优先于崩溃排查」。
> 这条不属于静默失效，但属于**报错信息把人引向错误方向** ——
> 一样费时间。

## 教训
> **placeholder 是给人看的，控件的初值是给机器用的。**
> 两者都"写在那一行代码里"，但只有一个算数。

判断一个输入框有没有初值，要问的是
"**这个控件现在的值是什么**"，
不是"这行代码里有没有出现过那个默认路径"。

修法：初值直接取常量
（`new QLineEdit(QString::fromUtf8(ImageSourceDirDefaults::kDirPath))`），
提示语只留"例子"那一半 ——
提示语里那个 `images` 本身也是一份抄本。

推论：**"框是空的"也是一种默认值**，
只不过是一份**没人登记过、也没人看**的默认值。

参见 `notes/atomic/at-the-third-copy-was-on-the-panel.md`。
