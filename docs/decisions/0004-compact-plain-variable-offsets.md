# Decision 0004: Compact Plain offsets for string and binary

- Status: Accepted
- Date: 2026-09-25

## Context

Plain v1 persists `row_count + 1` little-endian 64-bit offsets even for Arrow `string` and
`binary`, whose per-Array value buffer is limited by 32-bit offsets. On high-cardinality data,
this metadata makes Sniffer larger than an uncompressed Parquet file. Changing Plain ID 0 in
place would make old files ambiguous.

## Decision

- Assign encoding ID **4**, version **1.0**, descriptor name `compact_plain`, for string and
  binary only. Keep the Segment header version, footer payload v2, Plain ID 0, and all existing
  encoding payloads unchanged. Write the new footer descriptor only if a chunk uses ID 4;
  otherwise the footer bytes remain unchanged.
- The payload has the same three little-endian `u64` length fields and validity/value byte
  regions as Plain v1: `validity_length | offsets_length | values_length | validity | offsets |
  values`. The sole difference is that the normalized `row_count + 1` offsets are little-endian
  **u32**, so `offsets_length == 4 * (row_count + 1)`. Null rows repeat the previous offset and
  have no bytes in `values`. The first offset is zero and the last equals `values_length`.
- The encoder accepts ID 4 only when `values_length <= UINT32_MAX`; explicit forced use fails
  with a structured error otherwise. The decoder validates lengths, monotonic bounded offsets,
  normalized endpoints, validity/null count, Arrow value limits, selection bounds, and trailing
  bytes before constructing output. It must not reinterpret untrusted input as native integers.
- Adaptive selection uses the existing deterministic 90%-of-Plain size rule and compares the
  compact estimate with Dictionary and other supported candidates. The chunk's
  `uncompressed_length` continues to record the **Plain v1** payload size. Forced Plain remains
  ID 0. A new Reader accepts both IDs; an old Reader fails explicitly on ID 4 rather than
  mis-decoding it. Footer descriptors with unknown IDs or incompatible versions still fail.

## Consequences

High-cardinality string/binary chunks can save four bytes per row offset without changing
logical values, row order, projection, or predicate semantics. The new writer can emit files an
old Reader cannot read; the new Reader remains able to read v0.1 Plain files. Benchmarks must
report compression ratio, write/read time, and the selected encoding IDs.
