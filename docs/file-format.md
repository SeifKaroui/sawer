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

Metadata is deterministic, definite-length CBOR. Maps use ordered UTF-8 keys;
world-space values and widths are float64; counters and z-order are integers;
UUIDs are 16-byte byte strings. Indefinite-length values and CBOR tags are
not used. Unknown map fields may be ignored, but unknown operations and object
types are rejected.

Readers process frames in order and apply only fully validated frames. A
truncated final frame is discarded and the board is compacted before it is
saved again. Corruption before the final frame rejects the board. Existing
development JSONL boards remain readable; their next save rewrites them in
this format.

Image assets use kind `2` records before any placement that references them.
Their metadata contains `op: asset_put`, a 32-byte `asset_id`, `mime`, pixel
dimensions, and an optional binary preview; the record payload is the
canonical RGBA8 PNG. `asset_id` is SHA-256 of those exact PNG bytes. The
preview is also a canonical PNG limited to 256 pixels on either side, so Home
can composite it without decoding the full asset. A reader validates the hash,
dimensions, PNG canonicalization, and preview limit before accepting an asset.
An image placement is a normal `put` operation with `type: image`, the asset
ID, two world-space corners, and z-order. Repeated placements share one asset
payload. Repeated identical asset frames are accepted, while the same asset ID
with different payload bytes is corruption.

## Inspection tool

The bundled `sawer-format` program never needs companion files:

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
