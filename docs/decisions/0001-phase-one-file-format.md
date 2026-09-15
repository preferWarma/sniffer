# 0001: Phase-one file format

- Status: Accepted
- Date: 2026-09-15
- Scope: Sniffer Core format version 1.0, phase one

## Context

`DESIGN.md` fixes the high-level `Header -> Data -> Index -> Footer` shape but does
not define enough byte-level semantics to implement a safe reader. Phase one needs a
small, deterministic format for Plain-encoded flat Arrow types. This format is a
Sniffer Core format and is not byte-compatible with ByteHouse Sniffer or Parquet.

## Decision

All integers are explicitly little-endian. All file offsets, byte lengths, row counts,
and null counts are `uint64_t`. Variable-size column offsets are also stored as
`uint64_t`, independently of Arrow's in-memory 32-bit `Binary`/`String` offsets.

### File envelope

```text
Header (32 bytes)
ColumnChunk payloads
Footer payload
Footer trailer (40 bytes)
```

The fixed Header is:

```text
magic[8] = "SNIFSEG1"
format_major:u16 = 1
format_minor:u16 = 0
header_size:u32 = 32
flags:u32 = 0
reserved:u64 = 0
header_crc32c:u32
```

The header checksum covers the first 28 header bytes.

The fixed Footer trailer is:

```text
magic[8] = "SNIFEND1"
footer_offset:u64
footer_length:u64
footer_crc32c:u32
file_crc32c:u32
trailer_crc32c:u32
reserved:u32 = 0
```

The footer checksum covers the footer payload. The file checksum covers all bytes from
the beginning of the Header through the end of the footer payload, excluding the
trailer. The trailer checksum covers its first 32 bytes. CRC32C uses the Castagnoli
polynomial. `Open()` verifies Header, footer, trailer, and directory boundaries;
`VerifyFileChecksum()` performs the intentionally full-file verification. A scan
verifies every ColumnChunk it reads.

### Plain ColumnChunk payload

The phase-one payload is:

```text
validity_length:u64
offsets_length:u64
values_length:u64
validity bytes
offset bytes
value bytes
```

The optional validity bitmap uses Arrow's LSB bit numbering. It is absent when
`null_count == 0`; otherwise it has exactly `ceil(row_count / 8)` bytes. Masked value
slots are encoded deterministically as zero/false/empty even though Arrow does not
assign semantics to their contents.

Fixed-width values are encoded one by one in little-endian order. Boolean values use
one byte per row in Plain encoding. Floating-point values preserve their IEEE bit
patterns. Timestamp values are signed 64-bit integers; unit and timezone remain in the
schema descriptor.

String and binary chunks contain `row_count + 1` little-endian `uint64_t` offsets,
normalized to start at zero, followed by concatenated non-null values. Offsets must be
monotonic and bounded by `values_length`.

### Footer payload

The footer stores:

- schema version and field descriptors, including stable `field_id`, name, nullable
  flag, logical type, and timestamp parameters;
- encoding descriptors with stable ID and version (`Plain = 0`, version 1.0);
- the Row Group directory and, for every ColumnChunk, field ID, physical type,
  encoding ID, row/null counts, file offset/length, uncompressed length, and CRC32C.

Each Row Group contains exactly one ColumnChunk per schema field, ordered by schema
position. Field names are never used to identify persisted chunks.

### Compatibility and limits

Unknown format major versions, physical types, or encoding IDs fail explicitly.
Unknown optional indexes may be ignored only after a later decision defines their
capability bits and directory representation. Phase one writes no index region.

Supported logical types are bool, signed and unsigned integers, float32/64, timestamp,
string, and binary. Nested types and default values fail with `NotImplemented`.

`LayoutPolicy` index field lists must be empty in phase one. Each non-empty appended
batch is split into Row Groups no larger than `target_row_group_rows`; row groups are
not coalesced across `Append()` calls. An empty segment retains its schema and reads as
one zero-row batch.

## Consequences

- The reader can validate metadata without reading data chunks and can later preserve
  phase-two selective I/O.
- A full-file checksum is available without forcing every scan to read the full file.
- Plain encoding is intentionally simple and portable; zero-copy decoding is deferred
  because endian conversion and selection-aware decoding will need explicit ownership.
- Any change to these byte-level rules requires a new format version or a compatible,
  explicitly versioned extension.
