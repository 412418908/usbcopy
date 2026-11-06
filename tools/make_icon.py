#!/usr/bin/env python3
"""Generate app.ico (USB flash drive icon) for usbcopy / 7z.exe."""
from PIL import Image, ImageDraw

S = 1024  # master canvas (downscaled with LANCZOS for smooth edges)

WHITE = (255, 255, 255, 255)
SILVER = (206, 212, 221, 255)
SILVER_DK = (138, 146, 159, 255)
HOLE = (88, 95, 108, 255)
OUTLINE = (22, 48, 108, 255)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# ---- connector (USB-A plug) on top ----
d.rounded_rectangle((362, 60, 662, 340), radius=28, fill=SILVER,
                    outline=SILVER_DK, width=10)
# two contact slots
d.rounded_rectangle((408, 118, 452, 166), radius=10, fill=HOLE)
d.rounded_rectangle((572, 118, 616, 166), radius=10, fill=HOLE)

# ---- body with vertical blue gradient ----
bx0, by0, bx1, by1 = 292, 330, 732, 910
body_mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(body_mask)
md.rounded_rectangle((bx0, by0, bx1, by1), radius=64, fill=255)

grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(grad)
top = (72, 138, 245)      # lighter blue
bot = (24, 72, 190)       # deeper blue
for y in range(by0, by1 + 1):
    t = (y - by0) / (by1 - by0)
    c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,)
    gd.line([(0, y), (S, y)], fill=c)
img.paste(grad, (0, 0), body_mask)

# body outline + glass highlight
d.rounded_rectangle((bx0, by0, bx1, by1), radius=64, outline=OUTLINE, width=10)
d.rounded_rectangle((bx0 + 26, by0 + 18, bx1 - 26, by0 + 96), radius=40,
                    fill=(255, 255, 255, 46))

# ---- USB trident symbol (white) ----
cx = 512
LW = 26  # stroke width


def dot(x, y, r=LW // 2):
    d.ellipse((x - r, y - r, x + r, y + r), fill=WHITE)


# center stem + arrow head
d.line([(cx, 668), (cx, 505)], fill=WHITE, width=LW)
dot(cx, 668)
d.polygon([(cx, 435), (cx - 36, 505), (cx + 36, 505)], fill=WHITE)
# left / right branches ending in square pads
for ex in (392, 632):
    d.line([(cx, 566), (ex, 566)], fill=WHITE, width=LW)
    dot(cx, 566)
    d.rounded_rectangle((ex - 24, 566 - 23, ex + 24, 566 + 23), radius=10, fill=WHITE)
# circle at the bottom of the trident
d.ellipse((cx - 52, 700, cx + 52, 804), outline=WHITE, width=LW)

# ---- export ----
master = img.resize((256, 256), Image.LANCZOS)
sizes = [(256, 256), (64, 64), (48, 48), (32, 32), (16, 16)]
master.save("/workspace/app.ico", format="ICO", sizes=sizes)
print("app.ico written")
