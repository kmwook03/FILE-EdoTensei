#!/usr/bin/env python3
"""Opt-in, non-sparse >4GiB ZIP64 recovery; uses temporary disk images only."""
import argparse
import hashlib
import json
import pathlib
import resource
import shutil
import subprocess
import tempfile
import time
import zipfile


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=pathlib.Path)
    parser.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path(tempfile.gettempdir()))
    parser.add_argument("--result", type=pathlib.Path, help="Optional small JSON measurement artifact")
    args = parser.parse_args()
    program = args.program.resolve()
    size = (1 << 32) + 1024 * 1024
    if shutil.disk_usage(args.work_dir).free < 2 * size + 512 * 1024 * 1024:
        raise SystemExit("Need at least 9 GiB of free space for the source and recovered archive")
    with tempfile.TemporaryDirectory(prefix="fileedo-p1-large-", dir=args.work_dir) as temporary:
        root = pathlib.Path(temporary)
        source = root / "original.zip"
        # Write every byte. This exercises real ZIP64 sizes, CD offsets and full
        # CRC verification, unlike a sparse file containing a tiny archive at >4GiB.
        block = bytes(range(256)) * 4096
        generated = time.monotonic()
        with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_STORED) as writer:
            writer.comment = b"FILEEdo P1 actual ZIP64 >4GiB fixture"
            with writer.open("payload.bin", "w", force_zip64=True) as entry:
                remaining = size
                while remaining:
                    amount = min(remaining, len(block))
                    entry.write(block[:amount])
                    remaining -= amount
            # Its local header uses ordinary sizes; only the central-directory
            # offset needs ZIP64, at a physical position above 4GiB.
            writer.writestr("after-4gib.txt", b"ZIP64 local offset regression")
        generation_seconds = time.monotonic() - generated
        assert source.stat().st_size > 1 << 32
        with zipfile.ZipFile(source) as reader:
            assert reader.getinfo("payload.bin").file_size == size
            assert reader.testzip() is None, "Independent ZIP64 CRC verification failed"
        original_hash = digest(source)
        report = root / "report.jsonl"
        started = time.monotonic()
        completed = subprocess.run([str(program), "--mode", "raw", "--report", str(report),
                                    "--output", str(root / "out"), str(source)],
                                   capture_output=True, text=True)
        elapsed = time.monotonic() - started
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        records = [json.loads(line) for line in report.read_text().splitlines()]
        accepted = [record for record in records[:-1] if record["accepted"]]
        outputs = list((root / "out").iterdir())
        assert len(accepted) == len(outputs) == records[-1]["accepted"] == 1
        assert records[-1]["complete"] and records[-1]["raw_scan_complete"]
        assert accepted[0]["validation"] == "valid" and accepted[0]["byte_count"] == source.stat().st_size
        assert digest(outputs[0]) == original_hash
        result = {"archive_bytes": source.stat().st_size, "payload_bytes": size,
                  "source_allocated_bytes": source.stat().st_blocks * 512,
                  "sha256": original_hash, "hash_match": True,
                  "generation_seconds": round(generation_seconds, 3),
                  "recovery_seconds": round(elapsed, 3),
                  "child_max_rss_kib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
                  "summary": records[-1], "program": str(program)}
        encoded = json.dumps(result, indent=2) + "\n"
        print(encoded, end="")
        if args.result:
            args.result.write_text(encoded)


if __name__ == "__main__":
    main()
