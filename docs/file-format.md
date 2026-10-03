# Sawer board file format (version 1)

A `.sawer` file is a self-contained append-only binary log. It begins with
the eight-byte prologue `53 41 57 45 52 00 01 00` (`SAWER`, NUL, version 1,
NUL), followed by framed records.

Each record has a 32-byte header written field by field:

| Field | Size | Encoding |
| --- | ---: | --- |
| magic | 4 | ASCII `SWRF` |
| frame version | 1 | `1` |
| kind | 1 | `0` header, `1` operation, `2` asset |
| flags | 2 | little-endian, currently zero |
| sequence | 8 | little-endian |
| metadata size | 4 | little-endian |
| payload size | 8 | little-endian |
| CRC32C | 4 | little-endian |

CRC32C covers the header from `frame version` through `payload size`, then
the metadata and payload. The initial record is kind `0`, sequence zero, and
contains the board header. Operation frames are kind `1` and have contiguous
positive sequence numbers.

Metadata uses definite-length CBOR. The writer orders map keys
lexicographically as UTF-8 strings. World-space values and widths are doubles
in memory; nlohmann/json writes float32 when it represents the value exactly,
and float64 otherwise. The reader accepts float16, float32, and float64.
Counters and z-order are
integers; UUIDs are 16-byte byte strings. Indefinite-length values and CBOR
tags are not used. Metadata nesting is limited to 128 levels. Unknown map
fields may be ignored, but unknown operations and object types are rejected.

Readers process frames in order and apply only fully validated frames. A
truncated final frame, or a CRC failure in the final frame after a valid board
header, is discarded. Opening such a board immediately rewrites the recovered
state through atomic compaction. Earlier CRC failures and invalid complete
records reject the board. A file that changes during reading is also rejected.
Existing development JSONL boards remain readable; their next save rewrites them in
this format.

Image assets use kind `2` records before any placement that references them.
Their metadata contains `op: asset_put`, a 32-byte `asset_id`, `mime`, pixel
dimensions, and an optional binary preview; the record payload is the
canonical RGBA8 PNG. `asset_id` is SHA-256 of those exact PNG bytes. The
preview is also a canonical PNG limited to 256 pixels on either side, so Home
can composite it without decoding the full asset. A reader validates the
payload hash, dimensions, and PNG canonicalization. When a preview is present,
it must decode successfully and fit the preview size limit.
An image placement is a normal `put` operation with `type: image`, the asset
ID, two world-space corners, and z-order. Repeated placements share one asset
payload. Repeated identical asset frames are accepted, while the same asset ID
with different payload bytes is corruption.

## Inspection tool

Meson also builds `build/sawer-format` (`build/sawer-format.exe` on Windows).
It is a separate developer tool, currently omitted from the installer and
portable packages. The board itself never needs companion files:

```text
sawer-format info board.sawer
sawer-format validate board.sawer
sawer-format dump board.sawer
sawer-format extract board.sawer output-directory
sawer-format compact board.sawer
```

`dump` renders binary IDs as lowercase hexadecimal strings and omits raw image
payloads. `extract` writes each immutable PNG using its SHA-256 ID and a JSON
manifest. `compact` rewrites the final document state and drops unreferenced
assets and interrupted trailing records.

`validate` uses the application's semantic loader on a temporary copy, so
inspection cannot repair or modify the original board. Recoverable trailing
frames are accepted. `info`, `dump`, and `extract` inspect binary framing;
they do not replace semantic validation. Legacy JSONL boards are supported by
`validate` and `compact`, while the other commands require the binary format.
