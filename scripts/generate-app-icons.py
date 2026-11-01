#!/usr/bin/env python3
"""Generate Windows/macOS raster assets from the single HopDesk icon design."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
out = root / "assets/appicon"
out.mkdir(parents=True, exist_ok=True)
size = 1024
image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
pixels = image.load()
# Blue-to-violet diagonal gradient, clipped to a rounded-square app tile.
for y in range(size):
    for x in range(size):
        t = max(0.0, min(1.0, (x * 0.34 + y * 0.66) / (size - 1)))
        pixels[x, y] = (
            round(38 + (113 - 38) * t),
            round(120 + (63 - 120) * t),
            round(245 + (227 - 245) * t),
            255,
        )
mask = Image.new("L", (size, size), 0)
ImageDraw.Draw(mask).rounded_rectangle((64, 64, 960, 960), radius=224, fill=255)
image.putalpha(mask)
draw = ImageDraw.Draw(image)
# A clear H/bridge motif links two device endpoints.
draw.line([(300, 310), (300, 714)], fill=(255, 255, 255, 255), width=104)
draw.line([(724, 310), (724, 714)], fill=(220, 232, 255, 255), width=104)
draw.line([(300, 512), (724, 512)], fill=(245, 249, 255, 255), width=104)
r = 72
for cx, cy, fill in [
    (300, 270, (255, 255, 255, 255)),
    (724, 270, (255, 255, 255, 255)),
    (300, 754, (220, 232, 255, 255)),
    (724, 754, (220, 232, 255, 255)),
]:
    draw.ellipse((cx-r, cy-r, cx+r, cy+r), fill=fill)
png = out / "hopdesk.png"
ico = out / "hopdesk.ico"
image.save(png, optimize=True)
image.save(ico, format="ICO", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print(f"Generated {png} and {ico} from the HopDesk master design.")
