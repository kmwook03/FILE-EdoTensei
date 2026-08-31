#!/usr/bin/env python3
"""Generate deterministic sparse images and print FILEEdo wall-time measurements."""
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: benchmark.py <FILEEdo executable>")
    executable = pathlib.Path(sys.argv[1]).resolve()
    sizes = (64, 256, 1024)
    with tempfile.TemporaryDirectory(prefix="fileedo-benchmark-") as directory:
        root = pathlib.Path(directory)
        for mib in sizes:
            image = root / f"sparse-{mib}m.img"
            with image.open("wb") as stream:
                stream.truncate(mib * 1024 * 1024)
            output = root / f"output-{mib}"
            started = time.monotonic()
            subprocess.run([str(executable), "--mode", "raw", "--output", str(output),
                            str(image)], check=True, stdout=subprocess.DEVNULL)
            elapsed = time.monotonic() - started
            rate = mib / elapsed
            print(f"{mib:4d} MiB  {elapsed:8.3f} s  {rate:8.1f} MiB/s")


if __name__ == "__main__":
    main()
