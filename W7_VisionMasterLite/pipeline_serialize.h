#pragma once
// ============================================================
// 流水线存盘 / 读盘
// ============================================================
//
// 为什么从 mainwindow.cpp 里搬出来
// ============================================================
//
// 这两个函数以前是 mainwindow.cpp 里的 static 函数,无头测试编不动
// (mainwindow.h 拖着整个 QtWidgets 和全套算子)。
//
// 但它们**本来就设计成可测的** —— logFn 和 factoryFn 都是传进来的,
// 不依赖任何全局状态。搬出来只是让测试够得着。
//
// ============================================================
// ⚠ 读盘分成了两层(2026-10-24)
// ============================================================
//
// 「读盘」原来是一个函数:解析 JSON **并且**往画布上画。
// 于是想"只解析不画"的人(headless runner)没辙 ——
// 要么造一个离屏画布,要么自己再写一份解析(抄副本)。
//
// 现在分成两层:
//
//   pipeline_graph.h   JSON -> 图        纯 QtCore,零 OpenCV 零 QtWidgets
//   pipeline_serialize  图 -> 画布       QtWidgets
//
// 两边共用**同一份**解析规则(见 pipeline_graph.h 开头那段)。
//
// ⚠ 这一层(QtWidgets)的"为什么从 mainwindow 搬出来"那段话仍然成立:
//   它要 PipelineView,所以无头测试要拖 QtWidgets。
//   但**规则**已经不在这一层了 —— 规则在纯的那一层。
//
// ============================================================
// 格式版本
// ============================================================
// v1.0  节点(op/display/category/x/y/params/uuid) + 边(from/to)
// v1.1  边多了 from_side / to_side —— 见下面"为什么要记方向"
//
// ============================================================
// 为什么要记端口方向
// ============================================================
//
// 画布支持 4 个端口(左/右/上/下),用户可以拉一条 Top→Bottom 的**竖线**。
// v1.0 只记了"谁连谁",加载时一律按默认的 右出→左进 重画 ——
// 于是**存一次再打开,竖线就躺下了**。
//
// 兼容性:老文件没有这两个字段,读到的是 Undefined,取默认 0,
// 映射回 Right/Left —— 和 v1.0 的行为完全一致。所以老工程照样打得开,
// 不需要任何迁移步骤。
//
// ============================================================

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

#include "operator_node.h"   // PortSide
#include "pipeline_graph.h"  // OpDefaults / PipelineGraph / graphFromJson

namespace w7 {

class PipelineView;

// --- 存盘 ---
QJsonDocument pipelineToJson(PipelineView* canvas, const QString& imagePath);

// --- 读盘 ---
// factoryFn: 算子名 → 默认显示名/分类。返回 known=false 表示不认识,
//            该节点会被跳过并记一条日志(和以前 createOperatorByName
//            返回 nullptr 的行为完全一致)。
//
// outGraph: 可选。给了就把解析出来的那张图也带回去
//           (调用方要 image_path 之类的东西时用,省得自己再读一遍根对象 ——
//            **字段名只该有一个地方知道**)。
bool pipelineFromJson(PipelineView* canvas, const QJsonDocument& doc,
                      std::function<void(const QString&)> logFn,
                      std::function<OpDefaults(const QString&)> factoryFn,
                      QString& errOut,
                      PipelineGraph* outGraph = nullptr);

// --- 端口方向 ↔ JSON ---
// 存成 0..3 的整数;读的时候任何越界/缺失都退回 fallback。
// 注意 toInt() 对 Undefined 返回 0(= Left),而 Left 在"读回来"这件事上
// 是个合法值 —— 所以不能靠 toInt() 是否为 0 来判断字段有没有,
// 必须先 isDouble()。这是本函数存在的原因。
inline OperatorNode::PortSide parseSide(const QJsonValue& v,
                                        OperatorNode::PortSide fallback) {
    if (!v.isDouble()) return fallback;
    const int i = v.toInt();
    switch (i) {
        case 0: return OperatorNode::PortSide::Left;
        case 1: return OperatorNode::PortSide::Right;
        case 2: return OperatorNode::PortSide::Top;
        case 3: return OperatorNode::PortSide::Bottom;
        default: return fallback;   // 手改坏的文件不该让整条线消失
    }
}

} // namespace w7
