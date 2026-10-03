# Board loading optimizations

Measured on Windows x64 on 2026-10-02 with GCC 16.1.0 and a Meson Release
build (`-O3`). These are synthetic workloads with a warm filesystem cache;
they are component measurements, not a claimed speedup for every board.

## Results

| Measurement | Previous work | Optimized work |
| --- | ---: | ---: |
| CRC32C over 16 MiB, fastest of three runs | 92.459 ms, original bit loop | 31.453 ms, lookup table |
| Prepare four 1024×1024 images, 32 placements, median of three runs | 18.236 ms, decode on cache miss | 0.002 ms, reuse validated pixels |
| Extra full-image decodes during image preparation | 4 | 0 |
| Retained decoded pixels for the four-image workload | 16 MiB | 16 MiB |

CRC32C processing was approximately 2.94 times faster. Image preparation
measures CPU decoding and cache lookup, excluding GPU uploads. PNG decoding
and re-encoding for canonical validation remain part of opening the file.

The optimized loader's median opening times were 565.146 ms for 100,000 line
objects, 344.513 ms for one million stroke points, and 909.161 ms for the
four-image workload. These opening times establish a regression baseline;
they do not compare the old and new loaders.

Peak resident memory for the entire benchmark process, including fixture
generation and validation's temporary buffers, was 96,657,408 bytes.
Each decoded-image cache retains at most 128 MiB by default. The outgoing
and incoming caches may coexist during opening, so their combined limit is
256 MiB, in addition to validation buffers and GPU resources.

## Verification

The Windows Release build passed all 21 Meson checks, including 204 unit
test cases and the large-board, drawing, and buffer-growth regressions.
The GPU loading check recreates the renderer to prevent existing textures
from masking cache misses. It verifies synchronous and background opens,
identical sampled output, repeated opens, and failed/superseded loads.
Its 1024×1024 image first-frame object preparation measured 3.851 ms without
prepared pixels and 1.472 ms with reuse, with extra decodes reduced from one
to zero. First-frame timing includes more work than CPU image decoding and
varies with the graphics driver.

Checksum tests cover a standard CRC32C vector, an independent implementation
of the old algorithm, chunked and unaligned inputs, and record checksum
coverage. Existing final-record recovery and earlier-corruption tests pass.
Image tests cover cache moves, replacement, eviction, oversized entries,
late loading failures, and malformed or oversized embedded previews.
Linux runtime validation remains outstanding.
These are the results of that dated local run.

## Reproduction

Use only the repository's `build/` directory. After configuring and compiling
a Release build through Meson, run:

```text
meson test -C build --benchmark --no-rebuild --num-processes 1 --print-errorlogs
meson test -C build --no-rebuild --num-processes 1 --print-errorlogs
```

The benchmark output is recorded in `build/meson-logs/testlog.txt`; subsequent
test runs replace that log. The benchmark generates and removes its own board
files in the platform temporary directory.
