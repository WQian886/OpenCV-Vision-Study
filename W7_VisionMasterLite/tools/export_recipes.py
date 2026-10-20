#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 W6 生成器里的板型真值导出成 W7 的配方 JSON。

=== 为什么要有这个脚本 ===
配方坐标**手抄就是下一次事故**。这个项目已经因为抄副本吃过四遍亏
(EXPECTS[6] / lo[0] 越界 / 守卫本身 / 两处金层掩膜漂开),而手抄坐标是
最容易被审漏的一种 —— 一行 8 个数字,看起来对,错了没人知道。

所以这里不去"重新录一遍",而是 import 生成器本体,把它的 layout 直接倒出来。
生成器改了,重跑本脚本就同步;生成器不动,配方就跟着生成器走。

=== 用法 ===
    python tools/export_recipes.py

=== 改配方时 ===
改 W6 生成器,不要改生成的 JSON —— JSON 是产物,不是源。
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W6 = HERE.parent.parent / "W6_PCB_DefectProject"
OUT = HERE.parent / "recipes"

sys.path.insert(0, str(W6))

# gen_complex_pcb 依赖 PIL/qrcode,缺了就明说,不要静默产出一份空配方。
try:
    import gen_complex_pcb as G
except ImportError as e:  # pragma: no cover
    sys.exit(
        "导入 gen_complex_pcb 失败: %s\n"
        "需要能访问 %s 且装好 pillow / qrcode。" % (e, W6)
    )


# ============================================================
# 本体尺寸表
#
# gen_complex_pcb 的 layout 只记了 (label, kind, cx, cy) —— 中心点,
# 没有本体宽高。而 ROI 是"中心 +- 本体一半 +- 余量",没宽高就算不出框。
#
# 所以每个 kind 对应多宽多高,是从各 draw_* 函数的默认参数里读出来的
# (draw_resistor_small(w=44,h=16)、draw_to220(w=60,h=70) ...)。
# **这是一份会漂的副本**,漂了就表现为"框切歪了/切小了"。
# 改 draw_* 的尺寸时记得回来改这里 —— 或者更好:给 layout 也记上宽高。
# ============================================================
BODY = {
    "RES":       (44, 16),    # draw_resistor_small 默认
    "CAP_SMALL": (36, 18),    # draw_cap_small 默认
    "CAP_BIG":   (40, 40),    # draw_cap_big(r=20) 的圆头直径
    "DIODE":     (40, 14),    # draw_diode 默认
    "TO220":     (60, 70),    # draw_to220 里的 w, h
    "QFP64":     (140, 140),  # draw_qfp64 的 body
    "SOIC8":     (80, 50),    # draw_soic8 里的 w, h
    "BUTTON":    (48, 48),    # draw_button(size=24) 的直径
    "LED":       (16, 16),    # draw_led(r=8) 的直径
    "XTAL":      (50, 22),    # draw_crystal 默认
    "INDUCTOR":  (50, 24),    # draw_inductor 默认
    "TERMINAL":  (32, 30),    # draw_terminal(n=2) 的 w=12*2+8, h=30
    "HEADER":    (40, 8),     # JP1 两个 pad 在 460/500,相距 40
}

# ============================================================
# 规则分配表
#
# 每种元件身上**长什么样**,决定哪几条规则对它有意义。这是物理事实,
# 不是标定出来的数,所以按 kind 分类,而不是按 label —— 按 label 正是
# 原来 `if (label == "R2")` 的病根:换块板那些规则就全空转了。
#
# 判据:
#   missing     要有暗色本体(找得到本体才谈得上"本体没了")
#   shifted     同 missing
#   wrong       本体是单一纯色,换色了才看得出来
#   polarity    本体上有白色极性条
#   bridge      本体两侧有金焊盘,金条连片才算焊桥
#   insufficient 本体上有 >=4 个金焊盘,才有"中位数"可言
#
# 判据按本体的实际配色判定:
#   BLACK 本体 -> RES / DIODE / TO220 / QFP64 / SOIC8
#   浅色本体   -> XTAL(180,180,200) 不适用 missing(会被整框判缺件)
#   彩色本体   -> CAP_SMALL(YELLOW) CAP_BIG(灰) LED(橙) TERMINAL(绿)
#                BUTTON(灰) INDUCTOR(70,70,90) 都不适用 missing。
#
# ⚠ bridge 只给**两脚元件**,这是量出来的,不是拍的。
#
#   bridge 的判据是"ROI 内最长一行金像素数 / ROI 宽"(pcb_defect_judge.cpp)。
#   这个量对**两脚**元件才成立:焊盘分居左右,连片了才会变长。
#   对宽封装它量的其实是"焊盘本来就占多宽",于是好板直接顶到阈值:
#       好板实测  CAP_SMALL 85.71~96.43  TO220 100.00  INDUCTOR 84.29
#                 DIODE(power) 88.33
#   而真正焊桥的样本只有 91.43 —— **落在健康芯片电容的区间里**。
#   也就是说在这批图上,bridge 对宽封装**没有可用的阈值**:
#   调到 90 则好板全过,调到 40 则好板全炸,中间没有能分开的地方。
#
#   试过两种改法都不成立(都实测过):
#     · 改成"最长**连续**金段" —— 健康电容 51.79~62.50,焊桥 52.86,照样重叠。
#       (芯片电容的连续段就是**一个焊盘**本身,焊盘本来就占半个 ROI。)
#     · 改成"金层连通块个数" —— 好板 XTAL 1 块 / 焊桥 XTAL 4 块,
#       方向反了(焊桥本该让两块并成一块),而且其余封装在好板/废板上
#       块数一样(电阻都是 3 块),分不开。
#
#   结论:bridge 判据要重做(按封装建"焊盘间距"的期望值),那是下一步。
#   这一轮据实把 bridge 从宽封装上摘掉 —— 让它别报它**证明不了**的缺陷。
#   覆盖度会因此下降,但 judge 本来就会把"哪些元件没配规则"写进 warning,
#   不会假装整板都查过了。
# ============================================================
CHECKS = {
    "RES":       ["missing", "shifted", "wrong", "bridge"],
    "DIODE":     ["missing", "shifted", "polarity"],
    # ⚠ CAP_BIG / CAP_SMALL 配 missing 是**量出来才加的**,原来按"本体不是黑的"
    #   判定它们不适用 missing —— 那是拍的,实测推翻:
    #   power_missing.png 把 C2 整个擦掉,C2 暗像素 2.94% -> 0.00%,
    #   而好板 C1/C2 是 2.22/2.94。这一条是让 power_missing 从漏检变成
    #   抓到的**全部原因**(变异验证:把 CAP_BIG 的 checks 清空,断言立刻转红)。
    #   暗像素虽然只来自本体那一圈黑描边,但 dark_min_pct 是相对板面基色的,
    #   整体亮度缩放时两边一起动,比例不变。
    "CAP_SMALL": ["missing", "polarity"],
    "CAP_BIG":   ["missing", "polarity"],
    "TO220":     ["missing", "insufficient"],
    "QFP64":     ["missing", "insufficient"],
    "SOIC8":     ["missing", "insufficient"],
    "XTAL":      ["insufficient", "bridge"],
    "INDUCTOR":  [],
    "TERMINAL":  [],
    # ⚠ LED / HEADER 看着也像"能判缺件",实测**不能**(2026-10-10):
    #   LED 暗像素 0.77%,HEADER 0.00% —— 本体是橙/绿的自发光色,不是黑的。
    #   给它们配 missing 等于每一颗 LED、每一个排针都报缺件。
    #   这两个数就是它们**必须**留在空表里的理由,不是"忘了配"。
    "LED":       [],
    "BUTTON":    [],
}

# 每块板的判定规则里,R2/R3 这类"偏移容忍度"按元件类别给。
# shifted 的容差在配方里单列,不同封装能差很多(QFP64 挪 20px 是大事,
# 小电阻挪 20px 可能还没出框),写死在算子里就等于"所有元件一样灵"。
SHIFT_TOL_PCT = {"RES": 15.0, "CAP_SMALL": 15.0, "CAP_BIG": 15.0}


def export_demo():
    """W7 自带的那张 1000x700 demo 板。

    真值在 W6/gen_pcb_dataset.py 的 SITES 里(8 元组,带焊盘间距)。
    这个 demo 板不是 gen_complex_pcb 生成的,所以单独处理。

    SITES 是模块级常量,import 进来不会触发生成图片(main() 才有副作用),
    所以直接读它的值 —— 比正则去刨源码文本稳:文本格式一变就崩,
    而且崩了还可能悄悄少一行。
    """
    import gen_pcb_dataset as D
    # 跑一遍 baseline,把"每个 kind 的极性条画在哪一侧"登记进 D.POLARITY_SIDE。
    # 和 export_complex 一样 —— **让生成器自己说**,不在这个脚本里抄一张表。
    # build_baseline() 只返回 Image,不写文件,调用它没有副作用。
    D.build_baseline()
    side_of = dict(D.POLARITY_SIDE)

    sites = []
    for label, kind, cx, cy, _pad_dx, _pad_dy, bw, bh in D.SITES:
        # demo 板是刻意造来测这六条规则的,规则集就是当年那六条
        checks = {
            "R1": ["bridge"], "C1": ["polarity"], "U1": ["insufficient"],
            "R2": ["missing", "shifted"], "C2": [], "R3": ["wrong"],
        }.get(label, [])
        sites.append({
            "label": label, "kind": kind, "cx": cx, "cy": cy,
            "body_w": bw, "body_h": bh, "checks": checks,
            "polarity_side": side_of.get((cx, cy), ""),
        })
    return {
        "name": "demo",
        "title": "W7 自带 demo 板(1000x700)",
        "ref_w": 1000, "ref_h": 700, "buffer": 10,
        "sites": sites,
    }


def export_complex(kind, builder, title):
    _, layout = builder()
    # builder 跑完,POLARITY_SIDE 里是**这块板**登记的 kind -> 侧。
    # 每块板建 baseline 时会先 clear(),所以不会串味。
    side_of = dict(G.POLARITY_SIDE)

    sites = []
    for label, k, cx, cy in layout:
        if k not in BODY:
            sys.exit(
                "kind %r(label %s) 不在 BODY 表里 —— "
                "本体的宽高从哪来?" % (k, label)
            )
        bw, bh = BODY[k]
        checks = list(CHECKS.get(k, []))
        side = side_of.get((cx, cy), "")
        # ⚠ **没有极性条的封装不配 polarity 检查**。
        #   陶瓷电容没有极性标记,给它配了 polarity,规则每次都跑、
        #   每次都"读不到极性条" —— 报告里全是"没查"的噪音,
        #   把真正该看的"没查"淹掉。
        #   有没有极性条,生成器登记了才知道。
        if "polarity" in checks and not side:
            checks.remove("polarity")
        sites.append({
            "label": label, "kind": k, "cx": cx, "cy": cy,
            "body_w": bw, "body_h": bh,
            "checks": checks,
            "shift_tol_pct": SHIFT_TOL_PCT.get(k),
            "polarity_side": side,
        })
    return {
        "name": kind,
        "title": title,
        "ref_w": G.W, "ref_h": G.H, "buffer": 10,
        "sites": sites,
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    recipes = []

    demo = export_demo()
    if demo:
        recipes.append(demo)

    recipes.append(export_complex(
        "power", G.build_power_baseline, "LM2596 电源稳压板"))
    recipes.append(export_complex(
        "mcu", G.build_mcu_baseline, "STM32F103 最小系统板"))
    recipes.append(export_complex(
        "signal", G.build_signal_baseline, "双运放信号链板"))

    for r in recipes:
        p = OUT / ("%s.json" % r["name"])
        p.write_text(
            json.dumps(r, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        print("写出 %-14s %2d 个站点  ->  %s"
              % (r["name"], len(r["sites"]), p))


if __name__ == "__main__":
    main()