"""生成 HardwareMonitor 图标：512px PNG 源图 + 多尺寸 ICO。

开发用，需要 Pillow：
    D:\\My_programs\\.venv\\Scripts\\python.exe tools\\make_icon.py

产出（项目根目录）：
    HardwareMonitor.png   512×512 源图
    HardwareMonitor.ico   16/24/32/48/64/128/256 多尺寸，供 app.rc 嵌入
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, "HardwareMonitor.png")
OUT_ICO = os.path.join(ROOT, "HardwareMonitor.ico")

S = 512

BG    = (0x2A, 0x30, 0x40, 255)   # 深蓝灰底
GLASS = (0xF2, 0xF4, 0xF7, 255)   # 温度计玻璃
MERC  = (0xE2, 0x4B, 0x4A, 255)   # 水银（红）
TICK  = (0x76, 0x81, 0x96, 255)   # 刻度线

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# ── 圆角底 ────────────────────────────────────────────────
d.rounded_rectangle([0, 0, S - 1, S - 1], radius=112, fill=BG)

# ── 温度计几何 ────────────────────────────────────────────
cx        = 244          # 管体中心（右侧留给刻度）
tube_w    = 68
tube_top  = 112
tube_bot  = 330
bulb_r    = 80
bulb_cy   = 320          # 使内容上下留白均为 112px，视觉居中

# 玻璃外壳：管 + 球泡
d.rounded_rectangle([cx - tube_w // 2, tube_top, cx + tube_w // 2, tube_bot],
                    radius=tube_w // 2, fill=GLASS)
d.ellipse([cx - bulb_r, bulb_cy - bulb_r, cx + bulb_r, bulb_cy + bulb_r], fill=GLASS)

# ── 水银柱 ────────────────────────────────────────────────
inner_w = 34
mer_top = 168
d.rounded_rectangle([cx - inner_w // 2, mer_top, cx + inner_w // 2, tube_bot],
                    radius=inner_w // 2, fill=MERC)
inner_r = bulb_r - 18
d.ellipse([cx - inner_r, bulb_cy - inner_r, cx + inner_r, bulb_cy + inner_r], fill=MERC)

# ── 右侧刻度（三条，够 16px 下辨认） ──────────────────────
x0 = cx + tube_w // 2 + 18
for y in (176, 224, 272):
    d.rounded_rectangle([x0, y, x0 + 46, y + 12], radius=6, fill=TICK)

img.save(OUT_PNG)

img.save(OUT_ICO, sizes=[(16, 16), (24, 24), (32, 32),
                         (48, 48), (64, 64), (128, 128), (256, 256)])

print("PNG ->", OUT_PNG, img.size)
print("ICO ->", OUT_ICO, os.path.getsize(OUT_ICO), "bytes")
