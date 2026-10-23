# 临时对象上的 `.get()`，在下一行之前就悬空了

## 一句话
> **`f().get()` 里的 `f()` 返回临时对象 —— 语句一结束，它就死了，`.get()` 拿到的指针悬空。**

## 现象

`dl_infer` 调 ONNX Runtime 拿输入输出的名字：

```cpp
const char* inNames[]  = {session.GetInputNameAllocated(0, sc.env->GetAllocatorInfo()).get()};
const char* outNames[] = {session.GetOutputNameAllocated(0, sc.env->GetAllocatorInfo()).get()};
```

看起来没问题 —— 拿到名字、存进数组、传给 `Run()`。

**但是：**

`GetInputNameAllocated()` 返回的 `Ort::AllocatedStringPtr` 是个**临时对象**。
C++ 的规则：临时对象在**整条语句结束**时被销毁。

所以这条语句结束时：

1. `AllocatedStringPtr` 的析构函数跑，把那块内存**释放掉**
2. `inNames[0]` 变成一个**指向已释放内存的指针**
3. 下一行 `session.Run(..., inNames, ...)` 读的是**已释放的内存**

这就是 use-after-free。

## 为什么它特别安静

- **不崩**（大概率）
- **不报错**
- 读到的可能还是原来的字符串 —— 取决于分配器有没有把这块内存给别人

真 SDK 上最坏的情况是：
`GetOutputNameAllocated` 的分配**复用**了刚被释放的那块内存
→ `inNames[0]` 悄悄变成了**输出**的名字
→ `Run()` 报"找不到这个输入"，或者更糟。

> 一句话：**不崩不等于没事。**
> 「偶尔对偶尔错」是最难查的一类 —— 它会让人怀疑自己的模型。

## 修法

让那个拥有内存的对象**活得久一点**：

```cpp
auto inNameAlloc  = session.GetInputNameAllocated(0, sc.env->GetAllocatorInfo());
auto outNameAlloc = session.GetOutputNameAllocated(0, sc.env->GetAllocatorInfo());
const char* inNames[]  = { inNameAlloc.get() };
const char* outNames[] = { outNameAlloc.get() };
```

`auto` 出来的局部变量活到函数结束 → 指针期间一直有效。

> 代价是零（两个小对象栈上放着），收益是"本来就不该是 UAF"。

## 假 SDK 怎么把它变成**确定**的错

真 SDK 释放之后那块内存是"未定义"的 —— 可能还是原字符串，可能是一堆垃圾。
所以**在真 SDK 上，这个 bug 不一定每次都现形**。

假 SDK 故意做得更严：

```cpp
~AllocatedStringPtr() {
    if (m_buf) {
        std::memset(m_buf, 0, std::strlen(m_buf) + 1);   // ← 清零
        delete[] m_buf;
    }
}
```

于是"用了已释放的内存"变成**确定的**空串，
`Run()` 一比对名字就报：

```
fake-ort: 输入名不对(期望 "images", 实际 "")—— 已释放的内存被清零了,这里读到的是空串
```

测试立刻红。变异 M86 把修复改回去 → **红 19 条**。

> ⚠ 这个依赖要**记下来**：
> 假件把 UAF 变成确定错误，靠的是「释放即清零」这条**比真 SDK 严格**的规则。
> 如果哪天把清零去掉，M86 就会**活**（变成"偶尔错"）。
>
> 假的东西要在**该严的地方**严 —— 严的方向是"把不确定的错变成确定的错"。

## 教训

### 1. 所有权跟着**类型**走，不跟着**调用**走

`AllocatedStringPtr` 这个名字就说明它**管着一块内存**。
`f()` 返回一个拥有资源的临时对象时，那个资源的命周期**只到语句末尾**。

规则：**要留指针，就把拥有者留成变量。**

### 2. `auto` 一下通常就对了

```cpp
const char* p = f().get();     // 悬空
auto owner = f();               // 活着
const char* p = owner.get();   // 有效
```

### 3. 不崩不是证据

「跑了几百次都没崩」对 UAF 没有任何说服力。
本项目那条老原则在这里适用：**静默失效比崩溃难查**。

### 4. 假件要能把"未定义"变成"确定"

真件上「偶尔对偶尔错」的 bug，假件应该让它**确定地错**。
这样测试才能稳定地咬住它。

## 检查清单

- [ ] 有没有 `f().get()` / `f().data()` 这种**临时对象上的取指针**？
- [ ] 返回值是不是一个**拥有资源**的类型（智能指针、带 alloca 的句柄）？
- [ ] 那个指针**活到**最后一次使用了吗？
- [ ] 假件在"释放后读取"这件事上，是**确定地错**还是"未定义"？

参见 `notes/atomic/at-a-callback-copies-what-you-give-it.md`
（同一类问题的另一个马甲：资源被拷走）、
`notes/atomic/at-silent-fail-shape.md`。
