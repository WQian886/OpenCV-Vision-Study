# 窄串路径在 MSVC 上按 ANSI 解释：崩还算好的，静默跳过才是坏的

## 一句话
> **同一段 UTF-8 文本，路径窄串和文件内容窄串可以是两套编码 —— 而两边都"看起来能用"。**

## 现象

写测试时，进程直接死了（`0xC0000409`），不是断言失败，是整个测试进程崩掉。
崩溃点在**路径构造**，不在被测逻辑：

```cpp
const std::string kind = w6::boardKindOf(g_w6 / "images" / "根本没有这张图.png");
//                                                                          ^^^^^^^^ 崩在这
```

## 根因

MSVC 的 `std::filesystem` 有两处按 **ANSI 代码页**（本机 936 / GBK）解释窄串：

| 位置 | 行为 | 后果 |
|---|---|---|
| `path / "中文"`（窄串构造） | 按 ANSI 解释那段窄串 | 源文件是 UTF-8 → **直接抛异常** |
| `path::string()` | 宽路径转回 ANSI | 非 ANSI 字符 → **直接抛异常** |

探针实测（本机 ACP = 936）：

```
[1] path / "中文"        构造 THROWS: No mapping for the Unicode character exists in the target multi-byte code page.
[2] path(L"中文")        构造成功, .string() 长度 25
[3] path / "plain.png"   构造成功, .string() 长度 16
```

`u8path()` 才是"这段窄串是 UTF-8"的正确说法。

## 危险的那一半：绕开崩溃之后，它变成了静默跳过

用宽路径把崩溃绕开，事情**没有变好，只是换了个形状**：

```
imgPath.filename().string()   →  拿到 GBK 字节
manifest.csv 按原始字节读     →  UTF-8 字节
                              ↓
                        两边永远比不出来
                              ↓
                   "查不到板型" → 这块板被**静默跳过**
```

在报告里，**被跳过的板和合格的板长得一模一样**。

这就是本项目那条老原则的一个新变体：
> 崩溃给你一个栈；静默失效给你一个**看起来完全正常的结果**。

而且这次是**三层叠加**才漏掉的：崩溃 → 改用宽路径 → 不崩了 → 但比对还是错的。
每一步都"修好了上一层的症状"。

## 修法

### 1. 路径文本统一走 UTF-8

```cpp
std::string pathText(const fs::path& p) {
    try {
        return p.generic_u8string();   // UTF-8 + 正斜杠
    } catch (const std::exception&) {
        return p.generic_string();
    }
}
```

理由不只是"别崩"：**仓库里的 manifest.csv 是 UTF-8**，
文件名要和表里的内容**按同一套编码**比，否则比不出来 = 查不到 = 静默跳过。

### 2. 读图不用 `cv::imread`

`cv::imread` 内部按 ANSI 打开文件，中文路径读不出来，
而且**不报错** —— 只给你一个空 Mat，上层看到空图就报 "imread failed"，
看起来像文件坏了，其实是路径编码的事。

```cpp
std::ifstream f(p, std::ios::binary);            // 宽路径
std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
return cv::imdecode(cv::Mat(1, (int)buf.size(), CV_8UC1, buf.data()), cv::IMREAD_COLOR);
```

### 3. 构造路径时，窄串一律 `u8path()`

```cpp
fs::path u8p(const std::string& s) { return fs::u8path(s); }   // 这段窄串是 UTF-8
```

## 通用教训

### 1. "文件内容"和"文件名"必须在同一套编码下比较

一边 `ifstream` 读原始字节，一边 `path::string()` 取文件名 ——
这两个调用**来自不同的 API 家族，各自按各自的默认编码解释**。
用 `std::string` 比较它们的时候，**没有任何东西会提醒你编码不一致**。

> 凡是"从文件读出来的字符串"要和"从路径派生出来的字符串"比，
> 先确认两边的编码。

### 2. 绕开崩溃不等于修好

症状消失时，问一句：**是根因消失了，还是只是这一层不再报错了？**
这次修崩溃的办法（改用宽路径）让"路径构造"这层好了，
但"文件名比对"这层仍然是错的 —— 而它比崩更危险。

### 3. 非 ASCII 是普通输入，不是边界情况

用户会叫 `缺陷_错色.png`。项目作者是中文使用者，
板子目录也可能带中文。**这不是"极端情况"，是日常输入。**

## 检查清单

- [ ] 源文件是 UTF-8？窄串字面量进 `path` 之前包了 `u8path()`？
- [ ] 文件名和文件内容比较时，两边编码一致？
- [ ] 读文件/读图用宽路径或 `imdecode`，而不是 `ifstream(narrow)` / `cv::imread`？
- [ ] 报给用户的路径文本用 `generic_u8string()`，不是 `.string()`？
