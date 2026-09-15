import qrcode

qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=10, border=4)
qr.add_data("PCB-2026-001-SN:A12345X")
qr.make(fit=True)

img = qr.make_image(fill_color="black", back_color="white")
img.save(r"E:\qianwie\00study\OpenCV\W5_OCR_QRCode\images\qrcode_test.png")
print("OK 已生成:", img.size)