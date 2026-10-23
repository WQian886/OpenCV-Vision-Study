# 传进去的回调会被拷一份 —— 状态得放在共享的地方

## 现象

给 W7 的执行引擎写测试。引擎的算子执行器是个回调：

```cpp
using EngineRunner = std::function<bool(const QString&, const QJsonObject&,
                                        const QJsonObject&, QJsonObject*, QString*)>;
```

测试里写了个假执行器，把「被调了几次」「收到了什么 inputs」记在**自己的成员**里：

```cpp
struct FakeRunner {
    QVector<QJsonObject> gotInputs;
    bool operator()(...) { gotInputs.append(inputs); ... }
};
```

断言是：

```cpp
check(r.gotInputs.size() == 3, "执行器被调了 3 次");
```

实测：**红**，实际 0 次。

## 真凶

`runFrame` 的形参是 `const EngineRunner&`（看起来是引用），但
**`std::function` 按值存**：

```cpp
EngineRunner runner = ...;   // ← 这里发生拷贝
```

传进去的 functor 被拷贝了一份放进 `std::function` 内部。
测试的假执行器把记录写进**副本**的成员里 —— 外面那个 `r` 永远是空的。

## 为什么这个坑特别安静

- **不崩**
- **不报错**
- 只是「记录不见了」

第一反应会是「引擎没跑」「引擎少调了几次」——
于是会去查引擎，而引擎是对的。

本项目的老教训「抄副本」在这里换了个马甲：
**逻辑没抄，但状态被拷了。**

## 修法

记录用的状态放 `std::shared_ptr` 里，functor 拷几份都指向同一块：

```cpp
struct Recorder {
    QVector<QJsonObject> gotInputs;
    QString failOn;
};
struct FakeRunner {
    std::shared_ptr<Recorder> rec;     // ← 共享,不是拷贝
    bool operator()(...) { rec->gotInputs.append(inputs); ... }
};

auto rec = std::make_shared<Recorder>();
FakeRunner r{rec};
// ...
check(rec->gotInputs.size() == 3, "执行器被调了 3 次");
```

## 一个容易看错的地方

**捕获引用的 lambda 没这个问题**：

```cpp
auto hook = [&](EnginePhase, const EngineStepInfo& info) { ++counter; };  // 安全
```

`[&]` 捕获的是引用，拷几份都指向同一个 `counter`。

**有成员状态的 functor 才有问题** ——
而这两种在同一个文件里混着用时，最容易看错：
「刚才那条 lambda 没事，凭什么这条要改？」

区别不在「是不是回调」，在**状态是怎么传过去的**：

| 状态放哪 | 会被拷贝吗 |
|---|---|
| lambda `[&]` 捕获的引用 | 否 |
| functor 的成员变量 | **是** |
| `shared_ptr` 指的东西 | 否 |

## 检查清单

- [ ] 传给回调的状态，是**按值**传的吗？—— 按值就是拷贝
- [ ] 断言**读到**的地方，和执行器**写入**的地方，是同一块内存吗？
- [ ] 断言莫名其妙地红 / 计数是 0：先怀疑「是不是记到副本上了」
- [ ] 别把「逻辑没抄」当成「没抄」—— 状态也会被拷

参见 `notes/atomic/at-extract-it-or-it-is-just-a-second-copy.md`
（「抽成一份」和「复制一份去测」不是一回事）——
那一条讲抄**逻辑**，这一条讲抄**状态**，是同一类病的两张皮。
