# Rendering and navigation performance

Sawer caches completed geometry and GPU meshes, evicts cold entries
incrementally, overlaps streamed batches, renders the grid procedurally,
and reuses visibility queries and stroke detail during navigation.
It preserves the existing image, preview, selection, Home and MSAA paths.

## Implemented behavior

| Point | Behavior | Bounds and fallback |
| --- | --- | --- |
| 2. Incremental eviction | Geometry and stroke indices use ID-linked LRU order. Hits and victim selection avoid full-cache scans and sorting. Pressure removes individual cold entries. | Geometry cache: 256 MiB including estimated metadata, 50,000 entries, 90% pressure target. Ordinary age cleanup removes at most 64 entries per pruning pass. Stroke indices: 128 MiB and 256 entries. |
| 3. Overlapping batches | Two streamed vertex/staging slots have independent completion fences. CPU construction proceeds while the previous slot executes. A slot waits only if its previous GPU consumer is unfinished. | Each batch contains at most 1,048,575 vertices. Two fixed slots bound staging and vertex-buffer allocations. MSAA contents survive intermediate batches and resolve after the last batch. |
| 4. Procedural background | A fullscreen triangle shades all nine background styles directly. Grid phases are reduced in double precision on the CPU before conversion to shader floats. | One background draw per scene regeneration, no grid vertex uploads, viewport-sized rendering only. Logical/physical scale, board clipping, themes and custom colors are preserved. |
| 5. Visibility/detail reuse | A padded viewport caches sorted candidate IDs. Small pans filter candidates. Up to 32 document changes update candidates without another spatial query. Two detail variants and conservative hysteresis avoid alternating stroke rebuilds. | Maximum 20,000 cached candidates. Larger edits or a missing journal interval query again. Zoom-in upgrades are never delayed when finer detail is necessary. |
| 6. Persistent meshes | Completed meshes occupy reusable GPU page ranges. Pan and same-detail zoom update camera uniforms. Changed objects alone invalidate their CPU/GPU meshes. Deleted ranges retire after their last GPU consumer completes. | 64 pages of 65,535 vertices: approximately 48 MiB of resident GPU vertex storage. At most three outstanding resident-frame fences. Large, paged or over-budget geometry keeps the camera-relative streamed path. |

Resident meshes use double-precision origins snapped to 2,048-unit cells and
local float vertices. Objects wider or taller than 4,096 world units use the
streamed path to preserve precision. A frame stages at most 1,048,575 new
resident vertices; additional work uses the existing bounded path.

Cold streamed scenes still wait once for their shared resource uploads before
the bounded draw batches begin. Diagnostic pixel readbacks also wait for
completion. The removed synchronization was the unconditional wait after
every streamed draw batch.

The document provides a renderer-independent 4,096-entry change journal and a
transient document identity. Mutations, undo and redo enter this journal;
replaced documents and missing change history reset disposable render state.
Neither the identity nor the journal changes the `.sawer` file format.

Pathological stroke indices stop duplicating segment references after one
million references. They use conservative AABB blocks of 256 segments instead.
This keeps index memory bounded without dropping potentially visible segments.
An index pointer is borrowed only until another index request, a geometry
mutation or a transient-cache clear.

Geometry eviction also invalidates reusable mesh descriptors when necessary.
A completed streamed raster remains valid after its source meshes are evicted;
its descriptors are rebuilt before the next raster regeneration. Home and
document replacement release board caches and mesh pages.

## Verification

The new `--rendering-performance-test` covers:

- 1,000 retained lines and 30 warm pan frames: zero scene uploads, zero
  tessellations and reuse of the visibility query.
- Same-detail zoom, one-object style edits, move, delete, undo and redo, with
  pixel readback checks and bounded resident allocation across repeated edits.
- Alternating stroke detail levels after both variants are warm: zero uploads
  and tessellations.
- All nine grid styles, pan/zoom anchoring, MSAA enabled and disabled, and a
  3840×2160 drawable: one background draw and zero scene uploads.
- A line near world coordinate 999,000 at zoom 64 to check local-float precision.
- Home releasing resident mesh storage and geometry caches.

Unit checks cover LRU copy/move/rehash behavior, cache pressure preserving hot
stroke indices, conservative pathological-stroke queries, journal gaps/reset,
visibility ordering after edits and document replacement, and randomized mesh
range allocation with non-overlap and reuse assertions.

Existing application checks additionally cover images interleaved with vector
batches, dynamic previews, selection, resize, recovery, dense scenes over
16 million vertices and a 750,000-point fragmented stroke.

On 2026-10-02, the complete Windows Debug suite passed all 22 checks. The final
Windows Release suite passed all 22 checks with both Direct3D 12 and SDL's
Vulkan driver, using warnings as errors and the strict Release performance
budgets. Unit checks passed 2,396,346 assertions in 215 cases. The adapter was
an NVIDIA GeForce RTX 5060 Laptop GPU.

The final isolated Release checks reported the following single-run samples:

| Driver | Warm-pan CPU work p50 | Warm-pan CPU work p95 | Large-board average construction |
| --- | --- | --- | --- |
| Direct3D 12 | 1.28 ms | 2.09 ms | 1.11 ms |
| Vulkan on Windows | 1.52 ms | 2.36 ms | 1.13 ms |

Warm-pan samples contain 30 frames and 1,000 line objects. Both drivers report
zero scene uploads and tessellations for those frames. The large-board fixture
contains 100,001 objects, one million stroke points and 3,001 visible objects.
These are construction/recording measurements on this machine, with no
comparable earlier implementation baseline.

A RenderDoc 1.46 capture of a warm Direct3D 12 pan frame showed one background
draw, 51 resident draws and five UI/text draws. The two resident buffers were
bound without any buffer copies into them; the three copies in the frame
updated UI/text buffers. This is a structural work check, not a measured
before/after speedup or a GPU timing claim. Temporary capture instrumentation
is absent from the application sources.

Meson defaults `b_ndebug` to `if-release`, so optimized builds identify as
Release and exercise the existing stricter performance thresholds. Rendering
checks that need swapchain images use visible windows; Vulkan deliberately
skips presentation for hidden windows.
The four rendering performance checks run individually in Meson to avoid GPU
contention from unrelated application checks. The timing thresholds remain
unchanged.

## Reproduction

Use Sawer's single build directory:

```powershell
meson setup build --buildtype=debug -Dsawer_werror=true
meson compile -C build
meson test -C build --print-errorlogs
build/Sawer.exe --rendering-performance-test

meson setup build --wipe --buildtype=release -Dsawer_werror=true
meson compile -C build
meson test -C build --print-errorlogs
```

SDL's driver override permits an additional Vulkan check on a supported Windows
adapter:

```powershell
$env:SDL_GPU_DRIVER = 'vulkan'
build/Sawer.exe --rendering-performance-test
build/Sawer.exe --stroke-visibility-test
Remove-Item Env:SDL_GPU_DRIVER
```

`RendererStats` exposes resident upload bytes/draws/pages, visibility query
reuse, geometry cache metadata and batch reuse waits. The navigation check's
CPU-work percentiles sum construction, staging and command recording after
excluding nested swapchain/batch waits. They do not measure end-to-end latency
or GPU execution, and require a comparable hardware/build baseline before a
speedup can be claimed.

Shader generation is offline; ordinary builds need no shader compiler. See
[the shader guide](../assets/shaders/README.md) and
[`tools/compile_shaders.py`](../tools/compile_shaders.py) for pinned DXC artifacts
and regeneration.

## Remaining validation

Linux Wayland/X11 execution, integrated-GPU measurements, physical mixed-DPI
multi-monitor checks, tolerant full-image rendering goldens and long-running
memory measurements remain outstanding. Deeper Tracy CPU profiling and GPU
timing comparisons against a controlled baseline are still needed before
claiming the 60/120 FPS product targets across supported hardware. The existing
viewport raster cache remains useful for dense streamed scenes; residency is
bounded and does not promise zero uploads for every possible board.

SDL Render fallback and complete device-loss recovery remain separate work.
The measurements above are local hardware results, not hosted CI results.
