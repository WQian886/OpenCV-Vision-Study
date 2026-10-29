# 生成 CI 变异用的「形状不对」的二维码。
#
# 背景：CI 第 4 步（W5 冒烟）那把**形状**刀，问的是
# 「打出来的号，形状对不对」——PCB-2026-001-SN:A12345X。
#
# 变异 C 要证明这把刀**认得形状**：
# 送进去一张**号是好的、形状是坏的**图（流水号那段两位，格式要三位），
# 真程序照样解出来、照样退 0 ⇒ 只有形状那把刀会响。
#
# 配方跟 W5_OCR_QRCode/gen_qr.py 一样，只改数据：
# 那边的图是**形状对的**那张，这张是**形状不对**的那张。
#
# 二进制进仓库要有第二个来源 —— 这个文件就是那张图的来源。
import qrcode
from pathlib import Path

qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=10, border=4)
qr.add_data("PCB-2026-01-SN:A12345X")
qr.make(fit=True)

img = qr.make_image(fill_color="black", back_color="white")
out = Path(__file__).resolve().parent / "qr_wrongshape.png"
img.save(out)
print("OK 已生成:", out, img.size)
