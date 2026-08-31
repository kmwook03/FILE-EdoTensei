# Scanner benchmark

Recorded 2026-08-31 with the Debug build on the development container. Images
were sparse, zero-filled files and therefore contained no candidates. Run the
same workload with:

```bash
python3 test/benchmark.py ./app/FILEEdo
```

| Image size | Wall time | Throughput |
| ---: | ---: | ---: |
| 64 MiB | 2.288 s | 28.0 MiB/s |
| 256 MiB | 8.802 s | 29.1 MiB/s |
| 1024 MiB | 35.291 s | 29.0 MiB/s |

The stable rate is consistent with one scanner pass per input byte. This is a
regression baseline, not a cross-machine performance claim. Candidate storage
depends on signature density; the scan buffer itself remains fixed at 1 MiB.
