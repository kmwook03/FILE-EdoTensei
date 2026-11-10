#!/usr/bin/env python3
"""Independent ZIP writers, corruptions and SHA-256 recovery checks for P1."""
import collections
import hashlib
import io
import json
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

from generate_fixtures import mft_record, ntfs_image, resident_attribute, nonresident_attribute


def number(value, width):
    return value.to_bytes(width, "little")


def archive(entries, *, wide=False, comment=b"", reverse_directory=False):
    """Write ZIP records without depending on FILEEdo or Python's ZIP writer."""
    local = bytearray()
    directory = []
    for item in entries:
        name = item.get("name", b"entry")
        data = item.get("data", b"hello" * 100)
        method = item.get("method", 0)
        descriptor = item.get("descriptor", False)
        signed = item.get("signed", True)
        known = item.get("known", False)
        compressor = zlib.compressobj(wbits=-15)
        packed = data if method == 0 else compressor.compress(data) + compressor.flush()
        packed += item.get("trailing", b"")
        crc = zlib.crc32(data)
        flags = 8 if descriptor else 0
        version = 45 if wide else 20
        offset = len(local)
        local_sizes = (len(data), len(packed)) if not descriptor or known else (0, 0)
        extra = b""
        if wide:
            extra = struct.pack("<HHQQ", 1, 16, *local_sizes)
        local += struct.pack("<IHHHHHIIIHH", 0x04034B50, version, flags, method, 0, 0,
                             crc if not descriptor or known else 0,
                             0xFFFFFFFF if wide else local_sizes[1],
                             0xFFFFFFFF if wide else local_sizes[0], len(name), len(extra))
        local += name + extra + packed
        if descriptor:
            if signed:
                local += b"PK\x07\x08"
            local += number(crc, 4) + number(len(packed), 8 if wide else 4) + number(len(data), 8 if wide else 4)
        extra = struct.pack("<HHQQQ", 1, 24, len(data), len(packed), offset) if wide else b""
        directory.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, version, version,
                                     flags, method, 0, 0, crc,
                                     0xFFFFFFFF if wide else len(packed),
                                     0xFFFFFFFF if wide else len(data), len(name), len(extra), 0,
                                     0, 0, 0, 0xFFFFFFFF if wide else offset) + name + extra)
    directory_offset = len(local)
    local += b"".join(reversed(directory) if reverse_directory else directory)
    directory_size = len(local) - directory_offset
    if wide:
        zip64_offset = len(local)
        local += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0,
                             len(entries), len(entries), directory_size, directory_offset)
        local += struct.pack("<IIQI", 0x07064B50, 0, zip64_offset, 1)
    local += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0,
                         0xFFFF if wide else len(entries), 0xFFFF if wide else len(entries),
                         0xFFFFFFFF if wide else directory_size,
                         0xFFFFFFFF if wide else directory_offset, len(comment)) + comment
    return bytes(local)


def digest(path):
    with path.open("rb") as source:
        result = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
        return result.hexdigest()


def changed(data, offset, value, width):
    result = bytearray(data)
    result[offset:offset + width] = number(value, width)
    return bytes(result)


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="fileedo-p1-") as temporary:
        root = pathlib.Path(temporary)

        def carve(name, image, expected=(), options=(), code=0, reason=None, mode="raw"):
            source = root / f"{name}.img"
            source.write_bytes(image)
            before = digest(source)
            output = root / f"{name}-out"
            report = root / f"{name}.jsonl"
            completed = subprocess.run([program, "--mode", mode, "--output", str(output),
                                        "--report", str(report), *options, str(source)],
                                       capture_output=True, text=True)
            assert completed.returncode == code, (name, completed.stdout, completed.stderr)
            assert digest(source) == before, name
            records = [json.loads(line) for line in report.read_text().splitlines()]
            recovered = list(output.iterdir()) if output.exists() else []
            assert collections.Counter(digest(path) for path in recovered) == collections.Counter(
                hashlib.sha256(data).hexdigest() for data in expected), name
            accepted = [record for record in records[:-1] if record["accepted"]]
            assert len(accepted) == records[-1]["accepted"] == len(expected), name
            assert records[-1]["complete"] == (code == 0), name
            assert records[-1]["errors"] == 0, name
            for record in accepted:
                assert record["validation"] == "valid" and record["format"] == "zip", name
                assert record["byte_count"] == record["validated_length"], name
                assert not record["skipped"] and record["stop_reason"] == "", name
                assert record["output_path"].endswith(".zip"), name
            if reason:
                assert any(reason in warning for record in records[:-1] for warning in record["warnings"]), (name, reason)
            return records

        rng = random.Random(1907)
        payload = rng.randbytes(140000)
        entries = [{"name": b"a.bin", "data": payload},
                   {"name": b"nested/b.txt", "data": b"content" * 200, "method": 8},
                   {"name": b"empty", "data": b""}]
        mixed = archive(entries, comment=b"comment including PK\x05\x06, not an end record")
        carve("mixed", b"prefix" + mixed + b"trailing bytes", [mixed])
        reordered = archive(entries, reverse_directory=True)
        carve("directory-order", reordered, [reordered])
        empty = archive([])
        carve("empty", empty, [empty])
        empty64 = archive([], wide=True)
        carve("empty-zip64", empty64, [empty64])

        # Both mainstream seekable and non-seekable ZIP writers are independent oracles.
        class Pipe(io.BytesIO):
            def seekable(self):
                return False

            def seek(self, *args):
                raise io.UnsupportedOperation("pipe")

        for streaming in (False, True):
            target = Pipe() if streaming else io.BytesIO()
            with zipfile.ZipFile(target, "w") as writer:
                writer.writestr("stored", payload, compress_type=zipfile.ZIP_STORED)
                writer.writestr("deflated", payload, compress_type=zipfile.ZIP_DEFLATED)
                with writer.open("forced-zip64", "w", force_zip64=True) as entry:
                    entry.write(b"zip64 in a local header only")
            content = target.getvalue()
            carve(f"python-stream-{streaming}", content, [content])

        for wide in (False, True):
            for method in (0, 8):
                for signed in (False, True):
                    for known in (False, True):
                        content = archive([{"data": payload, "method": method, "descriptor": True,
                                            "signed": signed, "known": known}], wide=wide, comment=b"comment")
                        with zipfile.ZipFile(io.BytesIO(content)) as reader:
                            assert reader.read("entry") == payload
                        carve(f"descriptor-{wide}-{method}-{signed}-{known}", content, [content])
                content = archive([{"data": payload, "method": method}], wide=wide)
                carve(f"size-{wide}-{method}", content, [content])
                content = archive([{"data": b"", "method": method, "descriptor": True}], wide=wide)
                carve(f"empty-entry-{wide}-{method}", content, [content])

        # Store descriptor and signature positions cross the validator and scanner windows.
        for length in (65511, 65535, 65536, 65537):
            content = archive([{"data": b"PK\x03\x04garbage" + b"a" * length, "descriptor": True,
                                "signed": length % 2 == 0}])
            carve(f"store-boundary-{length}", content, [content])
        small = archive([{"data": b"small"}])
        carve("scanner-boundary", b"\0" * (1024 * 1024 - 2) + small, [small])
        carve("adjacent", small + small, [small, small])

        # NTFS metadata candidates must reach the same registry and ZIP validator.
        ntfs = ntfs_image()
        ntfs[1536:2560] = mft_record([resident_attribute(small)])
        records = carve("resident", ntfs, [small], mode="hybrid")
        assert any(r.get("accepted") and r["method"] == "ntfs_metadata" for r in records)
        split = archive([{"data": b"large data" * 100}])
        ntfs = ntfs_image()
        ntfs[4096:4608] = split[:512]
        ntfs[3072:3072 + len(split) - 512] = split[512:]
        ntfs[1536:2560] = mft_record([nonresident_attribute(len(split), [(8, 1), (6, 2)])])
        carve("metadata-extents", ntfs, [split], mode="ntfs")

        base = archive([{"data": b"unchanged payload"}])
        central = base.index(b"PK\x01\x02")
        end = base.index(b"PK\x05\x06")
        # ZIP64 may be needed only for a local offset, after its local header was
        # written with version 2.0. Sizes still occupy the ordinary 32-bit fields.
        offset64 = changed(base, central + 6, 45, 2)
        offset64 = changed(offset64, central + 42, 0xFFFFFFFF, 4)
        offset64 = changed(offset64, central + 30, 12, 2)
        offset64 = changed(offset64, end + 12, end - central + 12, 4)
        offset64 = offset64[:end] + struct.pack("<HHQ", 1, 8, 0) + offset64[end:]
        carve("zip64-offset-only", offset64, [offset64])
        corruptions = {
            "stored-crc": (changed(base, 35, 0, 1), "zip_crc_or_length_mismatch"),
            "local-crc": (changed(base, 14, 1, 4), "zip_directory_local_mismatch"),
            "central-crc": (changed(base, central + 16, 1, 4), "zip_directory_local_mismatch"),
            "central-offset": (changed(base, central + 42, 1, 4), "zip_directory_local_offset_mismatch"),
            "central-count": (changed(base, end + 10, 2, 2), "zip_end_directory_mismatch"),
            "central-size": (changed(base, end + 12, 1, 4), "zip_end_directory_mismatch"),
            "split": (changed(base, end + 4, 1, 2), "zip_unsupported_split_archive"),
            "split-entry": (changed(base, central + 34, 1, 2), "zip_unsupported_split_archive"),
            "encrypted": (changed(base, 6, 1, 2), "zip_unsupported_encryption"),
            "method": (changed(base, 8, 12, 2), "zip_unsupported_compression_method_12"),
            "missing-extra": (changed(base, 18, 0xFFFFFFFF, 4), "zip_missing_zip64_extra"),
        }
        zipped64 = archive([{"data": b"zip64"}], wide=True)
        locator = zipped64.index(b"PK\x06\x07")
        end64 = zipped64.index(b"PK\x06\x06")
        extended64 = changed(zipped64, end64 + 4, 76, 8)
        extended64 = extended64[:locator] + b"\0" * 32 + extended64[locator:]
        carve("zip64-end-extension", extended64, [extended64])
        carve("zip64-extension-limit", extended64, options=["--zip-max-size", str(end64 + 60)],
              code=3, reason="zip_archive_size_limit")
        corruptions["zip64-locator"] = (changed(zipped64, locator + 8, 1, 8), "zip_invalid_zip64_locator")
        corruptions["zip64-overflow"] = (changed(zipped64, 30 + 5 + 12, 2**64 - 1, 8), "zip_store_length_mismatch")
        deflate64 = archive([{"method": 8}], wide=True)
        corruptions["zip64-packed-overflow"] = (changed(deflate64, 30 + 5 + 12, 2**64 - 1, 8), "zip_truncated_entry_data")
        corruptions["malformed-extra"] = (changed(zipped64, 30 + 5 + 2, 0xFFFF, 2), "zip_malformed_extra_field")
        corruptions["deflate-tail"] = (archive([{"method": 8, "trailing": b"extra"}]), "zip_crc_or_length_mismatch")
        described = archive([{"descriptor": True, "method": 8}])
        dd = described.index(b"PK\x07\x08")
        corruptions["descriptor-crc"] = (changed(described, dd + 4, 1, 4), "zip_data_descriptor_mismatch")
        corruptions["descriptor-size"] = (changed(described, dd + 8, 1, 4), "zip_data_descriptor_mismatch")
        for name, (content, reason) in corruptions.items():
            carve(name, content, reason=reason)
        for cut in (4, 25, 36, central - 1, central + 30, len(base) - 1):
            carve(f"truncated-{cut}", base[:cut])
        carve("truncated-comment", archive([{}], comment=b"comment")[:-1])
        # Byte mutations in the actual stream may never yield an accepted archive.
        compressed = archive([{"method": 8, "data": payload}])
        for i in range(12):
            at = 40 + rng.randrange(len(payload) - 100)
            carve(f"deflate-corruption-{i}", changed(compressed, at, compressed[at] ^ 0x80, 1))

        for flag, value, reason in (("--zip-max-size", len(base) - 1, "zip_archive_size_limit"),
                                    ("--zip-max-entries", 0, "zip_entry_limit"),
                                    ("--zip-max-directory", 32, "zip_directory_size_limit"),
                                    ("--zip-max-uncompressed", 1, "zip_uncompressed_size_limit")):
            records = carve(flag[2:], base, options=[flag, str(value)], code=3, reason=reason)
            assert records[-1]["stop_reason"] == reason
            assert any(r.get("skipped") and r["validation"] == "not_validated" for r in records)
        bomb = archive([{"method": 8, "descriptor": True, "data": b"a" * 1000000}])
        carve("expansion-limit", bomb, options=["--zip-max-uncompressed", "1000"], code=3,
              reason="zip_uncompressed_size_limit")
        descriptor_limit = archive([{"method": 8, "descriptor": True, "data": payload}])
        carve("descriptor-archive-limit", descriptor_limit, options=["--zip-max-size", "5000"], code=3,
              reason="zip_archive_size_limit")
        for flag in ("--zip-max-size", "--zip-max-entries", "--zip-max-directory", "--zip-max-uncompressed"):
            for value in ("-1", "1x", "18446744073709551616"):
                result = subprocess.run([program, flag, value, str(root / "mixed.img")], capture_output=True)
                assert result.returncode == 2, (flag, value)
    print("All P1 ZIP integration tests passed")


if __name__ == "__main__":
    main()
