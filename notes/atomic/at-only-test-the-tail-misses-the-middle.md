# 只测末端，会漏掉传递链上的中段

## 问题
连线要还原方向（从哪个口拉出、接到哪个口），值必须走过这条链才到得了命令：

```
EdgeEndpoint → RemoveEdgeCmd → exec → undo
```

缺陷有**两处**：

```cpp
struct EdgeEndpoint { ... };              // ← 第一处：根本没带 side

RemoveEdgeCmd::RemoveEdgeCmd(..., const QUuid& fromId, const QUuid& toId)
    : m_canvas(canvas), m_fromId(fromId), m_toId(toId) {}   // ← 第二处：没存 side

void RemoveEdgeCmd::undo() {
    m_canvas->addConnection(from, to);    // 方向全走默认值 Right→Left
}
```

用户从 A 的**下边**拉出、接到 B 的**上边**画的竖线，
单独选中按 Delete、Ctrl+Z 撤销回来：

```
fromSide: Bottom(3) -> Right(1)
toSide:   Top(2)    -> Left(0)
```

**变成横线了。** 线还在、节点还在、连线关系还在，只有走向变了——
所以不报错、不打日志。

## 第一版用例为什么是绿的
先写的用例 7b 是：

```cpp
new RemoveEdgeCmd(&cv, a->uuid(), b->uuid(), Bottom, Top)
//                                     ^^^^^^ 直接把值塞进去了
```

**它把整段传递链跳过了。** 用例测的是 `RemoveEdgeCmd::undo()`，
而第一处缺陷（`EdgeEndpoint` 不带 side）在它的上游，**根本到不了**。

补了 7c，走真实路径：

```
框选线 → planSelectDelete → EdgeEndpoint → RemoveEdgeCmd → exec → undo
```

## 用变异矩阵证明两条用例不是抄两遍
| 变异 | 7b（直接构造） | 7c（走真实路径） |
|---|---|---|
| M1 `RemoveEdgeCmd::undo` 不还原 side | **红 2** | **红 2** |
| M2 `EdgeEndpoint` 不带 side | 绿 | **红 4** |

读法：
- **M1 两边都杀** —— 末端坏了，整条链都坏。
- **M2 只杀 7c** —— 中段断了，直接构造命令的 7b 完全无感。

**如果 M2 也杀 7b，就说明 7b 其实没测到它声称测的东西。**
矩阵长这样才说明两条用例各管一段；两列数字一样，多半是抄了一遍。

## 检查清单
- [ ] 这条测试是**直接构造**被测对象，还是走**真实调用路径**？后者才盖得住上游
- [ ] 值要经过几跳？每一跳都可能丢字段
- [ ] 想证明两条用例**不重复**：跑变异矩阵，看两列的红的**集合是否不同**
- [ ] 只杀得动末端的用例，等于没盖中段
