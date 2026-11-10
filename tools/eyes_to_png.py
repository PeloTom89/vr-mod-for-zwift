"""Turn an eye dump (eyes-HHMMSS-{L,R}-WxH.rgba from Ctrl+Shift+F7) into PNGs for inspection:
  <base>-L.png, <base>-R.png  each eye, downscaled to 1280 wide
  <base>-overlay.png          50/50 blend of L and R: things at the right depth overlap, and the
                              horizontal offset between the two copies is the stereo disparity
  <base>-anaglyph.png         L in red, R in cyan (same idea, easier to see which copy is which)
Stdlib only.  Usage:  python -I tools/eyes_to_png.py <path to ...-L-WxH.rgba>
"""
import re
import struct
import sys
import zlib


def png(path, w, h, rows):  # rows: list of bytes, RGB
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def load(path, w, h, bgra, step):
    data = open(path, "rb").read()
    assert len(data) == w * h * 4, f"{path}: size {len(data)} != {w}x{h}x4"
    rows = []
    for y in range(0, h, step):
        line = data[y * w * 4:(y + 1) * w * 4]
        px = bytearray()
        for x in range(0, w, step):
            r, g, b = line[x * 4], line[x * 4 + 1], line[x * 4 + 2]
            px += bytes((b, g, r) if bgra else (r, g, b))
        rows.append(bytes(px))
    return rows


def main(left):
    m = re.match(r"(.*)-L-(\d+)x(\d+)\.rgba$", left)
    base, w, h = m.group(1), int(m.group(2)), int(m.group(3))
    bgra = "BGRA" in open(base + ".txt").read()
    step = max(1, w // 1280)
    L = load(left, w, h, bgra, step)
    R = load(f"{base}-R-{w}x{h}.rgba", w, h, bgra, step)
    ow, oh = len(L[0]) // 3, len(L)
    png(base + "-L.png", ow, oh, L)
    png(base + "-R.png", ow, oh, R)
    png(base + "-overlay.png", ow, oh, [bytes((a + b) // 2 for a, b in zip(l, r)) for l, r in zip(L, R)])
    ana = []
    for l, r in zip(L, R):
        row = bytearray(len(l))
        for i in range(0, len(l), 3):
            row[i] = (l[i] * 299 + l[i + 1] * 587 + l[i + 2] * 114) // 1000   # L luminance -> red
            gray = (r[i] * 299 + r[i + 1] * 587 + r[i + 2] * 114) // 1000
            row[i + 1] = gray                                                # R luminance -> cyan
            row[i + 2] = gray
        ana.append(bytes(row))
    png(base + "-anaglyph.png", ow, oh, ana)
    print(open(base + ".txt").read())
    print(f"wrote {base}-{{L,R,overlay,anaglyph}}.png ({ow}x{oh})")


if __name__ == "__main__":
    main(sys.argv[1])
