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

# "错件"用的那个件的颜色 —— **不能是铜色**。
#
# ⚠ 2026-10-20:错件图 = "该装电阻的位置换成了一个电容"。如果这个电容
#   的本体是铜色(落进金层色带 H[85,115]),那它的**端子间隙里全是金属** ——
#   短路判据量"两脚之间的缝里有没有金属",量到的其实是本体的颜色,
#   于是好端端的板报了一条短路(实测:mcu_wrong.png 上 R2 同时报了
#   wrong 和 short,而它压根没被短路)。
#
#   缺陷图里**颜色必须唯一对应一种物理含义**:铜色 = 铜箔。
#   (YELLOW 之所以还是铜色,是调色板的老问题:常量按 RGB 写、
#   注释按 BGR 写,"电容黄"实际渲染成青蓝。整个调色板要改是另一个
#   大工程,这里只把**错件**用的颜色摘出来。)
WRONG_PART  = (40, 200, 120)      # 错件:青绿色(明亮、偏色,一眼不是电阻)


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
# 分层画:走线在**底层**,元件(焊盘 + 本体)在**顶层**
# ============================================================
#
# 为什么必须分层:
#   真实 PCB 上,元件本体和焊盘都坐在铜层**之上**。一条走线从元件
#   底下穿过去,从相机看到的是"看不见",不是"金线压在黑壳上"。
#
#   这个生成器以前是**先画元件、再画走线**,于是走线直接盖在
#   黑色本体和焊盘上。后果不是不好看,是**基线图本身就是废的**:
#     · signal_baseline.png 上 R3(720,450) 被两条走线在几何中心
#       接成一条贯穿本体的金线,两个焊盘被连成同一块 ——
#       电阻短路了,而这张图被标成 PASS;
#     · 2026-10-21 复查: mcu 板 8 个 QFP 焊盘被蓝色走线压成
#       **0 面积**, signal 板 SOIC-8 的 8 个焊盘有 4 个被 GND 蓝线
#       切掉一条(81 -> 72/63)。焊盘一块没少,量出来的"金属面积"
#       却不一样 —— 而"这个焊盘比同侪小多少"正是"焊少"判据要量的
#       东西,同侪之间必须可比。
#
#   为了让走线避开本体,以前还留了一套 _BODIES 矩形裁剪 ——
#   那是**近似**(圆形本体按外接矩形裁,角上多裁掉一截)。
#   分层画以后,本体自然盖住走线,那套机制连同它的近似一起删掉。
#
# 怎么分层(见每个 build_*_baseline):
#   1) 元件画在一张**透明**的顶层图层上(代码顺序不变);
#   2) 走线画在板面(底层)上;
#   3) 最后把顶层图层 alpha_composite 到板面上 —— 元件整体压在走线之上。
#
# 顺带解释一件以前看不懂的事:W7 那边为了让 bridge 阈值压住这个假象,
# 把阈值从 40 抬到 60 —— 治标没治本;判据本身的注释
# (pcb_defect_judge.cpp bridge 段)记着这件事。现在假象没了,
# 但阈值是按**数据**标定的,要不要调回去是另一件事(见 export_recipes.py)。

# ============================================================
# 本体宽高登记 —— 由画本体的那一行代码自己登记
#
# 为什么要有这个:
#   配方里的 body_w/body_h 决定 ROI 的大小。以前这些数字抄在
#   导出脚本(export_recipes.py)的 BODY 表里 —— 那份表自己就写着
#   "**这是一份会漂的副本**",而且真的漂过:
#   2026-10-21 把 QFP-64 的本体从 140x140 改成 200x200,
#   表里还是 140 —— ROI 切小了,焊盘落到框外,规则整条空转。
#
#   抄副本的老坑,这里不要再踩一次:本体多大只有画本体的代码知道,
#   所以由它登记,导出脚本只搬运。
#
# 键:(cx, cy) —— 和 POLARITY_SIDE / PADS 一样,位置就是实例的身份。
# 值:(w, h)
# ============================================================
BODY_SIZE = {}


def reg_body(cx, cy, w, h):
    """登记一个元件本体的宽高。画本体时调用。"""
    BODY_SIZE[(cx, cy)] = (w, h)


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
# 按位置登记和本文件里 BODY_SIZE / PADS 是同一个路子:
# 位置就是实例的身份。
#
# 值: "left" / "right" —— 相对元件本体,不是相对图。
# ============================================================
POLARITY_SIDE = {}


# ============================================================
# 端子(焊盘)位置登记 —— 由画焊盘的那一行代码自己登记
#
# 和 POLARITY_SIDE 同一个道理,同一个理由:
#   短路判据要回答"**这两个端子之间**有没有金属"(两脚被金属连通),
#   而"哪两个端子属于同一个元件、它们在哪儿"只有画它们的代码知道。
#   期望侧由**知道真相的那一方**登记,判据不猜 ——
#   猜错的后果是把好板判成短路,那是静默误报,比漏检更难查。
#
# 键:(cx, cy) —— 位置就是实例的身份(和 POLARITY_SIDE / BODY_SIZE 一致)。
# 值:[(x, y, 焊盘半宽), ...]
#   半宽用来把"焊盘自己"从"两脚之间的绝缘缝"里掐掉 ——
#   焊盘本来就是金属,它是端子,不是缺陷。
# ============================================================
PADS = {}


def reg_pads(cx, cy, pads):
    """登记一个元件的端子位置。画焊盘的那一行调用它。"""
    PADS[(cx, cy)] = pads


def trace(dt, points, color=GOLD, width=4, layer="top"):
    """画一条折线走线,points=[(x,y),...]

    画在**底层**板面上。元件(焊盘 + 本体)是顶层,最后整体压上来 ——
    所以走线从元件底下穿过去的那一段会被挡住,和真板一样
    (本体坐在铜层之上,底下那一段是看不见的)。

    ⚠ 以前这里要自己按一份"不透明矩形"表把走线裁断,那是**近似**
      (圆形本体按外接矩形裁,角上会多裁掉一截)。改成分层画以后,
      本体自然盖住走线,那套裁剪连同它的近似一起删掉了。
    """
    for i in range(len(points) - 1):
        dt.line([points[i], points[i + 1]], fill=color, width=width)
    # 在拐点画小圆(模拟过孔)
    for p in points[1:-1]:
        dt.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=color)


# ============================================================
# 焊少缺陷:某些焊盘画小一号
#
# 为什么必须**在画的时候**画小,而不是画完再擦:
#   画完再擦,要把焊盘那一块整块涂成板色 —— 而焊盘底下**压着走线**
#   (走线在底层、焊盘在顶层,焊盘盖在走线上)。擦掉的是整块矩形,
#   走线就被擦断一截,图上多出一道豁口 —— 那不是"焊少",是"走线断了"。
#
#   实测:power 板 TO-220 的 GND 金线正好从左焊盘上横穿过去;
#   signal 板 SOIC-8 的引脚也各有一条金线接出去。
#
# 键 = 焊盘中心坐标,值 = 新的半宽(size)。
# ⚠ 登记的端子尺寸**不变** —— 那是要跟同侪比的**标称值**,
#   判据量的正是"标称位置上的铜还剩多少"。
# ============================================================
_SHRINK = {}


def pad(d, cx, cy, size=10, color=GOLD):
    """画一个焊盘(实心铜箔方块,中心**没有孔**)

    ⚠ 中心不留孔,是因为这块数据要**量焊盘面积**:
      中心有孔的话,量到的"金"是环形的一圈,形状随焊盘大小变化
      (10x10 的焊盘只剩 51px² 的金,8x8 的只剩 15px²) ——
      同一个封装里焊盘大小不同,量出来的"铜箔面积"差好几倍,
      "焊盘比同侪小多少"这条判据直接失真。
      而且留孔还让相邻焊盘在开运算后连成一片(见 draw_qfp64 的说明)。

      贴片封装的焊盘本来也是实心的(有中心孔的是通孔焊盘),
      这里全画成实心 = 一份统一约定,不留第二种画法。
    """
    size = _SHRINK.get((cx, cy), size)
    d.rectangle([cx-size, cy-size, cx+size, cy+size], fill=color, outline=GOLD_DARK)


def pad_round(d, cx, cy, r=8, color=GOLD):
    """画一个圆形焊盘(实心,无孔 —— 理由同 pad())"""
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=color, outline=GOLD_DARK)


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
    reg_pads(cx, cy, [(cx-w//2-6, cy, 10), (cx+w//2+6, cy, 10)])
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    reg_body(cx, cy, w, h)
    for i, c in enumerate([(40,40,180),(40,180,40),(180,40,40),(180,180,40)]):
        x = cx-w//2+8 + i*(w-16)//4
        d.line([(x, cy-h//2+2),(x, cy+h//2-2)], fill=c, width=2)


def draw_cap_small(d, cx, cy, w=36, h=18, color=YELLOW, polarity=False):
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    reg_pads(cx, cy, [(cx-w//2-6, cy, 10), (cx+w//2+6, cy, 10)])
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=color, outline=BLACK)
    if polarity:
        d.rectangle([cx-w//2+2, cy-h//2+2, cx-w//2+6, cy+h//2-2], fill=WHITE)
        POLARITY_SIDE[(cx, cy)] = "left"   # 极性条在本体左侧
    reg_body(cx, cy, w, h)


def draw_cap_big(d, cx, cy, r=20, polarity=True):
    """电解电容(大圆头 + 极性条)"""
    silk_box(d, cx, cy, r*2+4, r*2+4)
    pad_round(d, cx, cy-r-6, r=8)
    pad_round(d, cx, cy+r+6, r=8)
    reg_pads(cx, cy, [(cx, cy-r-6, 8), (cx, cy+r+6, 8)])
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=(60,60,60), outline=(5,5,5))
    reg_body(cx, cy, r * 2, r * 2)
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
    reg_pads(cx, cy, [(cx-w//2-6, cy, 10), (cx+w//2+6, cy, 10)])
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=(70,70,90), outline=BLACK)
    # 内部圆点模拟绕线
    for i, x in enumerate([cx-w//2+10, cx, cx+w//2-10]):
        d.ellipse([x-4, cy-4, x+4, cy+4], fill=(150,150,170))
    reg_body(cx, cy, w, h)


def draw_diode(d, cx, cy, w=40, h=14):
    """二极管(矩形 + 极性条 + 箭头)"""
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    reg_pads(cx, cy, [(cx-w//2-6, cy, 10), (cx+w//2+6, cy, 10)])
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    # 极性条(阴极侧)
    d.rectangle([cx+2, cy-h//2+2, cx+6, cy+h//2-2], fill=WHITE)
    POLARITY_SIDE[(cx, cy)] = "right"          # 二极管的极性条在本体右侧
    # 二极管符号
    d.line([(cx-12, cy),(cx+12, cy)], fill=WHITE, width=1)
    reg_body(cx, cy, w, h)


def draw_to220(d, cx, cy, label="U?"):
    """TO-220 封装(三引脚 + 中间散热片)"""
    w, h = 60, 70
    silk_box(d, cx, cy, w+30, h)
    # 本体
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK, outline=(5,5,5))
    # 散热片孔
    d.ellipse([cx-8, cy-8, cx+8, cy+8], fill=(60,60,60), outline=WHITE)
    # 三个引脚(间距 20,焊盘 16 —— 之间留 4px 缝)
    pad(d, cx-20, cy+h//2+12, size=8)
    pad(d, cx,    cy+h//2+12, size=8)
    pad(d, cx+20, cy+h//2+12, size=8)
    reg_pads(cx, cy, [(cx-20, cy+h//2+12, 8), (cx, cy+h//2+12, 8),
                      (cx+20, cy+h//2+12, 8)])
    silk(d, label, cx, cy-h//2-12, font=FONT_S)
    reg_body(cx, cy, w, h)


def draw_qfp64(d, cx, cy, label="U?"):
    """QFP-64 封装(四面各 16 引脚)

    ⚠ 2026-10-21 重画。原来的画法有两个错:
      ① 引脚**互相重叠** —— pitch = body//17 = 8,而焊盘 10x10,
         相邻两个重叠 2px,64 个脚连成 4 条金带。
         真实封装不可能这样(那等于 64 个脚全短接),而且真值表
         把这块板标成 PASS —— 数据自己就打架了。
      ② 焊盘带中心孔(见 pad() 的说明),10x10 的焊盘只剩 51px² 的金,
         量"焊盘面积"根本量不准。

      现在:实心焊盘 8x8,pitch 12 —— 相邻之间留 4px 缝,
      缝宽要**大于**开运算的核(complex 板是 5x5),否则缝会被填上,
      又变成连成一片。
    """
    body = 200
    n = 16               # 每面的引脚数
    size = 4             # 焊盘半宽 -> 8x8 实心
    pitch = 12           # 引脚间距:> 2*size,留 4px 缝
    # 位置对称排布(不用 body//17 那种"从边往里数"的写法:
    # 那种写法在 body 改成 200 之后排布就不再居中)
    offs = [pitch * i - (n - 1) * pitch // 2 for i in range(n)]
    silk_box(d, cx, cy, body+30, body+30)
    d.rectangle([cx-body//2, cy-body//2, cx+body//2, cy+body//2],
                fill=BLACK, outline=(5,5,5))
    # 1 号脚标记
    d.ellipse([cx-body//2+6, cy-body//2+6, cx-body//2+16, cy-body//2+16], fill=(60,60,60))
    # 4 面引脚
    qfp_pads = []
    for o in offs:
        # 左
        x = cx - body//2
        y = cy + o
        pad(d, x-12, y, size=size)
        # 右
        pad(d, x+body+12, y, size=size)
        # 上
        pad(d, cx + o, cy - body//2 - 12, size=size)
        # 下
        pad(d, cx + o, cy + body//2 + 12, size=size)
        qfp_pads += [(x-12, y, size), (x+body+12, y, size),
                     (cx + o, cy - body//2 - 12, size),
                     (cx + o, cy + body//2 + 12, size)]
    reg_pads(cx, cy, qfp_pads)
    silk(d, label, cx, cy+body//2+24, font=FONT_M)
    reg_body(cx, cy, body, body)


def draw_soic8(d, cx, cy, label="U?"):
    """SOIC-8 封装(两侧各 4 引脚)

    ⚠ 2026-10-21 重画。原来 pitch = h//6 = 8,而焊盘也是 8x8 ——
      相邻焊盘**首尾相接**(缝宽 0),8 个脚连成 2 片。
      真实封装不会这样,而且连成片以后根本分不出"一个焊盘是多少面积"。
      现在:实心 8x8,pitch 12,留 4px 缝。
    """
    w, h = 80, 50
    n = 4                # 每面的引脚数
    size = 4             # 焊盘半宽 -> 8x8 实心
    pitch = 12           # > 2*size,留 4px 缝
    offs = [pitch * i - (n - 1) * pitch // 2 for i in range(n)]
    silk_box(d, cx, cy, w+30, h)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=BLACK)
    # 1 号脚标记
    d.ellipse([cx-w//2+3, cy-h//2+3, cx-w//2+10, cy-h//2+10], fill=(60,60,60))
    # 引脚
    soic_pads = []
    for o in offs:
        y = cy + o
        pad(d, cx-w//2-10, y, size=size)
        pad(d, cx+w//2+10, y, size=size)
        soic_pads += [(cx-w//2-10, y, size), (cx+w//2+10, y, size)]
    reg_pads(cx, cy, soic_pads)
    silk(d, label, cx, cy+h//2+14, font=FONT_S)
    reg_body(cx, cy, w, h)


def draw_button(d, cx, cy, size=24):
    """轻触按钮(方形 + 4 引脚)"""
    d.rectangle([cx-size, cy-size, cx+size, cy+size], fill=GRAY_BLOCK, outline=BLACK)
    d.ellipse([cx-8, cy-8, cx+8, cy+8], fill=(80,80,80))
    pad_round(d, cx-size-4, cy-size-4, r=5)
    pad_round(d, cx+size+4, cy-size-4, r=5)
    pad_round(d, cx-size-4, cy+size+4, r=5)
    pad_round(d, cx+size+4, cy+size+4, r=5)
    reg_pads(cx, cy, [(cx-size-4, cy-size-4, 5), (cx+size+4, cy-size-4, 5),
                      (cx-size-4, cy+size+4, 5), (cx+size+4, cy+size+4, 5)])
    reg_body(cx, cy, size * 2, size * 2)


def draw_led(d, cx, cy, r=8, color=ORANGE_LED):
    """LED(圆头 + 2 引脚)"""
    pad_round(d, cx-8, cy, r=5)
    pad_round(d, cx+8, cy, r=5)
    reg_pads(cx, cy, [(cx-8, cy, 5), (cx+8, cy, 5)])
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=color, outline=(200,200,200))
    # 极性平边
    d.line([(cx-r+2, cy-r+2),(cx-r+2, cy-r+6)], fill=BLACK, width=2)
    reg_body(cx, cy, r * 2, r * 2)


def draw_crystal(d, cx, cy, w=50, h=22):
    """晶振(矩形 + 内部金属壳)"""
    silk_box(d, cx, cy, w, h)
    pad(d, cx-w//2-6, cy)
    pad(d, cx+w//2+6, cy)
    reg_pads(cx, cy, [(cx-w//2-6, cy, 10), (cx+w//2+6, cy, 10)])
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2],
                fill=(180,180,200), outline=BLACK)
    d.text((cx-12, cy-6), "XTAL", fill=BLACK, font=FONT_XS)
    reg_body(cx, cy, w, h)


def draw_terminal(d, cx, cy, n=3):
    """绿色接线端子(n 个引脚)"""
    w, h = 12*n+8, 30
    silk_box(d, cx, cy, w, h)
    d.rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2], fill=GREEN_TERM, outline=BLACK)
    term_pads = []
    for i in range(n):
        x = cx - w//2 + 8 + i*12
        d.ellipse([x-3, cy-3, x+3, cy+3], fill=(220,220,220))
        pad_round(d, x, cy+h//2+6, r=4)
        term_pads.append((x, cy+h//2+6, 4))
    reg_pads(cx, cy, term_pads)
    reg_body(cx, cy, w, h)


def draw_qr(img, data, size=110, pos=None):
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
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    PADS.clear()             # 同理:端子位置也是每块板重新登记
    BODY_SIZE.clear()        # 本体宽高同样是每块板重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "Power Supply v1 — LM2596 Buck")
    # 分层:元件画在**顶层**图层,走线画在**底层**板面,最后整体压上去
    # (为什么必须分层,见文件头"分层画"那段)
    comp = Image.new("RGBA", (W, H), (0, 0, 0, 0))   # 顶层:元件
    d = ImageDraw.Draw(comp)
    dt = ImageDraw.Draw(img)                        # 底层:走线

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
    trace(dt, [(80, 188), (200, 188), (200, 156), (380, 156)], width=8)
    trace(dt, [(80, 212), (200, 212), (200, 244), (380, 244)], width=8)
    # U1.SW → D1 → L1 → C2+
    trace(dt, [(380, 240), (520, 240), (520, 188), (680, 188), (680, 156), (820, 156)], width=8)
    # GND
    trace(dt, [(200, 244), (380, 244), (520, 212), (680, 212), (820, 244), (820, 212)], width=6)
    # 输出到 J2
    trace(dt, [(820, 156), (980, 156)], width=8)
    trace(dt, [(820, 244), (980, 244)], width=8)
    # 反馈 R1-R2 分压
    trace(dt, [(820, 156), (480, 320), (480, 360)], width=3)
    trace(dt, [(480, 360), (380, 360)], width=3)
    trace(dt, [(380, 360), (380, 280), (380, 244)], width=3)
    trace(dt, [(380, 360), (380, 440), (480, 440), (480, 360)], width=3)  # 模拟 R2 到 GND
    # LED 指示
    trace(dt, [(820, 156), (290, 440), (290, 480)], width=3)
    trace(dt, [(290, 480), (200, 480), (200, 680), (200, 692)], width=3)
    # 去耦电容 (底层走线 = 蓝色)
    trace(dt, [(380, 244), (620, 244), (620, 440), (620, 480)], width=3, color=BLUE_BOTTOM)
    trace(dt, [(380, 244), (720, 440), (720, 480)], width=3, color=BLUE_BOTTOM)
    trace(dt, [(380, 244), (820, 440), (820, 480)], width=3, color=BLUE_BOTTOM)

    # 顶层元件整体压在底层走线之上(见文件头"分层画"那段)
    img = Image.alpha_composite(img.convert("RGBA"), comp).convert("RGB")
    draw_qr(img, "PWR-2026-A1")
    return img, layout


def site_xy(layout, label):
    """按位号取坐标(真值表里的位号 -> 这块板 layout 里那一行)

    缺陷函数不再自己写死坐标 —— 坐标只有 layout 一处,
    位号只有 REGISTRY 一处,两处对上,才画得对。
    """
    for entry in layout:
        if entry[0] == label:
            return entry[2], entry[3]
    raise KeyError(f"未知位号: {label!r}")


def defect_power_short(base, layout, site):
    """电源板短路：site 的两个引脚被一条铜条连起来了(两脚被金属连通)

    ⚠ 2026-10-20 重画。原来这张图画的是"在 (200,244) 与 (820,156) 之间
      拉一条横贯全板的金条",而那条金条:
        ① 跨过了 GND 与 VOUT 之间的**所有**铜箔,图上很"壮观";
        ② 但**短路判据认不出来** —— 旧判据找的是蓝紫圆环,这条金条
           既不是圆环,面积/宽高也全都撞出窗口(实测 0.1992% > 上界 0.08%,
           宽 41.4% 对角线 > 上界 8%),于是整块板一路 PASS(漏检)。

      现在的短路图必须满足两条:
        ① 物理上真的短接了 —— 同一个元件的两个端子之间出现金属;
        ② 自己的判据认得出来 —— 端子位置由本文件登记(PADS),
           判据量的是"两个端子之间的绝缘缝里有没有金属"。
    """
    img = base.copy()
    d = ImageDraw.Draw(img)
    # site 的中心(电感 L1 在 (680,200),两个引脚在 (649,200) 和 (711,200))
    cx, cy = site_xy(layout, site)
    d.rectangle([cx - 36, cy - 6, cx + 36, cy + 6], fill=GOLD, outline=GOLD_DARK)
    # 标个红圈 + 一个字,便于人眼看出。
    # ⚠ 红圈**不能压到**两个端子之间那段缝(短路判据就靠量那段缝),
    #   所以画在元件外围:它在 y=cy±6 处已经跑到 x=610/750 以外了,
    #   而铜条只有 cx±36。红字同理,放在电感上方,离铜条远远的。
    #   顺带证明了一件事:判据不依赖这些标记(旧的判据依赖它)。
    d.ellipse([cx - 70, cy - 40, cx + 70, cy + 40], outline=RED, width=3)
    silk(d, "SHORT!", cx, cy - 80, color=RED, font=FONT_M)
    return img, layout


def defect_power_missing(base, layout, site):
    """电源板缺件:site 输出电容整个擦掉"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = site_xy(layout, site)
    d.rectangle([cx-30, cy-30, cx+30, cy+30], fill=GREEN_PCB)
    # 焊盘保留
    pad_round(d, cx, cy-26, r=8)
    pad_round(d, cx, cy+26, r=8)
    return img, layout


# ============================================================
# 板 2:MCU 最小系统（STM32 QFP-64 风格）
# ============================================================
def build_mcu_baseline():
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    PADS.clear()             # 同理:端子位置也是每块板重新登记
    BODY_SIZE.clear()        # 本体宽高同样是每块板重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "MCU Core v1 — STM32F103 QFP-64")
    # 分层:元件画在**顶层**图层,走线画在**底层**板面,最后整体压上去
    # (为什么必须分层,见文件头"分层画"那段)
    comp = Image.new("RGBA", (W, H), (0, 0, 0, 0))   # 顶层:元件
    d = ImageDraw.Draw(comp)
    dt = ImageDraw.Draw(img)                        # 底层:走线

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
    # 排针没有本体,但配方需要一个框来切 ROI(不然算不出这个站点),
    # 所以显式登记本体框:
    # 排针底下没有会挡走线的黑壳。
    pad_round(d, 460, 240, r=4)
    pad_round(d, 500, 240, r=4)
    layout.append(("JP1","HEADER",480,240))
    reg_pads(480, 240, [(460, 240, 4), (500, 240, 4)])
    reg_body(480, 240, 40, 8)

    # === 8 个限流电阻(底部,模拟 IO 口) ===
    for i, x in enumerate(range(180, 1021, 110)):
        draw_resistor_small(d, x, 820)
        layout.append((f"R{i+3}","RES",x,820))

    # === 走线拓扑 ===
    # MCU 电源:左下 + 右下 旁路电容 → MCU VDD
    for cap in [(180, 200), (1020, 200), (180, 700), (1020, 700)]:
        trace(dt, [(cap[0], cap[1]), (600, 450)], width=4, color=BLUE_BOTTOM)

    # 晶振到 MCU
    trace(dt, [(600, 240), (600, 380)], width=3)
    trace(dt, [(700, 240), (700, 380)], width=3)
    trace(dt, [(530, 240), (530, 380)], width=3)
    trace(dt, [(770, 240), (770, 380)], width=3)

    # 复位按钮到 MCU NRST
    trace(dt, [(1020, 450), (670, 450)], width=3)
    trace(dt, [(940, 450), (940, 380), (670, 380)], width=3)

    # LED 指示
    trace(dt, [(180, 450), (530, 450)], width=3)
    trace(dt, [(260, 450), (260, 380), (530, 380)], width=3)

    # BOOT 跳线
    trace(dt, [(460, 240), (600, 320)], width=3)
    trace(dt, [(500, 240), (600, 310)], width=3)

    # 8 个 IO 电阻(底层走线)
    for i, x in enumerate(range(180, 1021, 110)):
        trace(dt, [(x, 820), (x, 580), (600, 520)], width=3, color=BLUE_BOTTOM)

    # 顶层元件整体压在底层走线之上(见文件头"分层画"那段)
    img = Image.alpha_composite(img.convert("RGBA"), comp).convert("RGB")
    draw_qr(img, "MCU-2026-B2")
    return img, layout


def defect_mcu_wrong(base, layout, site):
    """MCU 板错件:site(本该是电阻)换成了小电容"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = site_xy(layout, site)
    # 擦掉原电阻
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 画一个电容。颜色用 WRONG_PART —— 不能用铜色,否则这个件的本体
    # 自己就落在金属色带里,短路判据会把它当成"两脚被金属连通"。
    draw_cap_small(d, cx, cy, color=WRONG_PART, polarity=True)
    return img, layout


def defect_mcu_bridge(base, layout, site):
    """MCU 板焊桥:site 晶振两个引脚短接"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    # Y1 中心 (600, 240), 引脚在 (574,240) 和 (626,240)
    cx, cy = site_xy(layout, site)
    d.rectangle([cx - 26, cy - 8, cx + 26, cy + 8], fill=GOLD, outline=RED)
    d.ellipse([cx - 20, cy - 20, cx + 20, cy + 20], outline=RED, width=2)
    return img, layout


# ============================================================
# 板 3:双运放信号链
# ============================================================
def build_signal_baseline():
    POLARITY_SIDE.clear()   # 这块板用哪些 kind,重新登记
    PADS.clear()             # 同理:端子位置也是每块板重新登记
    BODY_SIZE.clear()        # 本体宽高同样是每块板重新登记
    img = Image.new("RGB", (W, H), GREEN_PCB)
    draw_pcb_base(img, "Signal Chain v1 — Dual Op-Amp Filter")
    # 分层:元件画在**顶层**图层,走线画在**底层**板面,最后整体压上去
    # (为什么必须分层,见文件头"分层画"那段)
    comp = Image.new("RGBA", (W, H), (0, 0, 0, 0))   # 顶层:元件
    d = ImageDraw.Draw(comp)
    dt = ImageDraw.Draw(img)                        # 底层:走线

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
    #   SOIC8 w=80    → 引脚 cx±50,y=cy-18/-6/+6/+18
    #   TERMINAL n=2  → 本体在 cx±16,接线柱在本体下沿
    # 端点落在中心的后果不是"接线画得丑",是金线压在黑色本体上 ——
    # 见文件头"分层画"那段,实测 R3 会因此在好板上就短路。
    trace(dt, [(96, 450), (192, 450)], width=4)      # J1 右沿 → R1 左焊盘
    trace(dt, [(248, 450), (316, 450)], width=4)     # R1 右焊盘 → C1 左焊盘
    trace(dt, [(364, 450), (430, 456)], width=4)     # C1 右焊盘 → U1 左脚
    trace(dt, [(530, 456), (596, 450)], width=4)     # U1 右脚 → C3 左焊盘
    trace(dt, [(644, 450), (692, 450)], width=4)     # C3 右焊盘 → R3 左焊盘
    trace(dt, [(748, 450), (810, 456)], width=4)     # R3 右焊盘 → U2 左脚
    trace(dt, [(910, 456), (996, 450)], width=4)     # U2 右脚 → C4 左焊盘
    trace(dt, [(1044, 450), (1124, 450)], width=4)   # C4 右焊盘 → J2 左沿

    # 反馈 (顶层)
    trace(dt, [(480, 350), (480, 425)], width=3)
    trace(dt, [(580, 350), (580, 425)], width=3)
    trace(dt, [(860, 350), (860, 425)], width=3)
    # 二极管限幅
    trace(dt, [(760, 350), (760, 425)], width=2)
    trace(dt, [(960, 350), (960, 425)], width=2)

    # GND 网络 (底层蓝色)
    trace(dt, [(80, 462), (1140, 462)], width=6, color=BLUE_BOTTOM)
    trace(dt, [(340, 462), (1020, 462)], width=4, color=BLUE_BOTTOM)
    # 旁路电容 (底层连接)
    for x in [380, 580, 760, 960]:
        trace(dt, [(x, 600), (x, 462)], width=3, color=BLUE_BOTTOM)

    # 顶层元件整体压在底层走线之上(见文件头"分层画"那段)
    img = Image.alpha_composite(img.convert("RGBA"), comp).convert("RGB")
    draw_qr(img, "SIG-2026-C3")
    return img, layout


def defect_signal_polarity(base, layout, site):
    """信号板极性反:site 二极管装反"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = site_xy(layout, site)
    # 擦掉原二极管
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 极性条改到左侧(原本在右 = 阴极)
    pad(d, cx-26, cy, size=8)
    pad(d, cx+26, cy, size=8)
    d.rectangle([cx-20, cy-7, cx+20, cy+7], fill=BLACK)
    d.rectangle([cx-19, cy-5, cx-13, cy+5], fill=WHITE)  # 极性条移到左侧 = 反
    d.line([(cx-12, cy),(cx+12, cy)], fill=WHITE, width=1)
    return img, layout


def defect_signal_shifted(base, layout, site):
    """信号板偏移:site 整体平移 20px"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    cx, cy = site_xy(layout, site)
    # 擦掉原电阻
    d.rectangle([cx-30, cy-14, cx+30, cy+14], fill=GREEN_PCB)
    # 偏移后重画
    draw_resistor_small(d, cx+20, cy+8)
    return img, layout


# ============================================================
# 主流程
# ============================================================
#
# 第五列"缺陷画在哪个元件上"是**真值表的一部分**。
#
# 它以前藏在函数体里(每个 defect_* 自己写死坐标),
# 真值表里没有这一列 —— 于是"缺陷画在谁身上"只有代码知道,
# 测试想验"判据点名的元件 == 缺陷真的画在的那个元件",
# 就只能自己再抄一份(抄副本的老坑)。
#
# 现在位号只在这里写一次:
#   画图   -> site_xy() 按位号从 layout 取坐标;
#   真值表 -> complex_manifest.csv 多一列 site,测试从**真值表**读期望。
# ============================================================
REGISTRY = [
    # (kind, name, builder, defect_fn or None, 缺陷画在哪个元件上)
    ("power",  "power_baseline",   build_power_baseline,   None,              ""),
    ("power",  "power_short",      build_power_baseline,   defect_power_short,   "L1"),
    ("power",  "power_missing",    build_power_baseline,   defect_power_missing, "C2"),
    # 焊少:不是"画完再擦",是**画的时候就画小**(见 SHRINK_SPECS)
    ("power",  "power_insufficient", build_power_baseline,  None,             "U1"),
    ("mcu",    "mcu_baseline",     build_mcu_baseline,     None,              ""),
    ("mcu",    "mcu_wrong",        build_mcu_baseline,     defect_mcu_wrong,     "R2"),
    ("mcu",    "mcu_bridge",       build_mcu_baseline,     defect_mcu_bridge,    "Y1"),
    ("mcu",    "mcu_insufficient", build_mcu_baseline,     None,                "U1"),
    # ⚠ 2026-10-21 新增:**两脚**元件(晶振 Y1)的焊少。
    #   之前所有焊少样本都在多脚元件上(TO-220 3 脚 / QFP-64 64 脚 / SOIC-8 8 脚),
    #   而"中位数"这个定义**只在脚数为偶数时才有分歧**:
    #   两脚元件的"同侪"只有一个,取平均 / 取大的 / 取小的 三种写法读数差得最开
    #   (实测 86.1% / 92.6% / 0%)。少了这个样本,中位数的定义就没人管了。
    ("mcu",    "mcu_insufficient_xtal", build_mcu_baseline, None,             "Y1"),
    ("signal", "signal_baseline",  build_signal_baseline,  None,              ""),
    ("signal", "signal_polarity",  build_signal_baseline,  defect_signal_polarity, "D1"),
    ("signal", "signal_shifted",   build_signal_baseline,  defect_signal_shifted,  "R3"),
    ("signal", "signal_insufficient", build_signal_baseline, None,             "U1"),
]

# ============================================================
# 「焊少」缺陷:把某个元件的某个焊盘画小
#
# 焊少是**画的时候**就画小的(见 _SHRINK 那段),不是画完再擦 ——
# 擦会把焊盘底下的走线一起擦断。
#
# 这里登记的是 (第几个端子, 新的半宽)。端子序号来自生成器登记的
# PADS(按画焊盘的顺序);**位号只写一次** —— 在 REGISTRY 那一行里,
# 真值表的 site 列也是从那儿来的。
#
# 选哪个端子、缩到多小,都是**量出来**的(见 notes 里那张实测表):
#   · 优先选**没有金线从上面横穿**的焊盘 —— 有金线压着的焊盘,
#     框里那块"金属"里混了走线的铜,焊盘缩小了读数掉得没那么狠;
#   · 缩小的幅度要保证读数掉过阈值(33%)而且有余量。
#
# ⚠ 2026-10-21 补的:mcu_insufficient_xtal —— 晶振 Y1 的**右边**那个焊盘。
#   晶振只有 2 个脚,两个焊盘一样大(各 336px²,实测),缩一个到 25px² 之后:
#     同侪中位数(取两个的平均) = 180.5  ->  读数 (180.5-25)/180.5 = 86.1%
#     要是中位数取"小的"那个        = 25     ->  读数 0% -> **整条规则漏检**
#   所以这张图是"中位数定义"的守门人,不是凑数的样本。
#
#   ⚠ 缩的是**右边**那个,不是左边:左边那个被 C1 的右焊盘压着 6px
#     (C1 在 530,右焊盘 544~564;Y1 左焊盘 559~579 —— 554 与 569 只差 15px,
#     而焊盘各有 21px 宽,于是重叠)。框里混着 C1 的铜,读数会被稀释:
#     实测缩左边那个时读数只有 **38.0%**(阈值 33,只剩 5 个点的余量),
#     缩右边那个(没人压着)读数 86.1%,余量 53 个点。
#     这条重叠是 mcu 板**原有的**数据毛病(不是这一轮引入的),
#     它今天不影响任何结论(好板两边都量到 336,读数 0%);
#     要根治得挪 C1 的位置,会连带改走线和配方 —— 记在 notes 里,不当这一轮做。
# ============================================================
SHRINK_SPECS = {
    "power_insufficient":     (2, 3),   # TO-220 第三个引脚
    "mcu_insufficient":       (0, 2),   # QFP-64 左边第一个引脚
    "signal_insufficient":    (0, 2),   # SOIC-8 左边第一个引脚
    "mcu_insufficient_xtal":  (1, 2),   # 晶振 Y1 右边那个脚(只有 2 个脚)
}


# 缺陷类型**不在文件名里猜**。
#
# 以前是 name.split("_", 1)[1],那对 "mcu_insufficient_xtal" 是错的:
# 切出来是 "insufficient_xtal",而类型其实还是 insufficient
# (xtal 是**位置**,不是类型)。真值表里类型写错,测试就会去找一个
# 不存在的缺陷类型 —— 而"找不到"和"没查过"在报告里长得一样。
DEFECT_TYPES = ("insufficient", "short", "missing", "wrong", "bridge",
                "polarity", "shifted")


def defect_type_of(name):
    """文件名 -> 缺陷类型(没有缺陷就返回空串)。"""
    tail = name.split("_", 1)[1] if "_" in name else ""
    for t in DEFECT_TYPES:
        if tail == t or tail.startswith(t + "_"):
            return t
    return ""


def main():
    out_dir = Path(__file__).resolve().parent / "images" / "complex"
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"📁 输出目录: {out_dir}")
    print(f"📐 画面尺寸: {W}x{H}\n")

    manifest = []
    summary_by_kind = {}

    for kind, name, builder, defect_fn, site in REGISTRY:
        base, layout = builder()
        if name in SHRINK_SPECS:
            # 焊少:把某个焊盘画小一号,**整块板重画一遍**
            # (第一遍已经把 PADS 登记齐了,第二遍按 _SHRINK 画)。
            _SHRINK.clear()
            idx, new_size = SHRINK_SPECS[name]
            px, py, _pr = PADS[site_xy(layout, site)][idx]
            _SHRINK[(px, py)] = new_size
            img, _ = builder()
            _SHRINK.clear()
            expected = "FAIL:" + defect_type_of(name)
        elif defect_fn is not None:
            img, _ = defect_fn(base, layout, site)
            expected = "FAIL:" + defect_type_of(name)
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
            "site":       site,
        })
        # 按 kind 累加
        if kind not in summary_by_kind:
            summary_by_kind[kind] = {"n": 0, "comp": 0}
        summary_by_kind[kind]["n"] += 1
        summary_by_kind[kind]["comp"] = n_comp  # 同一 kind 元件数相同

        defect_type = defect_type_of(name) or "baseline"
        print(f"  ✅ {path.name:28s}  {n_comp:2d} 元件  ({kind:6s} / {defect_type})")

    # 写 manifest
    csv_path = out_dir / "complex_manifest.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["file", "kind", "components", "size", "filesize_kb", "expected", "site"])
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
