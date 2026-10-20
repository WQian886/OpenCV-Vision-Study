from PIL import Image, ImageDraw, ImageFont
import os
from pathlib import Path

# 白色背景 PCB 风格图
W, H = 600, 200
img = Image.new('RGB', (W, H), color='white')
draw = ImageDraw.Draw(img)

# 用系统字体(Win 默认 Arial)
try:
    font = ImageFont.truetype("C:\\Windows\\Fonts\\arial.ttf", 36)
except Exception:
    font = ImageFont.load_default()

# PCB 元件丝印风格的文字
draw.text((20, 20),  "R123  100R",  fill='black', font=font)
draw.text((20, 70),  "C456  10uF",   fill='black', font=font)
draw.text((20, 120), "PCB-2026-001",  fill='black', font=font)

out = Path(__file__).resolve().parent / "images" / "ocr_test.png"
img.save(out)
print("OK 已生成:", out, img.size)