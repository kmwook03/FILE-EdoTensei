#!/usr/bin/env python3
"""Byte-for-byte recovery regressions and externally observable work limits."""
import collections
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="fileedo-p0-integration-") as directory:
        root = pathlib.Path(directory)
        subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name("generate_fixtures.py")),
                        str(root)], check=True)
        expected = root / "expected"

        def carve(name, image, originals, options=(), exit_code=0):
            output = root / f"{name}-out"
            report = root / f"{name}.jsonl"
            result = subprocess.run([program, "--output", str(output), "--report", str(report),
                                     *options, str(root / image)], capture_output=True, text=True)
            assert result.returncode == exit_code, (name, result.returncode, result.stdout, result.stderr)
            records = [json.loads(line) for line in report.read_text().splitlines()]
            summary = records[-1]
            assert summary["record_type"] == "summary" and summary["report_version"] == 1
            outputs = list(output.iterdir()) if output.exists() else []
            assert all(not path.name.endswith(".tmp") for path in outputs), name
            assert collections.Counter(map(digest, outputs)) == collections.Counter(
                digest(expected / original) for original in originals), name
            accepted = [record for record in records[:-1] if record["accepted"]]
            assert summary["accepted"] == len(accepted) == len(originals), name
            for record in accepted:
                assert record["byte_count"] == record["validated_length"]
                assert not record["skipped"] and record["stop_reason"] == ""
            assert summary["complete"] == (exit_code == 0), name
            return records

        carve("mixed-checksums", "mixed.img", ["original.jpg", "original.png", "original.pdf"], ["--mode", "raw"])
        for name in ("fragmented", "reversed", "sparse", "fragmented-mft"):
            records = carve(name, f"{name}.ntfs", [f"{name}.jpg"], ["--mode", "ntfs"])
            accepted = [record for record in records[:-1] if record["accepted"]]
            assert accepted[0]["method"] == "ntfs_metadata"
            assert len(accepted[0]["extents"]) > 1
            if name == "sparse":
                assert any(extent["sparse"] for extent in accepted[0]["extents"])

        for name in ("fragmented", "reversed"):
            records = carve(f"{name}-gap", f"{name}.ntfs", [f"{name}.jpg", "original.jpg"],
                            ["--mode", "hybrid", "--min-confidence", "100"])
            assert any(record.get("method") == "ntfs_metadata" and record.get("accepted") for record in records)

        records = carve("hybrid-resident", "resident.ntfs", ["original.jpg"], ["--mode", "hybrid"])
        assert any(record.get("accepted") and record.get("method") == "ntfs_metadata" for record in records)

        for flag, reason in (("--max-candidates", "candidate_limit"), ("--max-matches", "match_limit")):
            records = carve(reason, "mixed.img", ["original.jpg"], ["--mode", "raw", flag, "1"], 3)
            assert records[-1]["stop_reason"] == reason and records[-1]["errors"] == 0
            assert not records[-1]["raw_scan_complete"]
            carve(f"{reason}-zero", "mixed.img", [], ["--mode", "raw", flag, "0"], 3)
        records = carve("metadata-cap", "resident.ntfs", [], ["--mode", "ntfs", "--max-candidates", "0"], 3)
        assert records[-1]["stop_reason"] == "candidate_limit"
        carve("unlimited-deadline", "mixed.img", ["original.jpg", "original.png", "original.pdf"],
              ["--mode", "raw", "--time-limit", "0"])

        for flag, value in (("--max-candidates", "-1"), ("--max-matches", "1x"),
                            ("--max-candidates", "18446744073709551616"),
                            ("--time-limit", "18446744073709551615"), ("--time-limit", "")):
            result = subprocess.run([program, flag, value, str(root / "mixed.img")], capture_output=True)
            assert result.returncode == 2, (flag, value)

        before = digest(root / "mixed.img")
        alias = root / "report-alias"
        alias.hardlink_to(root / "mixed.img")
        result = subprocess.run([program, "--report", str(alias), str(root / "mixed.img")], capture_output=True)
        assert result.returncode == 1 and digest(root / "mixed.img") == before, "report must not truncate input"
    print("All P0 integration tests passed")


if __name__ == "__main__":
    main()
