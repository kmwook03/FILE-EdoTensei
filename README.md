# 💾 FILE-EdoTensei
FILE-EdoTensei is a Linux-focused C++17 forensic file-recovery tool. It scans an
image once with Aho-Corasick, validates each discovered candidate with a
format-specific parser, and writes deterministic evidence and recovery results.
It is best-effort software: a high score is useful ranking evidence, not a claim
that recovered content is complete or authentic.

## Skills & Environment

![C++](https://img.shields.io/badge/c%2B%2B-%2300599C.svg?style=for-the-badge&logo=cplusplus&logoColor=white) ![CMake](https://img.shields.io/badge/CMake-%23008FBA.svg?style=for-the-badge&logo=cmake&logoColor=white) ![Ubuntu](https://img.shields.io/badge/Ubuntu-%23E95420.svg?style=for-the-badge&logo=ubuntu&logoColor=white) ![Linux](https://img.shields.io/badge/Linux-%23FCC624.svg?style=for-the-badge&logo=linux&logoColor=black)

## Build and test
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is written to `app/FILEEdo`. A sanitizer configuration is also
available:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DFILEEDO_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

Tests use dependency-free C++ checks and deterministic fixtures produced by
`test/generate_fixtures.py`. The fixtures cover mixed content, corrupt and
truncated files, repeat runs, and a signature crossing the 1 MiB scan boundary.
P0 tests also compare recovered SHA-256 hashes, exercise reversed and sparse NTFS
extents and fragmented MFT records, and check deadlines with an injected clock.
The reader tests cover physical offsets above 4 GiB using a sparse temporary file.
For repeatable throughput measurements, generate sparse images of selected sizes
with `python3 test/benchmark.py ./app/FILEEdo`.
The latest local baseline is recorded in `docs/benchmark.md`.

## Usage
```bash
./app/FILEEdo disk.img
```

Available options are:

```text
--output <directory>       Recovery destination (default: current directory)
--report <path>            Streaming JSON Lines report
--mode raw|ntfs|hybrid     Recovery source (default: hybrid)
--min-confidence <0-100>   Reject candidates below the threshold
--time-limit <seconds>    Whole-job deadline (default: 7200; 0 disables)
--max-candidates <count>  Discovery cap (default: 100000)
--max-matches <count>     Scanner match cap (default: 1000000)
```

`hybrid` detects an NTFS VBR at byte zero or in an MBR partition and also runs
raw carving. If NTFS is not detected it falls back to raw recovery. Explicit
`ntfs` mode fails on unsupported input. Prefer image files during development;
only use `sudo` when direct access to a block device is necessary.

Candidate and match caps stop discovery while retained candidates can still be
validated and saved. The match cap counts all scanner patterns, including footers.
A zero count permits no candidates or matches. The deadline covers discovery,
validation, and extraction. Completed files remain available; interrupted temporary
outputs are removed. Checks are cooperative: blocking I/O, syncing, and final
reporting can delay termination. Exit codes are 0 for a complete run, 1 for runtime
errors, 2 for invalid arguments, and 3 for a budget-limited incomplete run.
The two-hour limit is an execution budget, not a measured 32GB recovery guarantee.

## Architecture

- `Searcher` is a stateful Aho-Corasick scanner. Pattern IDs are stable, matches
  use absolute 64-bit offsets, and automaton state crosses read boundaries.
- `FormatRegistry` owns descriptors and `FormatCarver` implementations.
  `RecoveryCandidate`, `ValidationResult`, and `RecoveryResult` are the shared
  contracts between discovery, validation, extraction, NTFS, and reporting.
- `CandidateReader` exposes bounded logical reads over raw, resident, fragmented,
  and sparse content. `validatedLength` counts logical bytes, independently of
  physical placement. Overlap selection uses the actual physical extents and
  favors metadata candidates when scores tie.
- `WorkBudget` shares a monotonic deadline and discovery limits across scanning,
  NTFS reads, format validation, and extraction. Its clock can be supplied by tests.
- Candidates are validated through random-access reads before any output is
  created. JPEG markers, PNG chunks and CRCs, and PDF version/EOF/xref evidence
  are handled by separate modules.
- Accepted content is written completely to a temporary file, synced, and
  atomically renamed. Existing output is never overwritten; `_1`, `_2`, and so
  on are selected deterministically.
- NTFS parsing validates volume geometry and arithmetic, applies MFT update
  sequence fixups, bounds-checks attributes and data runs, and supports resident,
  nonresident, fragmented, and sparse unnamed data streams. Metadata-derived
  extent streams are validated by the same format modules as raw candidates.

The extension point for a new format is `FormatCarver`: add stable header and
structural patterns, implement `validateContent` using logical reads, then register the
module in `FormatRegistry`. Maximum recovery size is held by each format's
descriptor.

## Report schema
The optional report is JSON Lines so records remain useful if a later candidate
fails. Each candidate record contains:

- `record_type`, `report_version`, and input path/size;
- absolute start offset and physical/sparse extents;
- format, recovery method, output path, and recovered byte count;
- accepted state, validation state, confidence, and truncation reason;
- logical `validated_length`, `skipped`, and `stop_reason`;
- named score contributions and warnings.

The final line is a summary with accepted, rejected, and error counts. Version 1
fields keep their meaning; incompatible schema changes require a new
`report_version`.
Additional summary fields include `complete`, `stop_reason`, `skipped`, `discovered`,
`matches`, `raw_scan_performed`, `raw_scan_complete`, `raw_scanned_bytes`, and
`elapsed_ms`. `rejected` includes retained candidates that were not saved;
`skipped` identifies those left unfinished. Candidates omitted by a discovery cap
cannot be counted as known rejects. `discovered` counts retained discoveries before
deduplication. A deadline or cancellation is not reported as source corruption;
unfinished validation uses `not_validated`. Actual read failures use `io_error`.
An empty stop reason indicates no recorded stop condition.

## Validation and confidence
Confidence is normalized to 0–100 from named contributions: header evidence,
structural parsing, valid end markers, PNG checksums or PDF cross-reference
evidence, and NTFS extent consistency. Truncation and invalid structure reduce
the score. The threshold filters output but rejected candidates are still
reported.

## Known limits

- Raw carving assumes contiguous content; speculative fragment reassembly is
  intentionally not attempted.
- NTFS attribute lists, compressed streams, and encrypted streams are rejected.
- Deleted clusters may have been overwritten even when metadata remains.
- PDF validation is structural evidence, not a full implementation of ISO 32000.
- Recovery from live devices can race filesystem activity. Work from a
  write-protected image whenever possible.

Never commit disk images, recovered user data, or generated output.
