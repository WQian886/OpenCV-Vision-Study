#pragma once
// ============================================================
// 流水线存盘 / 读盘
//
// ============================================================
// 为什么从 mainwindow.cpp 里搬出来
// ============================================================
// 这两个函数以前是 mainwindow.cpp 里的 static 函数,无头测试编不动
// (mainwindow.h 拖着整个 QtWidgets 和全套算子)。
//
// 但它们**本来就设计成可测的** —— logFn 和 factoryFn 都是传进来的,
// 不依赖任何全局状态。搬出来只是让测试够得着。
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
// 画布支持 4 个端口(左/右/上/下),用户可以拉一条 Top→Bottom 的**竖线**。
// v1.0 只记了"谁连谁",加载时一律按默认的 右出→左进 重画 ——
// 于是**存一次再打开,竖线就躺下了**。
//
// 兼容性:老文件没有这两个字段,读到的是 Undefined,取默认 0,
// 映射回 Right/Left —— 和 v1.0 的行为完全一致。所以老工程照样打得开,
// 不需要任何迁移步骤。
// ============================================================

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

#include "operator_node.h"   // PortSide

namespace w7 {

class PipelineView;

// --- 工厂返回的东西 ---
//
// 这里**故意不返回 OperatorBase***。
//
// 存盘模块真正需要的只有两件事:这个算子名认不认识、它的显示名和分类是什么。
// 早先的签名是 `OperatorBase*(name)` —— 于是读一个文件要先 new 一个算子,
// 用完立刻 delete,只为了取两个字符串。而 OperatorBase 的头文件里
// `#include <opencv2/opencv.hpp>`,于是 pipeline_serialize.cpp 也跟着
// 依赖 OpenCV,w7_undo_test 就得链接 OpenCV,建一次测试目标慢一大截。
//
// 改成 POD 之后:存盘模块不认识算子(它只认名字和标签),
// 测试目标保持"零 OpenCV"这个属性,调用方那点构造逻辑收到 mainwindow 里。
struct OpDefaults {
    bool    known    = false;   // 认不认识这个算子名
    QString display;            // 认得的话,给什么显示名
    QString category;
};

// --- 存盘 ---
QJsonDocument pipelineToJson(PipelineView* canvas, const QString& imagePath);

// --- 读盘 ---
// factoryFn: 算子名 → 默认显示名/分类。返回 known=false 表示不认识,
//            该节点会被跳过并记一条日志(和以前 createOperatorByName
//            返回 nullptr 的行为完全一致)。
bool pipelineFromJson(PipelineView* canvas, const QJsonDocument& doc,
                      std::function<void(const QString&)> logFn,
                      std::function<OpDefaults(const QString&)> factoryFn,
                      QString& errOut);

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