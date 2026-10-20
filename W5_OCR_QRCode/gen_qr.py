import qrcode
from pathlib import Path

qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=10, border=4)
qr.add_data("PCB-2026-001-SN:A12345X")
qr.make(fit=True)

img = qr.make_image(fill_color="black", back_color="white")
# 相对脚本自身定位输出目录,不写死盘符 —— 仓库换个位置照样能生成。
out = Path(__file__).resolve().parent / "images" / "qrcode_test.png"
img.save(out)
print("OK 已生成:", out, img.size)