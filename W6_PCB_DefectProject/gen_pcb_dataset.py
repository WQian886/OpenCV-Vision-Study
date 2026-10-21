#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
W6 PCB 缺陷检测 — 测试数据集合成器
=====================================

生成一张"标准 PCB"（baseline）+ 7 类缺陷变体：
    missing       缺件
    wrong         错件
    polarity      极性反
    bridge        焊桥
    insufficient  焊少
    shifted       偏移
    short         短路

依赖：
    pip install pillow qrcode

输出（默认写到本脚本同级的 images/ 目录）：
    baseline.png
    defect_missing.png
    defect_wrong.png
    defect_polarity.png
    defect_bridge.png
    defect_insufficient.png
    defect_shifted.png
    defect_short.png
    manifest.csv      ← 每张图的元信息（路径/缺陷类型/期望 PASS/FAIL）
"""

import csv
import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

try:
    import qrcode
except ImportError:
    print("❌ 缺少 qrcode 库,请运行: pip install qrcode")
    sys.exit(1)


# ============================================================
# 1. 画布与调色板
# ============================================================
W, H = 1000, 700

# PCB 调色板（BGR 顺序记一下,OpenCV 那边是 BGR）
GREEN_PCB   = (40, 90, 50)     # 板基色
GREEN_DARK  = (30, 75, 40)     # 走线层底色（轻微纹理）
GOLD        = (30, 180, 220)   # 金色走线 / 焊盘（BGR: 220,180,30 ≈ 金黄）
GOLD_DARK   = (20, 140, 180)
WHITE       = (255, 255, 255)  # 丝印
BLACK       = (10, 10, 10)     # 元件本体
RED         = (40, 40, 220)    # 极性标记
YELLOW      = (40, 200, 230)   # 电容黄
BLUE_DARK   = (180, 80, 30)    # IC 蓝（棕褐色也行）

# "错件"用的那个件的颜色 —— **不能是铜色**。
#
# ⚠ 2026-10-20 单独拎出来一个常量,原因是一个实测出来的坑:
#   错件图 = "该装电阻的位置换成了一个电容"。而这个电容的本体如果是
#   **铜色**(落进金层色带 H[85,115]),那么这个元件的**端子间隙里全是金属** ——
#   短路判据量"两脚之间的缝里有没有金属",量到的就是本体的颜色,
#   于是**好端端的板报了一条短路**(实测:MCU 板的 mcu_wrong.png 上
#   R2 同时报了 wrong 和 short,而它压根没被短路)。
#
#   所以缺陷图里**颜色必须唯一对应一种物理含义**:铜色 = 铜箔,
#   不许拿来当别的件的颜色。YELLOW 之所以还是铜色,是调色板的老问题
#   (常量按 RGB 写、注释按 BGR 写,"电容黄"实际渲染成青蓝)——
#   整个调色板要改就是另一个大工程,这里只把**错件**用的颜色摘出来。
WRONG_PART  = (40, 200, 120)   # 错件:青绿色(明亮、偏色,一眼不是电阻)

# ============================================================
# 极性条画在哪一侧 —— 由画它的那一行代码自己登记
#
# 和 gen_complex_pcb.py 里的同名表是同一个道理:
# "极性条在哪边"只有画它的那一行知道,别处写就是猜 ——
# 而猜错的后果是**判据把好板判成反装**(静默误报)。
#
# 键是 (cx, cy):位置就是实例的身份(同一个 kind 里也可能有带条/不带条的)。
# 值 "left"/"right" 相对**元件本体**,不是相对图。
# ============================================================
POLARITY_SIDE = {}

# ============================================================
# 端子(焊盘)位置登记 —— 由画焊盘的那一行代码自己登记
#
# 为什么要登记:
#   短路判据要回答的是"**这两个端子之间**有没有金属"(两脚被连通),
#   而"哪两个端子属于同一个元件、它们在哪儿"只有画它们的代码知道。
#   和 POLARITY_SIDE 一个道理:期望侧由**知道真相的那一方**登记,
#   判据不猜 —— 猜错的后果是把好板判成短路(静默误报)。
#
# 键是 (cx, cy):位置就是实例的身份(和 POLARITY_SIDE 一致)。
# 值是 [(x, y, 焊盘半宽), ...]:焊盘中心 + 半宽。
# 半宽的用处:把"焊盘自己"从"两脚之间的绝缘缝"里掐掉 ——
# 焊盘本来就是金属,它是端子,不是缺陷。
# ============================================================
PADS = {}

# 字体（Pillow 自带 bitmap 字体,避免 Windows 字体缺失）
def _font(size):
    try:
        return ImageFont.truetype("arial.ttf", size)
    except Exception:
        return ImageFont.load_default()

FONT_S = _font(14)
FONT_M = _font(18)
FONT_L = _font(22)


# ============================================================
# 2. 布局：6 个元件位（3 列 × 2 行）
# ============================================================
# (label, kind, pad_cx, pad_cy, pad_dx, pad_dy, w, h)
#   pad_cx/pad_cy = 两个焊盘的中心
#   pad_dx/pad_dy = 两个焊盘的间距
#   w, h          = 元件本体宽高
SITES = [
    # 左上 R1
    ("R1", "R",  150, 200,  40, 0,   100, 28),
    # 中上 C1
    ("C1", "C",  400, 200,  36, 0,   70,  28),
    # 右上 U1 (IC)
    ("U1", "IC", 720, 200,  90, 0,   180, 60),
    # 左下 R2
    ("R2", "R",  150, 480,  40, 0,   100, 28),
    # 中下 C2
    ("C2", "C",  400, 480,  36, 0,   70,  28),
    # 右下 R3
    ("R3", "R",  700, 480,  40, 0,   100, 28),
]

# 走线（连接相邻焊盘外侧端点）
# 每条 = (x1, y1, x2, y2, width)
TRACES = [
    # 横向主走线（上排）
    ( 80, 200, 110, 200, 6),
    (190, 200, 382, 200, 6),
    (418, 200, 630, 200, 6),
    (810, 200, 950, 200, 6),
    # 横向主走线（下排）
    ( 80, 480, 110, 480, 6),
    (190, 480, 382, 480, 6),
    (418, 480, 664, 480, 6),
    (740, 480, 950, 480, 6),
    # 上下排连接（垂直走线）
    (110, 200, 110, 480, 6),
    (950, 200, 950, 480, 6),
    (382, 200, 382, 480, 6),
    (810, 200, 810, 280, 6),
    (740, 480, 740, 380, 6),
]


# ============================================================
# 3. 绘图原语
# ============================================================
def draw_pcb_base(img):
    """画 PCB 底色 + 边框 + 标题"""
    d = ImageDraw.Draw(img)
    # 底色
    d.rectangle([0, 0, W, H], fill=GREEN_PCB)
    # 边框（丝印白线）
    d.rectangle([20, 20, W-20, H-20], outline=WHITE, width=2)
    # 标题
    d.text((30, 30), "PCB-2026-001", fill=WHITE, font=FONT_M)


def draw_trace(d, x1, y1, x2, y2, w=GOLD, width=6):
    """画一条金色走线"""
    d.line([(x1, y1), (x2, y2)], fill=w, width=width)


def draw_pad(d, cx, cy, size=12, color=GOLD):
    """画一个方形焊盘"""
    d.rectangle([cx-size, cy-size, cx+size, cy+size], fill=color, outline=GOLD_DARK)


def draw_two_pads(d, cx, cy, dx, pad_size=12):
    """画一对焊盘（电阻/电容用）"""
    draw_pad(d, cx - dx, cy, pad_size)
    draw_pad(d, cx + dx, cy, pad_size)


def draw_ic_pads(d, cx, cy, dx, count=6, pad_size=8, body_h=60):
    """画 IC 多引脚焊盘（上下各 count 个），返回端子列表

    ⚠ 焊盘必须**伸出本体**,不能埋在本体内。

    以前这排焊盘正好画在本体边缘上(cy±30 = 本体半高),
    而本体是后画的 —— 于是本体把焊盘的内侧盖掉,两端的焊盘
    只剩一条可见、中间的被盖光。实测同一个 IC 上:
        两端的焊盘可见 64px²,中间的 136px²
    焊盘本身一点没变小,只是**被本体挡得不一样多** ——
    而"这个焊盘比同侪小多少"正是"焊少"判据要量的东西,
    假象直接被当成了缺陷。

    所以这里让焊盘排布的横向范围**比本体宽**(两端各多出
    pad_size + gap),本体就挡不到焊盘了,各焊盘可见面积一致。

    返回端子列表(而不是在这里登记):调用方拿到的必须是
    **实际画出去的坐标**,不许再算一遍 —— 抄第二份几何是
    本项目栽过好几次的坑。
    """
    gap = 6
    span = dx + pad_size + gap          # 焊盘排布的半宽(比本体宽)
    spacing = 2 * span / (count - 1)
    # 竖直方向:焊盘**整个**露在本体外(内缘刚好贴着本体边)。
    # 埋在本体里的那一半量不到 —— "焊盘面积"量的是**看得见的铜**,
    # 一半被黑壳盖住的焊盘量出来的数只有一半,和同侪没法比。
    py = body_h // 2 + pad_size
    pads = []
    for i in range(count):
        x = int(cx - span + i * spacing)
        draw_pad(d, x, cy - py, pad_size)
        draw_pad(d, x, cy + py, pad_size)
        pads.append((x, cy - py, pad_size))
        pads.append((x, cy + py, pad_size))
    return pads


def draw_resistor(d, cx, cy, w, h, color=BLACK, polarity_dot=False):
    """画一个电阻（黑长方 + 圆头色环）"""
    x0, y0 = cx - w//2, cy - h//2
    x1, y1 = cx + w//2, cy + h//2
    d.rectangle([x0, y0, x1, y1], fill=color, outline=(5, 5, 5))
    # 色环
    ring_colors = [(40, 40, 180), (40, 180, 40), (180, 40, 40), (180, 180, 40)]
    for i, c in enumerate(ring_colors):
        x = x0 + 12 + i * (w - 24) // 4
        d.line([(x, y0+2), (x, y1-2)], fill=c, width=3)
    if polarity_dot:
        d.ellipse([x1-6, y0+2, x1-2, y0+6], fill=RED)


def draw_capacitor(d, cx, cy, w, h, color=YELLOW, polarity_dot=False):
    """画一个电容（黄/黑长方 + 极性条）"""
    x0, y0 = cx - w//2, cy - h//2
    x1, y1 = cx + w//2, cy + h//2
    d.rectangle([x0, y0, x1, y1], fill=color, outline=(5, 5, 5))
    # 极性条（一侧白条,代表负极）
    d.rectangle([x0+2, y0+2, x0+8, y1-2], fill=WHITE)
    POLARITY_SIDE[(cx, cy)] = "left"   # 极性条在本体左侧
    if polarity_dot:
        d.ellipse([x1-6, y0+2, x1-2, y0+6], fill=RED)


def draw_ic(d, cx, cy, w, h, color=BLUE_DARK, pin_dot_at=None):
    """画一个 IC（深色长方 + 半月缺口 + 引脚小白点）"""
    x0, y0 = cx - w//2, cy - h//2
    x1, y1 = cx + w//2, cy + h//2
    d.rectangle([x0, y0, x1, y1], fill=color, outline=(5, 5, 5))
    # 缺口
    d.ellipse([x0+4, cy-8, x0+16, cy+8], fill=(20, 20, 20))
    # 1 号脚标记
    if pin_dot_at:
        px, py = pin_dot_at
        d.ellipse([px-3, py-3, px+3, py+3], fill=WHITE)


def draw_silk(d, label, cx, cy):
    """画丝印白字（label 居中放在元件上方）"""
    bbox = d.textbbox((0, 0), label, font=FONT_M)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text((cx - tw//2, cy - 40), label, fill=WHITE, font=FONT_M)


def draw_component(d, kind, cx, cy, dx, w, h, rotated=False):
    """统一入口：按 kind 画一个元件（rotated 用于极性反缺陷）"""
    if kind == "R":
        draw_two_pads(d, cx - dx, cy, 0)
        draw_two_pads(d, cx + dx, cy, 0)
        # 登记端子位置(短路判据的唯一依据)。半宽 = draw_two_pads 的 pad_size。
        PADS[(cx, cy)] = [(cx - dx, cy, 12), (cx + dx, cy, 12)]
        if rotated:
            draw_resistor(d, cx, cy, h, w)   # 反转宽高=180°旋转
        else:
            draw_resistor(d, cx, cy, w, h)
    elif kind == "C":
        draw_two_pads(d, cx - dx, cy, 0)
        draw_two_pads(d, cx + dx, cy, 0)
        PADS[(cx, cy)] = [(cx - dx, cy, 12), (cx + dx, cy, 12)]
        if rotated:
            draw_capacitor(d, cx, cy, w, h, polarity_dot=False)
        else:
            draw_capacitor(d, cx, cy, w, h, polarity_dot=True)
    elif kind == "IC":
        # 端子位置由画焊盘的那一行算出来并返回 —— 这里只搬运,
        # 不再自己算一遍 x(抄第二份几何 = 本项目栽过好几次的坑)。
        ic_pads = draw_ic_pads(d, cx, cy, dx, count=6, body_h=h)
        # IC 的端子多而且密(6x2),短路判据只对"两脚元件"有意义,
        # 照样登记下来 —— 判据看到端子数不是 2 会自己报"没查",
        # 而不是靠配方不写来回避。
        PADS[(cx, cy)] = ic_pads
        if rotated:
            draw_ic(d, cx, cy, w, h, pin_dot_at=None)
        else:
            draw_ic(d, cx, cy, w, h, pin_dot_at=(cx - dx + 6, cy - h // 2))


def draw_qr_code(img, data="PCB-2026-001", size=120, pos=(840, 540)):
    """画 QR 码到指定位置（复用 W5 的 qrcode 库）"""
    qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=6, border=2)
    qr.add_data(data)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    qr_img = qr_img.resize((size, size), Image.NEAREST)
    img.paste(qr_img, pos)
    # 标签
    d = ImageDraw.Draw(img)
    d.text((pos[0], pos[1] + size + 4), "SN:" + data, fill=WHITE, font=FONT_S)


# ============================================================
# 4. 标准板（baseline）
# ============================================================
def build_baseline() -> Image.Image:
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img)
    d = ImageDraw.Draw(img)
    # 走线
    for (x1, y1, x2, y2, w) in TRACES:
        draw_trace(d, x1, y1, x2, y2, GOLD, w)
    # 元件 + 焊盘 + 丝印
    for (label, kind, cx, cy, dx, _dy, w, h) in SITES:
        draw_component(d, kind, cx, cy, dx, w, h, rotated=False)
        draw_silk(d, label, cx, cy)
    # QR 序列号
    draw_qr_code(img, data="PCB-2026-001", size=120, pos=(840, 540))
    return img


# ============================================================
# 5. 缺陷变体（7 类）
# ============================================================
def defect_missing(base: Image.Image, site) -> Image.Image:
    """缺件：把 site 这个元件整个擦掉（焊盘保留,本体不见）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 焊盘保留
    draw_two_pads(d, cx - dx, cy, 0)
    draw_two_pads(d, cx + dx, cy, 0)
    return img


def defect_wrong(base: Image.Image, site) -> Image.Image:
    """错件：把 site 换成 C（应该装电阻的位子装了电容）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    # 擦掉原元件
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 画一个电容（错的）。颜色用 WRONG_PART —— 不能用铜色,
    # 否则这个件的本体自己就落在金属色带里,短路判据会把它当成短路。
    draw_capacitor(d, cx, cy, w, h, color=WRONG_PART, polarity_dot=True)
    return img


def defect_polarity(base: Image.Image, site) -> Image.Image:
    """极性反：把 site 旋转 180°（极性条从左换到右）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    # 擦掉原元件
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 反向画：本体反转 + 极性条改到右侧
    draw_capacitor_polarity_reversed(d, cx, cy, w, h)
    return img


def draw_capacitor_polarity_reversed(d, cx, cy, w, h):
    """极性条放到右侧 = 装反了"""
    x0, y0 = cx - w//2, cy - h//2
    x1, y1 = cx + w//2, cy + h//2
    d.rectangle([x0, y0, x1, y1], fill=YELLOW, outline=(5, 5, 5))
    # 极性条放到右侧
    d.rectangle([x1-8, y0+2, x1-2, y1-2], fill=WHITE)
    # 红点也改到左侧
    d.ellipse([x0+2, y0+2, x0+6, y0+6], fill=RED)


def defect_bridge(base: Image.Image, site) -> Image.Image:
    """焊桥：把 site 的两个焊盘用金色长条连起来"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    # 在两个焊盘之间画一条金色横条
    d.rectangle([cx - dx - 12, cy - 8, cx + dx + 12, cy + 8],
                fill=GOLD, outline=GOLD_DARK)
    return img


def defect_insufficient(base: Image.Image, site) -> Image.Image:
    """焊少：把 site 的某个焊盘擦掉一大块

    擦哪个焊盘 —— 用**登记下来的端子位置**,不在这里再写一遍坐标。
    以前这里写死了 `cx - dx`,而画焊盘的那一行后来把焊盘挪了
    (伸出本体,见 draw_ic_pads 的说明),两处就对不上了:
    缺陷擦在一个**根本没有焊盘**的位置上,而真正的焊盘安然无恙。
    """
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    px, py, pr = PADS[(cx, cy)][0]
    # 擦掉整个焊盘
    d.rectangle([px - pr, py - pr, px + pr, py + pr], fill=GREEN_PCB)
    # 画一个小得多的焊盘（面积 < 阈值）
    draw_pad(d, px, py, size=4, color=GOLD)
    return img


def defect_shifted(base: Image.Image, site) -> Image.Image:
    """偏移：把 site 整体平移 30px,脱离焊盘中心"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    # 擦掉原元件
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 偏移 30px 重新画
    draw_resistor(d, cx + 30, cy + 10, w, h)
    return img


def defect_short(base: Image.Image, site) -> Image.Image:
    """短路：site 的两个焊盘被一条铜条连起来了（两脚被金属连通）

    ⚠ 2026-10-20 重画。原来这张图画的是"在底部走线之间补一段铜"，
      而那一段**本来就存在**(走线 (418,480)-(664,480))——
      画上去只是把 6px 的走线加粗到 7px，图像上根本不像短路，
      电学上也没短接任何东西(那条走线两端本来就通)。
      旧判据之所以能"抓到"它,是因为它量的根本不是短路,
      而是**画给人眼看的那个蓝紫圆环**。

      现在的短路图必须满足两条：
        ① 物理上真的短接了 —— 同一个元件的两个端子之间出现金属；
        ② 自己的判据认得出来 —— 端子位置由本文件登记(PADS),
           判据量的是"两个端子之间的绝缘缝里有没有金属"。
    """
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = site
    # 两个端子之间架一条铜条(端子在 cx±dx,半宽 12)
    d.rectangle([cx - dx, cy - 8, cx + dx, cy + 8], fill=GOLD, outline=GOLD_DARK)
    # 标个红圈便于人眼看出。
    # ⚠ 红圈**不能压到**两个端子之间那段缝(短路判据就靠量那段缝),
    #   所以画在元件外围:它的左右两个极值点在 x=cx±50,落在铜条两端之外。
    #   顺带证明了一件事:判据不依赖这个标记圈(旧的判据依赖它)。
    d.ellipse([cx - 50, cy - 30, cx + 50, cy + 30], outline=RED, width=2)
    return img


# ============================================================
# 6. 主流程
# ============================================================
#
# 第四列"缺陷画在哪个元件上"是**真值表的一部分**。
#
# 它以前藏在函数体里(SITES[3] = R2),真值表里没有这一列,
# 于是"缺陷画在谁身上"只有代码知道 ——
# 测试想验"判据点名的元件 == 缺陷真的画在的那个元件",
# 就只能自己再抄一份,抄错了没人知道(抄副本的老坑)。
#
# 现在位号只在这里写一次:
#   画图  -> site_by_label() 按位号取坐标;
#   真值表 -> manifest.csv 多一列 site,测试从**真值表**读期望。
# ============================================================
DEFECT_REGISTRY = [
    # (名字, 画缺陷的函数, 期望, 缺陷画在哪个元件上)
    ("baseline",       None,                "ok",          ""),
    ("missing",        defect_missing,      "FAIL:missing", "R2"),
    ("wrong",          defect_wrong,        "FAIL:wrong",   "R3"),
    ("polarity",       defect_polarity,     "FAIL:polarity","C1"),
    ("bridge",         defect_bridge,       "FAIL:bridge",  "R1"),
    ("insufficient",   defect_insufficient, "FAIL:insufficient", "U1"),
    ("shifted",        defect_shifted,      "FAIL:shifted", "R2"),
    ("short",          defect_short,        "FAIL:short",   "R2"),
]


def site_by_label(label):
    """按位号取站点行(真值表里的位号 -> SITES 里那一行)

    位号写错就直接抛 —— 不静默地取第一个。
    """
    for s in SITES:
        if s[0] == label:
            return s
    raise KeyError(f"未知位号: {label!r}")


def main():
    out_dir = Path(__file__).resolve().parent / "images"
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"📁 输出目录: {out_dir}")
    base = build_baseline()
    manifest = []

    for name, fn, expected, site_label in DEFECT_REGISTRY:
        if fn is None:
            img = base
        else:
            img = fn(base, site_by_label(site_label))
        # 注意:img 是 RGB,但 OpenCV 读出来是 BGR,这里我们直接 RGB 保存
        # (W6 main.cpp 里用 cv::imread(path, IMREAD_COLOR) 读 BGR,色彩对应一致)
        path = out_dir / f"{name}.png"
        img.save(path)
        print(f"  ✅ {path.name}  ({img.size[0]}x{img.size[1]})")
        manifest.append({
            "file":     path.name,
            "defect":   "none" if expected == "ok" else expected.split(":", 1)[1],
            "expected": "PASS" if expected == "ok" else "FAIL",
            "site":     site_label,
            "note":     "baseline reference" if expected == "ok" else f"synthetic defect: {expected.split(':', 1)[1]}",
        })

    # 写 manifest.csv
    csv_path = out_dir / "manifest.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["file", "defect", "expected", "site", "note"])
        w.writeheader()
        for row in manifest:
            w.writerow(row)
    print(f"  ✅ manifest.csv  ({len(manifest)} 行)")

    print("\n✨ 完成。8 张图 + 1 个 manifest.csv 已生成。")
    print("👉 下一步:VS 打开 W6_PCB_DefectProject → CMake 配置 → F7 编译 → Ctrl+F5 跑 inspect")


if __name__ == "__main__":
    main()
