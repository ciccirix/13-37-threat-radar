#!/usr/bin/env python3
# Convert a photo into the T-Watch Meta-detector image format:
#   [w:u16 LE][h:u16 LE][ w*h*2 bytes RGB565 LE ]
# Drop the output on the microSD as /meta/<family>.bin where family is one of:
#   rayban  quest  meta  oakley   (rayban is used for any Luxottica id 0x0D53)
#
# usage: python img_to_metabin.py input.jpg /meta_out/rayban.bin [maxW maxH]
import sys, struct
from PIL import Image

if len(sys.argv) < 3:
    print("usage: img_to_metabin.py input.(jpg|png) output.bin [maxW=380 maxH=300]")
    sys.exit(1)

inp, outp = sys.argv[1], sys.argv[2]
maxw = int(sys.argv[3]) if len(sys.argv) > 3 else 380
maxh = int(sys.argv[4]) if len(sys.argv) > 4 else 300

im = Image.open(inp).convert("RGB")
im.thumbnail((maxw, maxh))
w, h = im.size
px = im.load()

out = bytearray(struct.pack("<HH", w, h))
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

open(outp, "wb").write(out)
print(f"wrote {outp}  {w}x{h}  {len(out)} bytes")
