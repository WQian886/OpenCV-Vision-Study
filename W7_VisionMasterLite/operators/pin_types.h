#pragma once
// ============================================================
// pin 类型系统 — 对标 VM 的 pin 类型检查
//
// 以前 W7 的端口是纯装饰:所有算子都是"一个左口一个右口",
// 任何两个节点都能连。连错不报错,要等到 Run 之后下游拿到
// 一堆对不上的字段才崩 —— 而那时用户已经把整条流水线搭完了。
//
// 真实的错法(都是新手一定会犯的):
//   · 数量判定 → 颜色分割   (前者出"结果/描述"文本,后者要图像)
//   · 文件夹图像 → 工业相机   (两个都是图像源,后者根本没有输入口)
//   · 节点 → 它自己
//
// 这里给算子声明"我这个口吃什么、吐什么",连线时先查一遍。
// 关键设计:检查只放在"用户主动连线"这条路上(doConnect),
// 不放在 addConnection 里 —— 加载旧工程、撤销重做都走
// addConnection,那些是恢复既有状态,不是新建决策,不该被拦。
// ============================================================

#include <QString>

namespace w7 {

// 端口数据类型。刻意只分三类 —— 分太细就该上类型系统了,
// 而这个项目的算子之间传的就是"图像 / 一包数据 / 一段文本"
// 三种东西,三类够用且不会误伤。
//
// 叫 Data 而不是 Numbers:blob_count 吐的是 count/total_area(纯数值),
// 但 pcb_inspect 吐的是 components(元件记录数组)+ count,
// dl_infer 吐的是 detections(检测框数组)。都归到"数据"才不撒谎。
enum class PinType {
    Any,      // 什么都收 —— 默认值,保证新算子不写声明也不会被拦
    Image,    // 图像(实际传的是 image_path 字符串)
    Data,     // 一包数据(count / position_x / score / components / detections ...)
    Text,     // 文本(result / description / defects / decoded ...)
};

// 中文名,给错误提示和画布标签用
QString pinTypeName(PinType t);

// === 连线合法性检查 ===
// 返回空串 = 可以连;否则是不允许的原因,直接弹给用户看。
// 单独拆成自由函数是为了能脱离 Qt 界面单测 —— 这段判断错了
// 不会崩,只是把该拦的连上/该放的拦住,属于静默失效。
//
// 没有 fromIsSource 参数:图像源输出的是图像,往任何要图的下游连
// 本来就合法,类型检查自己会覆盖,不需要额外规则。
//
// toIsSource:图像源没有输入口,天然不能当下游
QString checkPinCompatibility(PinType outType, PinType inType,
                              bool toIsSource,
                              const QString& fromDisplay,
                              const QString& toDisplay,
                              bool selfConnect);

// === 多前驱冲突检查 ===
//
// 背景:Run 引擎合并前驱输出时是"后写覆盖"
// (mainwindow.cpp 里 `inputs[it.key()] = it.value()`),
// 所以同一个下游接两条线时,**谁赢取决于 connections() 的遍历顺序**,
// 而遍历顺序和"用户先连的"无关 —— 挪一下节点的上下位置就可能翻转。
//
// 实测(2026-10-15 一次性探针,照抄引擎合并逻辑):
//   顺序 [A(红,10000px), B(蓝,5000px)] -> 下游拿到 blue/5000
//   顺序 [B(蓝,5000px), A(红,10000px)] -> 下游拿到 red/10000
// 两个上游状态点都是绿的、都不报错,只有下游那个数悄悄变了。
//
// 真实场合最常见的就是"抓红 + 抓蓝"两条 color_segment 都接 blob_count。
//
// 返回空串 = 可以连;否则是拒绝理由。
// 同样只在"用户主动连线"这条路上查(doConnect),
// 不拦加载旧工程 / 撤销重做 —— 那些是恢复既有状态。
//
// existingFromDisplay:该下游**已经**接到的那条线的来源算子名(空=还没有前驱)
QString checkMultiplePredecessors(const QString& toDisplay,
                                  const QString& existingFromDisplay,
                                  const QString& newFromDisplay);

} // namespace w7
