#!/usr/bin/env python3
import binascii
import pathlib
import struct
import sys
import zlib


def png_chunk(kind, data):
    crc = binascii.crc32(kind + data) & 0xffffffff
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def make_png():
    signature = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    raw = b"\x00\xff\x00\x00"
    return signature + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b"")


def main():
    root = pathlib.Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    jpeg = b"\xff\xd8\xff\xe0\x00\x02\xff\xd9"
    pdf = b"%PDF-1.4\n1 0 obj\n<<>>\nendobj\nxref\nstartxref\n0\n%%EOF\n"
    png = make_png()
    mixed = b"noise" + jpeg + b"gap" + png + b"gap" + pdf
    (root / "mixed.img").write_bytes(mixed)
    boundary = bytearray(b"x" * (1024 * 1024 - 2))
    boundary.extend(jpeg)
    (root / "boundary.img").write_bytes(boundary)
    (root / "empty.img").write_bytes(b"")
    (root / "truncated.img").write_bytes(b"pad\xff\xd8\xff\xe0\x00\x02")
    corrupt_png = bytearray(png)
    corrupt_png[-1] ^= 0xff
    (root / "corrupt.img").write_bytes(corrupt_png)
    (root / "not-a-directory").write_bytes(b"x")
    embedded_pdf = (b"%PDF-1.7\n1 0 obj\nstream\n" + jpeg +
                    b"\nendstream\nendobj\nxref\nstartxref\n0\n%%EOF\n"
                    b"2 0 obj\n<<>>\nendobj\nstartxref\n0\n%%EOF\n")
    (root / "embedded.img").write_bytes(embedded_pdf)
    (root / "adjacent.img").write_bytes(jpeg + png)

    ntfs = bytearray(4096)
    ntfs[3:11] = b"NTFS    "
    struct.pack_into("<H", ntfs, 11, 512)
    ntfs[13] = 1
    struct.pack_into("<Q", ntfs, 40, 8)
    struct.pack_into("<Q", ntfs, 48, 1)
    ntfs[64] = 0xf6  # -10 => 1024-byte MFT records
    ntfs[510:512] = b"\x55\xaa"

    active = memoryview(ntfs)[512:1536]
    active[0:4] = b"FILE"
    struct.pack_into("<H", active, 22, 1)

    deleted = memoryview(ntfs)[1536:2560]
    deleted[0:4] = b"FILE"
    struct.pack_into("<HH", deleted, 4, 0x30, 3)
    struct.pack_into("<H", deleted, 20, 0x38)
    struct.pack_into("<H", deleted, 22, 0)
    struct.pack_into("<I", deleted, 28, 1024)
    deleted[0x30:0x36] = b"\xaa\xbb\x11\x22\x33\x44"

    name = "lost.jpg".encode("utf-16le")
    name_value = bytearray(66 + len(name))
    name_value[64] = len(name) // 2
    name_value[65] = 1
    name_value[66:] = name
    name_attr_length = (24 + len(name_value) + 7) & ~7
    name_pos = 0x38
    struct.pack_into("<II", deleted, name_pos, 0x30, name_attr_length)
    struct.pack_into("<IH", deleted, name_pos + 16, len(name_value), 24)
    deleted[name_pos + 24:name_pos + 24 + len(name_value)] = name_value

    data_pos = name_pos + name_attr_length
    struct.pack_into("<II", deleted, data_pos, 0x80, 32)
    struct.pack_into("<IH", deleted, data_pos + 16, len(jpeg), 24)
    deleted[data_pos + 24:data_pos + 24 + len(jpeg)] = jpeg
    struct.pack_into("<I", deleted, data_pos + 32, 0xffffffff)
    struct.pack_into("<I", deleted, 24, data_pos + 36)
    deleted[510:512] = b"\xaa\xbb"
    deleted[1022:1024] = b"\xaa\xbb"
    (root / "resident.ntfs").write_bytes(ntfs)


if __name__ == "__main__":
    main()
