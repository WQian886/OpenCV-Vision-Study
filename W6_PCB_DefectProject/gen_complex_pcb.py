#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
W6 复杂电路图生成器
====================

产出 3 种真实功能电路板,用于压测 W6 检测管线：
    1. 电源稳压板  (LM2596 风格)  baseline + 2 张缺陷  → power_*
    2. MCU 最小系统 (STM32 QFP)    baseline + 2 张缺陷  → mcu_*
    3. 双运放信号链 (SOIC-8 ×2)    baseline + 2 张缺陷  → signal_*

合计 9 张图 + 1 个 complex_manifest.csv,输出到 images/complex/。
不修改 gen_pcb_dataset.py 与 main.cpp。
依赖：pillow + qrcode（与 W5/W6 一致）。
"""

import csv
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

try:
    import qrcode
except ImportError:
    print("❌ 缺少 qrcode 库,请运行: pip install qrcode")
    sys.exit(1)


# ============================================================
# 调色板 / 字体
# ============================================================
W, H = 1200, 900

GREEN_PCB   = (40, 90, 50)
GREEN_DARK  = (30, 75, 40)
GOLD        = (30, 180, 220)      # 顶层走线 (BGR)
GOLD_DARK   = (20, 140, 180)
BLUE_BOTTOM = (180, 80, 30)       # 底层走线 (BGR: 蓝)
WHITE       = (255, 255, 255)
BLACK       = (10, 10, 10)
RED         = (40, 40, 220)       # 极性/警告
YELLOW      = (40, 200, 230)
BLUE_DARK   = (180, 80, 30)       # IC 棕褐
GREEN_TERM  = (40, 150, 60)       # 端子绿色
ORANGE_LED  = (40, 140, 230)      # LED 橙
GRAY_BLOCK  = (140, 140, 140)     # 按钮灰


def _font(size):
    try:
        return ImageFont.truetype("arial.ttf", size)
    except Exception:
        return ImageFont.load_default()

FONT_XS = _font(11)
FONT_S  = _font(13)
FONT_M  = _font(16)
FONT_L  = _font(20)
FONT_XL = _font(26)


# ============================================================
# 通用绘图原语
# ============================================================
def draw_pcb_base(img, title):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, H], fill=GREEN_PCB)
    d.rectangle([20, 20, W-20, H-20], outline=WHITE, width=2)
    d.text((30, 26), title, fill=WHITE, font=FONT_L)


def silk(d, text, cx, cy, color=WHITE, font=FONT_S):
    bbox = d.textbbox((0, 0), text, font=font)
    tw, th = bbox[2]-bbox[0], bbox[3]-bbox[1]
    d.text((cx - tw//2, cy - th//2), text, fill=color, font=font)


# ============================================================
# 元件本体登记表(走线要避让的东西)
# ============================================================
#
# 为什么需要这个:
#   真实 PCB 上,元件本体坐在铜层**之上**。一条走线从元件底下穿过去,
#   从相机看到的是"看不见",不是"金线压在黑壳上"。
#
#   这个生成器以前是先画元件、再画走线(Power L279~L313 元件 / L315~L337
#   走线,signal 和 mcu 同),于是走线直接盖在黑色本体上。后果不是不好看,
#   是**基线图本身就是废的**:signal_baseline.png 上 R3(720,450) 被
#   L516/L517 两条走线在几何中心接成一条贯穿本体的金线,两个焊盘被连成
#   同一块 —— 电阻短路了,而这张图被标成 PASS。
#
#   W7 那边为了压住这个假象,把 bridge 阈值从 40 抬到 60,
#   治标没治本;判据本身的注释(pcb_defect_judge.cpp bridge 段)记着这件事。
#
# 所以这里记一份"不透明矩形",由每个 draw_* 在画本体时顺手登记 ——
# **不复写一遍几何**:本体的 w/h 只在 draw_* 里算一次,
# 抄第二份的账这个项目已经吃过好几次(kOffColorMul、EXPECTS、lo[0] 守卫)。
_BODIES = []

# ============================================================
# 极性条画在哪一侧 —— 由画它的那一行代码自己登记
#
# 为什么要登记,而不是在导出脚本里写一张表:
# 导出脚本里已经有一张 BODY 表(本体宽高),它自己的注释写着
# "**这是一份会漂的副本**"。再加一张"极性侧"表就是第二份同类表 ——
# 而"极性条在哪边"是**画它的那一行**才知道的事,别处猜就是猜。
#
# 登记的键是 **(cx, cy)**,不是 kind ——
# 因为**同一个 kind 里有的有极性条、有的没有**:
# power 板的 C3/C4/C5 是有极性条的贴片电容,C6/C7 是没有极性条的
# 陶瓷电容,两者 kind 都是 CAP_SMALL。按 kind 登记会把"有"传染给"没有",
# 于是报告里每个陶瓷电容都"查了极性、读不到标记"。
#
# 按位置登记和本文件里 _BODIES / _hidden() 是同一个路子:
# 位置就是实例的身份。
#
# 值: "left" / "right" —— 相对元件本体,不是相对图。
# ============================================================
POLARITY_SIDE = {}


def opaque(cx, cy, w, h):
    """登记一个不透明本体矩形(居中 w×h)。画本体时调用。"""
    _BODIES.append((cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2))


def _hidden(pt):
    x, y = int(pt[0]), int(pt[1])
    for x0, y0, x1, y1 in _BODIES:
        if x0 <= x <= x1 and y0 <= y <= y1:
            return True
    return False


def trace(d, points, color=GOLD, width=4, layer="top"):
    """画一条折线走线,points=[(x,y),...]

    落在元件本体里的那段不画 —— 本体在铜之上,那一段是被盖住的。
    用 1px 步长切出可见段,再按原样连成折线,所以没被挡住的
    部分渲染结果和以前逐像素一致。
    """
    if not _BODIES:
        for i in range(len(points) - 1):
            d.line([points[i], points[i + 1]], fill=color, width=width)
    else:
        for i in range(len(points) - 1):
            p0, p1 = points[i], points[i + 1]
            n = max(abs(p1[0] - p0[0]), abs(p1[1] - p0[1]))
            if n == 0:
                if not _hidden(p0):
                    d.ellipse([p0[0] - 2, p0[1] - 2, p0[0] + 2, p0[1] + 2], fill=color)
                continue
            run = []
            for s in range(n + 1):
                p = (p0[0] + (p1[0] - p0[0]) * s / n,
                     p0[1] + (p1[1] - p0[1]) * s / n)
                if _hidden(p):
                    if len(run) > 1:
                        d.line(run, fill=color, width=width)
                    run = []
                else:
                    run.append(p)
            if len(run) > 1:
                d.line(run, fill=color, width=width)
    # 在拐点画小圆(模拟过孔)。拐点被本体盖住就同样不画 ——
    # 焊盘上打的过孔是能看见的,黑壳底下的看不见。
    for p in points[1:-1]:
        if not _hidden(p):
            d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=color)


def pad(d, cx, cy, size=10, color=GOLD):
    d.rectangle([cx-size, cy-size, cx+size, cy+size], fill=color, outline=GOLD_DARK)
    d.ellipse([cx-3, cy-3, cx+3, cy+3], fill=GREEN_PCB)  # 中心孔


def pad_round(d, cx, cy, r=8, color=GOLD):
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=color, outline=GOLD_DARK)
    d.ellipse([cx-3, cy-3, cx+3, cy+3], fill=GREEN_PCB)


def silk_box(d, cx, cy, w, h):
    """画元件外框丝印"""
    d.rectangle([cx-w//2-4, cy-h//2-4, cx+w//2+4, cy+h//2+4], outline=WHITE, width=1)


# ============================================================
# 扩展元件库
# ============================================================
def draw_resistor_small(d, cx, cy, w=44, h=16):
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    opaque(cx, cy, w, h)
    for i, c in enumerate([(40,40,180),(40,180,40),(180,40,40),(180,180,40)]):
        x = cx-w//2+8 + i*(w-16)//4
        d.line([(x, cy-h//2+2),(x, cy+h//2-2)], fill=c, width=2)


def draw_cap_small(d, cx, cy, w=36, h=18, color=YELLOW, polarity=False):
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=color, outline=BLACK)
    if polarity:
        d.rectangle([cx-w//2+2, cy-h//2+2, cx-w//2+6, cy+h//2-2], fill=WHITE)
        POLARITY_SIDE[(cx, cy)] = "left"   # 极性条在本体左侧
    opaque(cx, cy, w, h)


def draw_cap_big(d, cx, cy, r=20, polarity=True):
    """电解电容(大圆头 + 极性条)"""
    silk_box(d, cx, cy, r*2+4, r*2+4)
    pad_round(d, cx, cy-r-6, r=8)
    pad_round(d, cx, cy+r+6, r=8)
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=(60,60,60), outline=(5,5,5))
    opaque(cx, cy, r * 2, r * 2)
    # 顶部十字 + 极性条
    d.line([(cx-r+4, cy),(cx+r-4, cy)], fill=(180,180,180), width=1)
    d.line([(cx, cy-r+4),(cx, cy+r-4)], fill=(180,180,180), width=1)
    if polarity:
        d.rectangle([cx-r+3, cy-3, cx-r+7, cy+3], fill=WHITE)  # 极性条
        POLARITY_SIDE[(cx, cy)] = "left"      # 极性条在本体左侧


def draw_inductor(d, cx, cy, w=50, h=24):
    """电感(矩形 + 内部 3 个圆点)"""
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=(70,70,90), outline=BLACK)
    # 内部圆点模拟绕线
    for i, x in enumerate([cx-w//2+10, cx, cx+w//2-10]):
        d.ellipse([x-4, cy-4, x+4, cy+4], fill=(150,150,170))
    opaque(cx, cy, w, h)


def draw_diode(d, cx, cy, w=40, h=14):
    """二极管(矩形 + 极性条 + 箭头)"""
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    # 极性条(阴极侧)
    d.rectangle([cx+2, cy-h//2+2, cx+6, cy+h//2-2], fill=WHITE)
    POLARITY_SIDE[(cx, cy)] = "right"          # 二极管的极性条在本体右侧
    # 二极管符号
    d.line([(cx-12, cy),(cx+12, cy)], fill=WHITE, width=1)
    opaque(cx, cy, w, h)


def draw_to220(d, cx, cy, label="U?"):
    """TO-220 封装(三引脚 + 中间散热片)"""
    w, h = 60, 70
    silk_box(d, cx, cy, w+30, h)
    # 本体
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK, outline=(5,5,5))
    # 散热片孔
    d.ellipse([cx-8, cy-8, cx+8, cy+8], fill=(60,60,60), outline=WHITE)
    # 三个引脚
    pad(d, cx-20, cy+h//2+12, size=8)
    pad(d, cx,    cy+h//2+12, size=8)
    pad(d, cx+20, cy+h//2+12, size=8)
    silk(d, label, cx, cy-h//2-12, font=FONT_S)
    opaque(cx, cy, w, h)


def draw_qfp64(d, cx, cy, label="U?"):
    """QFP-64 封装(四面各 16 引脚)"""
    body = 140
    silk_box(d, cx, cy, body+30, body+30)
    d.rectangle([cx-body//2, cy-body//2, cx+body//2, cy+body//2],
                fill=BLACK, outline=(5,5,5))
    # 1 号脚标记
    d.ellipse([cx-body//2+6, cy-body//2+6, cx-body//2+16, cy-body//2+16], fill=(60,60,60))
    # 4 面引脚
    pitch = body // 17
    for i in range(16):
        # 左
        x = cx - body//2
        y = cy - body//2 + pitch*(i+1)
        pad(d, x-12, y, size=5)
        # 右
        pad(d, x+body+12, y, size=5)
        # 上
        pad(d, cx - body//2 + pitch*(i+1), cy - body//2 - 12, size=5)
        # 下
        pad(d, cx - body//2 + pitch*(i+1), cy + body//2 + 12, size=5)
    silk(d, label, cx, cy+body//2+24, font=FONT_M)
    opaque(cx, cy, body, body)


def draw_soic8(d, cx, cy, label="U?"):
    """SOIC-8 封装(两侧各 4 引脚)"""
    w, h = 80, 50
    silk_box(d, cx, cy, w+30, h)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    # 1 号脚标记
    d.ellipse([cx-w//2+3, cy-h//2+3, cx-w//2+10, cy-h//2+10], fill=(60,60,60))
    # 引脚
    pitch = h // 6
    for i in range(4):
        y = cy - h//2 + pitch*(i+1)
        pad(d, cx-w//2-10, y, size=4)
        pad(d, cx+w//2+10, y, size=4)
    silk(d, label, cx, cy+h//2+14, font=FONT_S)
    opaque(cx, cy, w, h)


def draw_button(d, cx, cy, size=24):
    """轻触按钮(方形 + 4 引脚)"""
    d.rectangle([cx-size, cy-size, cx+size, cy+size], fill=GRAY_BLOCK, outline=BLACK)
    d.ellipse([cx-8, cy-8, cx+8, cy+8], fill=(80,80,80))
    pad_round(d, cx-size-4, cy-size-4, r=5)
    pad_round(d, cx+size+4, cy-size-4, r=5)
    pad_round(d, cx-size-4, cy+size+4, r=5)
    pad_round(d, cx+size+4, cy+size+4, r=5)
    opaque(cx, cy, size * 2, size * 2)


def draw_led(d, cx, cy, r=8, color=ORANGE_LED):
    """LED(圆头 + 2 引脚)"""
    pad_round(d, cx-8, cy, r=5)
    pad_round(d, cx+8, cy, r=5)
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=color, outline=(200,200,200))
    # 极性平边
    d.line([(cx-r+2, cy-r+2),(cx-r+2, cy-r+6)], fill=BLACK, width=2)
    opaque(cx, cy, r * 2, r * 2)


def draw_crystal(d, cx, cy, w=50, h=22):
    """晶振(矩形 + 内部金属壳)"""
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2],
                fill=(180,180,200), outline=BLACK)
    d.text((cx-12, cy-6), "XTAL", fill=BLACK, font=FONT_XS)
    opaque(cx, cy, w, h)


def draw_terminal(d, cx, cy, n=3):
    """绿色接线端子(n 个引脚)"""
    w, h = 12*n+8, 30
    silk_box(d, cx, cy, w, h)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=GREEN_TERM, outline=BLACK)
    for i in range(n):
        x = cx - w//2 + 8 + i*12
        d.ellipse([x-3, cy-3, x+3, cy+3], fill=(220,220,220))
        pad_round(d, x, cy+h//2+6, r=4)
    opaque(cx, cy, w, h)


def draw_qr(d, img, data, size=110, pos=None):
    qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=5, border=2)
    qr.add_data(data)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    qr_img = qr_img.resize((size, size), Image.NEAREST)
    pos = pos or (W-size-40, H-size-40)
    img.paste(qr_img, pos)
    dd = ImageDraw.Draw(img)
    dd.text((pos[0], pos[1]+size+4), "SN:" + data, fill=WHITE, font=FONT_XS)


# ============================================================
# 板 1:电源稳压板（LM2596 风格）
# ============================================================
def build_power_baseline():
    _BODIES.clear()
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "Power Supply v1 — LM2596 Buck")
    d = ImageDraw.Draw(img)

    layout = []  # 收集 (label, kind, x, y) 用于 manifest

    # === 左侧:输入端子 + 输入电容 ===
    draw_terminal(d, 80, 200, n=2);  layout.append(("J1","TERMINAL",80,200))
    draw_cap_big(d, 200, 200, polarity=True);  layout.append(("C1","CAP_BIG",200,200))

    # === 中部:TO-220 IC ===
    draw_to220(d, 380, 200, label="U1 (LM2596)");  layout.append(("U1","TO220",380,200))

    # === 续流二极管 ===
    draw_diode(d, 520, 200);  layout.append(("D1","DIODE",520,200))

    # === 电感 ===
    draw_inductor(d, 680, 200);  layout.append(("L1","INDUCTOR",680,200))

    # === 输出电容 ===
    draw_cap_big(d, 820, 200, polarity=True);  layout.append(("C2","CAP_BIG",820,200))

    # === 输出端子 ===
    draw_terminal(d, 980, 200, n=2);  layout.append(("J2","TERMINAL",980,200))

    # === 第二行:反馈电阻分压 ===
    draw_resistor_small(d, 380, 360);  layout.append(("R1","RES",380,360))
    draw_resistor_small(d, 480, 360);  layout.append(("R2","RES",480,360))

    # === 第三行:状态指示 LED + 限流电阻 ===
    draw_led(d, 200, 480, color=(0, 220, 0));  layout.append(("LED1","LED",200,480))
    draw_resistor_small(d, 290, 480);  layout.append(("R3","RES",290,480))

    # === 第四行:多个去耦小电容 ===
    for i, x in enumerate([620, 720, 820]):
        draw_cap_small(d, x, 480, polarity=True)
        layout.append((f"C{i+3}","CAP_SMALL",x,480))

    # === 底部:输入/输出电容 + 散热注释 ===
    draw_cap_small(d, 200, 640);  layout.append(("C6","CAP_SMALL",200,640))
    draw_cap_small(d, 820, 640);  layout.append(("C7","CAP_SMALL",820,640))

    # === 走线拓扑（粗电源线 + 细信号线） ===
    # 输入 12V:J1+ → C1+ → U1.VIN
    trace(d, [(80, 188), (200, 188), (200, 156), (380, 156)], width=8)
    trace(d, [(80, 212), (200, 212), (200, 244), (380, 244)], width=8)
    # U1.SW → D1 → L1 → C2+
    trace(d, [(380, 240), (520, 240), (520, 188), (680, 188), (680, 156), (820, 156)], width=8)
    # GND
    trace(d, [(200, 244), (380, 244), (520, 212), (680, 212), (820, 244), (820, 212)], width=6)
    # 输出到 J2
    trace(d, [(820, 156), (980, 156)], width=8)
    trace(d, [(820, 244), (980, 244)], width=8)
    # 反馈 R1-R2 分压
    trace(d, [(820, 156), (480, 320), (480, 360)], width=3)
    trace(d, [(480, 360), (380, 360)], width=3)
    trace(d, [(380, 360), (380, 280), (380, 244)], width=3)
    trace(d, [(380, 360), (380, 440), (480, 440), (480, 360)], width=3)  # 模拟 R2 到 GND
    # LED 指示
    trace(d, [(820, 156), (290, 440), (290, 480)], width=3)
    trace(d, [(290, 480), (200, 480), (200, 680), (200, 692)], width=3)
    # 去耦电容 (底层走线 = 蓝色)
    trace(d, [(380, 244), (620, 244), (620, 440), (620, 480)], width=3, color=BLUE_BOTTOM)
    trace(d, [(380, 244), (720, 440), (720, 480)], width=3, color=BLUE_BOTTOM)
    trace(d, [(380, 244), (820, 440), (820, 480)], width=3, color=BLUE_BOTTOM)

    draw_qr(d, img, "PWR-2026-A1")
    return img, layout


def defect_power_short(base, layout):
    """电源板短路:C1 和 L1 之间 GND 与 VOUT 桥接"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    # 在 (200, 244) 与 (820, 156) 之间画一条金色短路
    d.rectangle([200, 240, 820, 250], fill=GOLD, outline=RED)
    d.ellipse([460, 220, 540, 280], outline=RED, width=3)
    silk(d, "SHORT!", 500, 240, color=RED, font=FONT_M)
    return img, layout


def defect_power_missing(base, layout):
    """电源板缺件:C2 输出电容整个擦掉"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = 820, 200
    d.rectangle([cx-30, cy-30, cx+30, cy+30], fill=GREEN_PCB)
    # 焊盘保留
    pad_round(d, cx, cy-26, r=8)
    pad_round(d, cx, cy+26, r=8)
    return img, layout


# ============================================================
# 板 2:MCU 最小系统（STM32 QFP-64 风格）
# ============================================================
def build_mcu_baseline():
    _BODIES.clear()
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "MCU Core v1 — STM32F103 QFP-64")
    d = ImageDraw.Draw(img)

    layout = []

    # === 中央 MCU ===
    draw_qfp64(d, 600, 450, label="U1 (STM32)");  layout.append(("U1","QFP64",600,450))

    # === 晶振(靠近 MCU) ===
    draw_crystal(d, 600, 240);  layout.append(("Y1","XTAL",600,240))
    draw_crystal(d, 700, 240);  layout.append(("Y2","XTAL",700,240))

    # === 晶振旁路电容 ===
    draw_cap_small(d, 530, 240, polarity=False);  layout.append(("C1","CAP_SMALL",530,240))
    draw_cap_small(d, 770, 240, polarity=False);  layout.append(("C2","CAP_SMALL",770,240))

    # === 四角各 2 个去耦电容 ===
    cap_positions = [(180, 200),(180, 700),(1020, 200),(1020, 700)]
    for i,(x,y) in enumerate(cap_positions):
        draw_cap_small(d, x, y, polarity=False)
        layout.append((f"C{i+3}","CAP_SMALL",x,y))

    # === 复位按钮(右下) ===
    draw_button(d, 1020, 450);  layout.append(("SW1","BUTTON",1020,450))
    draw_resistor_small(d, 940, 450);  layout.append(("R1","RES",940,450))

    # === LED + 限流电阻(左上) ===
    draw_led(d, 180, 450, color=(0, 220, 0));  layout.append(("LED1","LED",180,450))
    draw_resistor_small(d, 260, 450);  layout.append(("R2","RES",260,450))

    # === BOOT 跳线(顶部) ===
    pad_round(d, 460, 240, r=4)
    pad_round(d, 500, 240, r=4)
    layout.append(("JP1","HEADER",480,240))

    # === 8 个限流电阻(底部,模拟 IO 口) ===
    for i, x in enumerate(range(180, 1021, 110)):
        draw_resistor_small(d, x, 820)
        layout.append((f"R{i+3}","RES",x,820))

    # === 走线拓扑 ===
    # MCU 电源:左下 + 右下 旁路电容 → MCU VDD
    for cap in [(180, 200), (1020, 200), (180, 700), (1020, 700)]:
        trace(d, [(cap[0], cap[1]), (600, 450)], width=4, color=BLUE_BOTTOM)

    # 晶振到 MCU
    trace(d, [(600, 240), (600, 380)], width=3)
    trace(d, [(700, 240), (700, 380)], width=3)
    trace(d, [(530, 240), (530, 380)], width=3)
    trace(d, [(770, 240), (770, 380)], width=3)

    # 复位按钮到 MCU NRST
    trace(d, [(1020, 450), (670, 450)], width=3)
    trace(d, [(940, 450), (940, 380), (670, 380)], width=3)

    # LED 指示
    trace(d, [(180, 450), (530, 450)], width=3)
    trace(d, [(260, 450), (260, 380), (530, 380)], width=3)

    # BOOT 跳线
    trace(d, [(460, 240), (600, 320)], width=3)
    trace(d, [(500, 240), (600, 310)], width=3)

    # 8 个 IO 电阻(底层走线)
    for i, x in enumerate(range(180, 1021, 110)):
        trace(d, [(x, 820), (x, 580), (600, 520)], width=3, color=BLUE_BOTTOM)

    draw_qr(d, img, "MCU-2026-B2")
    return img, layout


def defect_mcu_wrong(base, layout):
    """MCU 板错件:R2(本该是电阻)换成了小电容"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = 260, 450
    # 擦掉原电阻
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 画一个黄色电容
    draw_cap_small(d, cx, cy, color=YELLOW, polarity=True)
    return img, layout


def defect_mcu_bridge(base, layout):
    """MCU 板焊桥:Y1 晶振两个引脚短接"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    # Y1 中心 (600, 240), 引脚在 (574,240) 和 (626,240)
    d.rectangle([574, 232, 626, 248], fill=GOLD, outline=RED)
    d.ellipse([580, 220, 620, 260], outline=RED, width=2)
    return img, layout


# ============================================================
# 板 3:双运放信号链
# ============================================================
def build_signal_baseline():
    _BODIES.clear()
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "Signal Chain v1 — Dual Op-Amp Filter")
    d = ImageDraw.Draw(img)

    layout = []

    # === 输入端子 ===
    draw_terminal(d, 80, 450, n=2);  layout.append(("J1","TERMINAL",80,450))

    # === 第一级 RC 低通: R1 + C1 ===
    draw_resistor_small(d, 220, 450);  layout.append(("R1","RES",220,450))
    draw_cap_small(d, 340, 450, polarity=True);  layout.append(("C1","CAP_SMALL",340,450))

    # === 第一级运放 U1 ===
    draw_soic8(d, 480, 450, label="U1 (OPA)");  layout.append(("U1","SOIC8",480,450))

    # === 反馈 R2 + 反馈 C2 ===
    draw_resistor_small(d, 480, 350);  layout.append(("R2","RES",480,350))
    draw_cap_small(d, 580, 350, polarity=True);  layout.append(("C2","CAP_SMALL",580,350))

    # === 第二级高通: C3 + R3 ===
    draw_cap_small(d, 620, 450, polarity=True);  layout.append(("C3","CAP_SMALL",620,450))
    draw_resistor_small(d, 720, 450);  layout.append(("R3","RES",720,450))

    # === 第二级运放 U2 ===
    draw_soic8(d, 860, 450, label="U2 (OPA)");  layout.append(("U2","SOIC8",860,450))

    # === 反馈 R4 + 二极管限幅 D1/D2 ===
    draw_resistor_small(d, 860, 350);  layout.append(("R4","RES",860,350))
    draw_diode(d, 760, 350);  layout.append(("D1","DIODE",760,350))
    draw_diode(d, 960, 350);  layout.append(("D2","DIODE",960,350))

    # === 输出耦合电容 + 输出端子 ===
    draw_cap_small(d, 1020, 450, polarity=True);  layout.append(("C4","CAP_SMALL",1020,450))
    draw_terminal(d, 1140, 450, n=2);  layout.append(("J2","TERMINAL",1140,450))

    # === 旁路电容(每个运放 2 个) ===
    draw_cap_small(d, 380, 600, polarity=False);  layout.append(("C5","CAP_SMALL",380,600))
    draw_cap_small(d, 580, 600, polarity=False);  layout.append(("C6","CAP_SMALL",580,600))
    draw_cap_small(d, 760, 600, polarity=False);  layout.append(("C7","CAP_SMALL",760,600))
    draw_cap_small(d, 960, 600, polarity=False);  layout.append(("C8","CAP_SMALL",960,600))

    # === 走线 ===
    # 信号链:J1 → R1 → C1 → U1.in → ... → U2.out → J2
    #
    # 端点一律落在**焊盘**上,不走元件几何中心(以前的写法)。
    # 走线焊点坐标就是各 draw_* 里 pad()/引脚的实际位置:
    #   RES w=44      → 焊盘 cx±28      CAP_SMALL w=36 → 焊盘 cx±24
    #   SOIC8 w=80    → 引脚 cx±50,y=cy-17/-9/-1/+7
    #   TERMINAL n=2  → 本体在 cx±16,接线柱在本体下沿
    # 端点落在中心的后果不是"接线画得丑",是金线压在黑色本体上 ——
    # 见文件头 _BODIES 那段,实测 R3 会因此在好板上就短路。
    trace(d, [(96, 450), (192, 450)], width=4)      # J1 右沿 → R1 左焊盘
    trace(d, [(248, 450), (316, 450)], width=4)     # R1 右焊盘 → C1 左焊盘
    trace(d, [(364, 450), (430, 449)], width=4)     # C1 右焊盘 → U1 左脚
    trace(d, [(530, 449), (596, 450)], width=4)     # U1 右脚 → C3 左焊盘
    trace(d, [(644, 450), (692, 450)], width=4)     # C3 右焊盘 → R3 左焊盘
    trace(d, [(748, 450), (810, 449)], width=4)     # R3 右焊盘 → U2 左脚
    trace(d, [(910, 449), (996, 450)], width=4)     # U2 右脚 → C4 左焊盘
    trace(d, [(1044, 450), (1124, 450)], width=4)   # C4 右焊盘 → J2 左沿

    # 反馈 (顶层)
    trace(d, [(480, 350), (480, 425)], width=3)
    trace(d, [(580, 350), (580, 425)], width=3)
    trace(d, [(860, 350), (860, 425)], width=3)
    # 二极管限幅
    trace(d, [(760, 350), (760, 425)], width=2)
    trace(d, [(960, 350), (960, 425)], width=2)

    # GND 网络 (底层蓝色)
    trace(d, [(80, 462), (1140, 462)], width=6, color=BLUE_BOTTOM)
    trace(d, [(340, 462), (1020, 462)], width=4, color=BLUE_BOTTOM)
    # 旁路电容 (底层连接)
    for x in [380, 580, 760, 960]:
        trace(d, [(x, 600), (x, 462)], width=3, color=BLUE_BOTTOM)

    draw_qr(d, img, "SIG-2026-C3")
    return img, layout


def defect_signal_polarity(base, layout):
    """信号板极性反:D1 二极管装反"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = 760, 350
    # 擦掉原 D1
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 极性条改到左侧(原本在右 = 阴极)
    pad(d, cx-26, cy, size=8)
    pad(d, cx+26, cy, size=8)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2] if False else [cx-20, cy-7, cx+20, cy+7],
                fill=BLACK)
    d.rectangle([cx-19, cy-5, cx-13, cy+5], fill=WHITE)  # 极性条移到左侧 = 反
    d.line([(cx-12, cy),(cx+12, cy)], fill=WHITE, width=1)
    return img, layout


def defect_signal_shifted(base, layout):
    """信号板偏移:R3 整体平移 20px"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = 720, 450
    # 擦掉原 R3
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 偏移后重画
    draw_resistor_small(d, cx+20, cy+8)
    return img, layout


# ============================================================
# 主流程
# ============================================================
REGISTRY = [
    # (kind, name, builder, defect_fn or None)
    ("power",  "power_baseline",   build_power_baseline,   None),
    ("power",  "power_short",      build_power_baseline,   defect_power_short),
    ("power",  "power_missing",    build_power_baseline,   defect_power_missing),
    ("mcu",    "mcu_baseline",     build_mcu_baseline,     None),
    ("mcu",    "mcu_wrong",        build_mcu_baseline,     defect_mcu_wrong),
    ("mcu",    "mcu_bridge",       build_mcu_baseline,     defect_mcu_bridge),
    ("signal", "signal_baseline",  build_signal_baseline,  None),
    ("signal", "signal_polarity",  build_signal_baseline,  defect_signal_polarity),
    ("signal", "signal_shifted",   build_signal_baseline,  defect_signal_shifted),
]


def main():
    out_dir = Path(__file__).resolve().parent / "images" / "complex"
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"📁 输出目录: {out_dir}")
    print(f"📐 画面尺寸: {W}x{H}\n")

    manifest = []
    summary_by_kind = {}

    for kind, name, builder, defect_fn in REGISTRY:
        base, layout = builder()
        if defect_fn is not None:
            img, _ = defect_fn(base, layout)
            expected = f"FAIL:{name.split('_', 1)[1]}"
        else:
            img = base
            expected = "PASS"

        path = out_dir / f"{name}.png"
        img.save(path)
        # 统计
        n_comp = len(layout)
        n_traces = sum(1 for c in layout if c[1] in ("TERMINAL",))  # 粗略
        area_kb = path.stat().st_size // 1024
        manifest.append({
            "file":       path.name,
            "kind":       kind,
            "components": n_comp,
            "size":       f"{W}x{H}",
            "filesize_kb": area_kb,
            "expected":   expected,
        })
        # 按 kind 累加
        if kind not in summary_by_kind:
            summary_by_kind[kind] = {"n": 0, "comp": 0}
        summary_by_kind[kind]["n"] += 1
        summary_by_kind[kind]["comp"] = n_comp  # 同一 kind 元件数相同

        defect_type = name.split("_", 1)[1] if "_" in name else "baseline"
        print(f"  ✅ {path.name:28s}  {n_comp:2d} 元件  ({kind:6s} / {defect_type})")

    # 写 manifest
    csv_path = out_dir / "complex_manifest.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["file", "kind", "components", "size", "filesize_kb", "expected"])
        w.writeheader()
        for row in manifest:
            w.writerow(row)
    print(f"\n  ✅ complex_manifest.csv  ({len(manifest)} 行)")

    # 复杂度汇总
    print("\n📊 复杂度汇总:")
    print(f"  {'板型':<10} {'数量':<6} {'元件/板':<10} {'总图数'}")
    print("  " + "-"*40)
    total = 0
    for k, v in summary_by_kind.items():
        print(f"  {k:<10} {v['comp']:<10} {v['n']:<6} -")
        total += v["n"]
    print(f"  {'合计':<10} {'':<10} {'':<6} {total}")

    print(f"\n✨ 完成。{len(manifest)} 张复杂电路图已生成。")
    print("👉 跑验证: w6_pcb_defect.exe inspect images/complex/power_baseline.png")


if __name__ == "__main__":
    main()
