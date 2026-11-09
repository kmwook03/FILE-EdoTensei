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


def make_jpeg(size):
    return b"\xff\xd8\xff\xe0" + struct.pack(">H", size - 6) + b"x" * (size - 8) + b"\xff\xd9"


def resident_attribute(payload, attribute_type=0x80):
    attribute = bytearray((24 + len(payload) + 7) & ~7)
    struct.pack_into("<II", attribute, 0, attribute_type, len(attribute))
    struct.pack_into("<IH", attribute, 16, len(payload), 24)
    attribute[24:24 + len(payload)] = payload
    return attribute


def nonresident_attribute(logical_size, runs):
    encoded = bytearray()
    previous = 0
    for lcn, clusters in runs:
        if lcn is None:
            encoded.extend((0x01, clusters))
        else:
            encoded.extend((0x11, clusters, (lcn - previous) & 0xff))
            previous = lcn
    encoded.append(0)
    attribute = bytearray((64 + len(encoded) + 7) & ~7)
    struct.pack_into("<II", attribute, 0, 0x80, len(attribute))
    attribute[8] = 1
    struct.pack_into("<QQH", attribute, 16, 0, sum(length for _, length in runs) - 1, 64)
    allocated = sum(length for lcn, length in runs if lcn is not None) * 512
    struct.pack_into("<QQQ", attribute, 40, allocated, logical_size, logical_size)
    attribute[64:64 + len(encoded)] = encoded
    return attribute


def mft_record(attributes, active=False):
    record = bytearray(1024)
    record[0:4] = b"FILE"
    struct.pack_into("<HH", record, 4, 0x30, 3)
    struct.pack_into("<HH", record, 20, 0x38, int(active))
    struct.pack_into("<I", record, 28, len(record))
    position = 0x38
    for attribute in attributes:
        record[position:position + len(attribute)] = attribute
        position += len(attribute)
    struct.pack_into("<I", record, position, 0xffffffff)
    struct.pack_into("<I", record, 24, position + 4)
    record[0x30:0x32] = b"\xaa\xbb"
    for sector in range(2):
        tail = (sector + 1) * 512 - 2
        record[0x32 + sector * 2:0x34 + sector * 2] = record[tail:tail + 2]
        record[tail:tail + 2] = b"\xaa\xbb"
    return record


def ntfs_image(size=8192):
    image = bytearray(size)
    image[3:11] = b"NTFS    "
    struct.pack_into("<H", image, 11, 512)
    image[13] = 1
    struct.pack_into("<Q", image, 40, len(image) // 512)
    struct.pack_into("<Q", image, 48, 1)
    image[64] = 0xf6
    image[510:512] = b"\x55\xaa"
    image[512:1536] = mft_record([], active=True)
    return image


def p0_fixtures(root):
    expected = root / "expected"
    for name, runs, gap_offset in (
        ("fragmented", [(8, 1), (12, 1)], 4608),
        ("reversed", [(12, 1), (8, 1)], 6656),
        ("sparse", [(8, 1), (None, 1), (12, 1)], None),
    ):
        payload = bytearray(make_jpeg(sum(length for _, length in runs) * 512))
        image = ntfs_image()
        position = 0
        for lcn, length in runs:
            amount = length * 512
            if lcn is None:
                payload[position:position + amount] = bytes(amount)
            else:
                image[lcn * 512:lcn * 512 + amount] = payload[position:position + amount]
            position += amount
        image[1536:2560] = mft_record([nonresident_attribute(len(payload), runs)])
        if gap_offset is not None:
            image[gap_offset:gap_offset + 8] = make_jpeg(8)
        (root / f"{name}.ntfs").write_bytes(image)
        (expected / f"{name}.jpg").write_bytes(payload)

    payload = make_jpeg(600)
    image = ntfs_image()
    image[512:1536] = mft_record([nonresident_attribute(2048, [(1, 3), (8, 1)])], active=True)
    record = mft_record([resident_attribute(payload)])
    image[1536:2048] = record[:512]
    image[4096:4608] = record[512:]
    (root / "fragmented-mft.ntfs").write_bytes(image)
    (expected / "fragmented-mft.jpg").write_bytes(payload)


def main():
    root = pathlib.Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    jpeg = b"\xff\xd8\xff\xe0\x00\x02\xff\xd9"
    pdf = b"%PDF-1.4\n1 0 obj\n<<>>\nendobj\nxref\nstartxref\n0\n%%EOF\n"
    png = make_png()
    expected = root / "expected"
    expected.mkdir(exist_ok=True)
    for name, payload in (("original.jpg", jpeg), ("original.png", png), ("original.pdf", pdf)):
        (expected / name).write_bytes(payload)
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

    ntfs = ntfs_image(4096)
    name = "lost.jpg".encode("utf-16le")
    name_value = bytearray(66 + len(name))
    name_value[64] = len(name) // 2
    name_value[65] = 1
    name_value[66:] = name
    ntfs[1536:2560] = mft_record([resident_attribute(name_value, 0x30), resident_attribute(jpeg)])
    (root / "resident.ntfs").write_bytes(ntfs)
    p0_fixtures(root)


if __name__ == "__main__":
    main()
