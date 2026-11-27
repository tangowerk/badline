# Badline placeholder icon: C64 screen with raster bars, drawn on a 32x32 pixel grid (Pepto palette)
import sys
from PIL import Image
out = sys.argv[1]
P = {"lb": (108, 94, 181), "b": (53, 40, 121), "bk": (0, 0, 0), "w": (255, 255, 255),
     "y": (184, 199, 111), "o": (111, 79, 37), "r": (104, 55, 43), "lr": (154, 103, 89),
     "c": (112, 164, 178), "g": (88, 141, 67), "lg": (154, 210, 132), "gy": (149, 149, 149)}
N = 32
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
px = img.load()
def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[x, y] = P[c] + (255,)
# monitor body with rounded corners
rect(1, 2, 31, 30, "lb")
for x, y in [(1, 2), (30, 2), (1, 29), (30, 29)]:
    px[x, y] = (0, 0, 0, 0)
# screen
rect(4, 5, 28, 27, "b")
# raster bars across the full width of the screen (the "bad lines")
bars = ["r", "lr", "o", "y", "w", "y", "o", "lr", "r"]
for i, c in enumerate(bars):
    rect(4, 11 + i, 28, 12 + i, c)
# cursor block and a "READY." hint
rect(6, 7, 8, 9, "lb")
rect(6, 22, 8, 24, "lb")
img.save(out + "_32.png")
for size in (48, 256, 1024):
    img.resize((size, size), Image.NEAREST).save(out + ("" if size == 256 else f"_{size}") + ".png")
