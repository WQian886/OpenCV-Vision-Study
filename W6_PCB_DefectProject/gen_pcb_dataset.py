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


def draw_ic_pads(d, cx, cy, dx, count=6, pad_size=8):
    """画 IC 多引脚焊盘（左右各 count 个）"""
    spacing = dx * 2 / (count - 1)
    for i in range(count):
        x = int(cx - dx + i * spacing)
        draw_pad(d, x, cy - 30, pad_size)
        draw_pad(d, x, cy + 30, pad_size)


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
        if rotated:
            draw_resistor(d, cx, cy, h, w)   # 反转宽高=180°旋转
        else:
            draw_resistor(d, cx, cy, w, h)
    elif kind == "C":
        draw_two_pads(d, cx - dx, cy, 0)
        draw_two_pads(d, cx + dx, cy, 0)
        if rotated:
            draw_capacitor(d, cx, cy, w, h, polarity_dot=False)
        else:
            draw_capacitor(d, cx, cy, w, h, polarity_dot=True)
    elif kind == "IC":
        draw_ic_pads(d, cx, cy, dx, count=6)
        if rotated:
            draw_ic(d, cx, cy, w, h, pin_dot_at=None)
        else:
            draw_ic(d, cx, cy, w, h, pin_dot_at=(cx - dx + 6, cy - 30))


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
def defect_missing(base: Image.Image) -> Image.Image:
    """缺件：把 R2 整个擦掉（焊盘保留,本体不见）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[3]   # R2
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 焊盘保留
    draw_two_pads(d, cx - dx, cy, 0)
    draw_two_pads(d, cx + dx, cy, 0)
    return img


def defect_wrong(base: Image.Image) -> Image.Image:
    """错件：把 R3 换成 C（应该装电阻的位子装了电容）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[5]   # R3
    # 擦掉原 R3
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 画一个电容（错的）
    draw_capacitor(d, cx, cy, w, h, color=YELLOW, polarity_dot=True)
    return img


def defect_polarity(base: Image.Image) -> Image.Image:
    """极性反：把 C1 旋转 180°（极性条从左换到右）"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[1]   # C1
    # 擦掉原 C1
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


def defect_bridge(base: Image.Image) -> Image.Image:
    """焊桥：把 R1 的两个焊盘用金色长条连起来"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[0]   # R1
    # 在两个焊盘之间画一条金色横条
    d.rectangle([cx - dx - 12, cy - 8, cx + dx + 12, cy + 8],
                fill=GOLD, outline=GOLD_DARK)
    return img


def defect_insufficient(base: Image.Image) -> Image.Image:
    """焊少：把 U1 的某个焊盘擦掉一大块"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[2]   # U1
    # 把最左侧第二个焊盘擦掉（缩成小点）
    pad_x = int(cx - dx)
    # 擦
    d.rectangle([pad_x - 12, cy - 30 - 12, pad_x + 12, cy - 30 + 12],
                fill=GREEN_PCB)
    # 画一个小得多的焊盘（面积 < 阈值）
    draw_pad(d, pad_x, cy - 30, size=4, color=GOLD)
    return img


def defect_shifted(base: Image.Image) -> Image.Image:
    """偏移：把 R2 整体平移 30px,脱离焊盘中心"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    label, kind, cx, cy, dx, _dy, w, h = SITES[3]   # R2
    # 擦掉原 R2
    x0, y0 = cx - w//2 - 4, cy - h//2 - 4
    x1, y1 = cx + w//2 + 4, cy + h//2 + 4
    d.rectangle([x0, y0, x1, y1], fill=GREEN_PCB)
    # 偏移 30px 重新画
    draw_resistor(d, cx + 30, cy + 10, w, h)
    return img


def defect_short(base: Image.Image) -> Image.Image:
    """短路：在底部走线之间画一条多余的铜桥"""
    img = base.copy()
    d = ImageDraw.Draw(img)
    # 在 (382, 480) 与 (740, 480) 之间拉一条横向短路桥
    # 但走线 (418,480)-(664,480) 是断开的,正好可以在中间补一段
    d.rectangle([418, 477, 664, 483], fill=GOLD, outline=GOLD_DARK)
    # 标个红色圈便于人眼看出（检测算法不依赖这个）
    d.ellipse([520, 460, 580, 500], outline=RED, width=2)
    return img


# ============================================================
# 6. 主流程
# ============================================================
DEFECT_REGISTRY = [
    ("baseline",       None,                "ok"),
    ("missing",        defect_missing,      "FAIL:missing"),
    ("wrong",          defect_wrong,        "FAIL:wrong"),
    ("polarity",       defect_polarity,     "FAIL:polarity"),
    ("bridge",         defect_bridge,       "FAIL:bridge"),
    ("insufficient",   defect_insufficient, "FAIL:insufficient"),
    ("shifted",        defect_shifted,      "FAIL:shifted"),
    ("short",          defect_short,        "FAIL:short"),
]


def main():
    out_dir = Path(__file__).resolve().parent / "images"
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"📁 输出目录: {out_dir}")
    base = build_baseline()
    manifest = []

    for name, fn, expected in DEFECT_REGISTRY:
        if fn is None:
            img = base
        else:
            img = fn(base)
        # 注意:img 是 RGB,但 OpenCV 读出来是 BGR,这里我们直接 RGB 保存
        # (W6 main.cpp 里用 cv::imread(path, IMREAD_COLOR) 读 BGR,色彩对应一致)
        path = out_dir / f"{name}.png"
        img.save(path)
        print(f"  ✅ {path.name}  ({img.size[0]}x{img.size[1]})")
        manifest.append({
            "file":     path.name,
            "defect":   "none" if expected == "ok" else expected.split(":", 1)[1],
            "expected": "PASS" if expected == "ok" else "FAIL",
            "note":     "baseline reference" if expected == "ok" else f"synthetic defect: {expected.split(':', 1)[1]}",
        })

    # 写 manifest.csv
    csv_path = out_dir / "manifest.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["file", "defect", "expected", "note"])
        w.writeheader()
        for row in manifest:
            w.writerow(row)
    print(f"  ✅ manifest.csv  ({len(manifest)} 行)")

    print("\n✨ 完成。8 张图 + 1 个 manifest.csv 已生成。")
    print("👉 下一步:VS 打开 W6_PCB_DefectProject → CMake 配置 → F7 编译 → Ctrl+F5 跑 inspect")


if __name__ == "__main__":
    main()
