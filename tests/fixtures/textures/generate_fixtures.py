# Generates the PNG fixtures for the texture importer tests (US-021) with the standard library only, so the bytes are
# reproducible. The JPEG fixture comes from generate_jpeg.ps1, which uses Windows' System.Drawing encoder.
#
# Run from this directory:  python3 generate_fixtures.py
import struct
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def png(width: int, height: int, bit_depth: int, color_type: int, rows: list) -> bytes:
    # Each row is filtered with type 0 (none), so the rows are stored as they are.
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    header = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def srgb_2x2() -> bytes:
    # Columns alternate black and white. The mip-1 texel is the average of four texels: linear light gives 188 in sRGB,
    # while averaging the encoded values gives 128.
    black, white = (0, 0, 0), (255, 255, 255)
    rows = [[*black, *white], [*black, *white]]
    return png(2, 2, 8, 2, rows)


def normal_5x3() -> bytes:
    # A flat normal map with an odd size. One texel holds (1, 1, 1), which is not unit length and must be renormalized.
    flat = (128, 128, 255)
    rows = []
    for y in range(3):
        row = []
        for x in range(5):
            texel = (255, 255, 255) if (x, y) == (4, 2) else flat
            row.extend(texel)
        rows.append(row)
    return png(5, 3, 8, 2, rows)


def gradient_4x4_rgba() -> bytes:
    # A 4x4 color image with a full-coverage alpha channel. It is one BC7 block. The colors run along a line in RGB, which
    # two endpoints can represent, so the round trip is close; a two-dimensional gradient is not a line and is not used.
    rows = []
    for y in range(4):
        row = []
        for x in range(4):
            row.extend((x * 85, 255 - x * 85, 128, 255))
        rows.append(row)
    return png(4, 4, 8, 6, rows)


def sixteen_bit_2x2() -> bytes:
    # 16-bit RGB. The importer keeps the high byte of each sample and reports the reduction.
    high = 0xFFFF
    rows = [
        list(struct.pack(">HHHHHH", 0, 0x8000, high, 0, high, 0x8000)),
        list(struct.pack(">HHHHHH", 0x8000, 0x8000, 0x8000, high, 0, high)),
    ]
    return png(2, 2, 16, 2, rows)


FIXTURES = {
    "srgb_2x2.png": srgb_2x2(),
    "normal_5x3.png": normal_5x3(),
    "gradient_4x4.png": gradient_4x4_rgba(),
    "sixteen_bit_2x2.png": sixteen_bit_2x2(),
}

if __name__ == "__main__":
    for name, data in FIXTURES.items():
        with open(name, "wb") as out:
            out.write(data)
        print(f"wrote {name} ({len(data)} bytes)")
