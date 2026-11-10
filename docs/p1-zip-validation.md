# P1: contiguous ZIP and ZIP64 recovery

P1 adds one ZIP module to the existing format registry. Discovery works in raw,
NTFS and hybrid modes. Validation uses logical offsets through `CandidateReader`,
so NTFS-provided extent mappings also work. Raw discovery still assumes a
contiguous archive; raw fragment inference and document classification remain
P2 and P3 work.

## Supported records and extraction

- Single-volume ZIP with Store (0) and raw Deflate (8), including empty archives
  and entries, archive comments, and central-directory entries in a different
  order from the local entries.
- Local headers, central directory, EOCD, ZIP64 extra fields, ZIP64 EOCD and
  locator. ZIP64 may be used for small files, sizes above 4GiB, or offsets alone.
- Data descriptors with or without the optional signature, with 32-bit and
  ZIP64 sizes. Unknown Deflate lengths come from the stream terminator. Unknown
  Store lengths use a bounded window search and running CRC to locate a matching
  descriptor followed by a local or central record.
- Local and central names, flags, timestamps, methods, sizes, CRCs and relative
  offsets must agree. Every local entry must have exactly one directory entry.
  ZIP64 permits a central version upgrade when the offset alone needs ZIP64.
- Verification uses 64KiB input/output buffers. Deflate output is discarded
  after counting bytes and computing CRC. Extraction streams the original
  compressed archive bytes through the existing temporary-file publication path.

Structure, lengths and CRCs provide integrity evidence, not proof of original
identity. Test success requires whole-archive SHA-256 equality. The parser follows
the [PKWARE APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT),
especially sections 4.3, 4.4 and 4.5.3, and uses the
[zlib streaming interface](https://www.zlib.net/manual.html).

## Limits and reporting

| CLI option | Default | Scope |
| --- | --- | --- |
| `--zip-max-size` | 68,719,476,736 bytes | Entire recovered archive, including records and comment |
| `--zip-max-entries` | 100,000 | Local entries per archive |
| `--zip-max-directory` | 67,108,864 bytes | Local metadata total and central directory, each independently; ZIP64 end extension also bounded |
| `--zip-max-uncompressed` | 137,438,953,472 bytes | Sum of expanded entry sizes per archive |

All values are unsigned integers; zero is a literal cap. Limits are checked
before large metadata allocations and while expanding streams. The shared job
deadline also covers parsing, CRC verification and extraction. A ZIP limit leaves
the candidate `not_validated`, `skipped: true`, with a specific `zip_*_limit`
stop reason. The job reports `complete: false` and exits 3, while unrelated
candidates can continue. Candidate and match discovery caps retain P0 semantics.
Report schema version 1 and existing format behavior are preserved.

Damaged or truncated ZIPs and unsupported features are rejected without creating
an output file. Warnings distinguish CRC/length disagreement, malformed records,
encryption, unsupported methods/flags/versions, and split archives. SFX prefixes,
patch records, digital signatures, encrypted directories, and compression methods
other than Store/Deflate are outside this implementation. It does not attempt
backtracking through deliberately ambiguous Store descriptor sequences. Opaque
extra fields are bounded and preserved; their application-specific semantics
are not validated.

## Repeatable verification

```bash
cmake -S . -B /tmp/fileedo-p1-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/fileedo-p1-release -j 4
ctest --test-dir /tmp/fileedo-p1-release --output-on-failure

cmake -S . -B /tmp/fileedo-p1-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DFILEEDO_ENABLE_SANITIZERS=ON
cmake --build /tmp/fileedo-p1-sanitize -j 4
ctest --test-dir /tmp/fileedo-p1-sanitize --output-on-failure

python3 test/p1_large_zip.py ./app/FILEEdo \
  --result /tmp/fileedo-p1-large-result.json
```

Ordinary CTest includes the existing JPG, PNG, PDF and NTFS regressions plus P1
unit and integration tests. P1 covers independent Python `zipfile` seekable and
streaming writers, a second small fixture writer, Store/Deflate, signed/unsigned
classic/ZIP64 descriptors, empty streams, comments, directory reordering, scan
and descriptor boundaries, NTFS resident/reversed extents, stream corruption,
malformed lengths/offsets, truncation, unsupported features and resource limits.
An injected clock checks interrupted ZIP validation and refuses partial output.

The opt-in large test writes every byte of a Store ZIP64 payload of
4,296,015,872 bytes, adds another entry whose local offset exceeds 4GiB, and
checks the original archive with Python `ZipFile.testzip()`. It runs raw recovery
and compares the SHA-256 of the complete original and recovered archives. It
automatically removes all large temporary inputs and outputs. It is excluded
from CTest and needs about 9GiB of temporary storage.

## Local results

Environment: Linux x86-64 / WSL2, Intel Core i5-1135G7, GCC 13.3.0, zlib 1.3,
Release build. Temporary input and output use the local WSL filesystem; this is
not a physical USB or a 32GB workload. Large-test elapsed time includes raw
scan, verification and output sync/publication; fixture generation, independent
ZIP verification and SHA-256 calculation are outside that time.

Measured on 2026-11-10:

| Check | Result |
| --- | --- |
| Release CTest | 6/6 passed |
| ASan + UBSan + LeakSanitizer CTest | 6/6 passed, outside the ptrace sandbox |
| Actual archive size | 4,296,016,290 bytes |
| Source allocation (`st_blocks * 512`) | 4,296,024,064 bytes; fully written, not sparse |
| Raw scan + validation + extraction | 50.319 seconds (CLI summary: 50,305 ms) |
| Child maximum RSS (Linux `getrusage`) | 21,400 KiB |
| Accepted / rejected / skipped / errors | 1 / 3 / 0 / 0 |
| Full input scanned / job complete | yes / yes |
| Original vs recovered SHA-256 | identical |

Whole-archive SHA-256 for this run:
`efd452bcfc9a552ac71709f7c95c76755bcb517998a6b46ccd40a491c6765c19`.
The independent writer includes a timestamp on the trailing entry, so another
generation can have a different original hash; each run compares its own original
and recovered bytes. The three rejected candidates are interior/end records,
not additional corrupt source files. Small ZIP64 fixtures and this actual large
archive are separate checks. This result does not measure physical USB recovery,
quick formatting, 32GB workloads, raw fragmentation, or document application
compatibility.
