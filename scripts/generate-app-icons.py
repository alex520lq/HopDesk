#!/usr/bin/env python3
"""Generate all raster icon formats from the single HopDesk SVG master."""
from pathlib import Path
import sys
import cairosvg
from PIL import Image

root = Path(__file__).resolve().parents[1]
svg = root / "assets/appicon/hopdesk.svg"
out = root / "assets/appicon"
png = out / "hopdesk.png"
ico = out / "hopdesk.ico"
cairosvg.svg2png(url=str(svg), write_to=str(png), output_width=1024, output_height=1024)
image = Image.open(png).convert("RGBA")
image.save(ico, format="ICO", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print(f"Generated {png} and {ico}")
