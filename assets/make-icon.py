"""Writes assets/encodeall.ico: white "EA" in a pixel font on black, square, monochrome like the UI.
Run: py -I assets/make-icon.py"""
import os, struct, zlib
GLYPHS = {
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
}
def render(size):
    px = [[(0, 0, 0, 255)] * size for _ in range(size)]
    text = "EA"
    cols = len(text) * 5 + (len(text) - 1)  # glyph width 5, gap 1
    cell = max(1, (size * 3 // 4) // cols)
    w, h = cols * cell, 7 * cell
    ox, oy = (size - w) // 2, (size - h) // 2
    for gi, ch in enumerate(text):
        for r, row in enumerate(GLYPHS[ch]):
            for c, bit in enumerate(row):
                if bit == "1":
                    for y in range(cell):
                        for x in range(cell):
                            px[oy + r * cell + y][ox + (gi * 6 + c) * cell + x] = (255, 255, 255, 255)
    return px
def png(px):
    size = len(px)
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in px)
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
sizes = [16, 24, 32, 48, 64, 256]
images = [png(render(s)) for s in sizes]
out = struct.pack("<HHH", 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
for s, img in zip(sizes, images):
    out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
    offset += len(img)
out += b"".join(images)
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "encodeall.ico")
open(path, "wb").write(out)
print(path, len(out))
