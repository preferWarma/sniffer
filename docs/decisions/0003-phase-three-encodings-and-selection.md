# Decision 0003: Phase-three encodings and deterministic selection

- Status: Accepted
- Date: 2026-09-15

## Context

Phase three requires Dictionary, RLE, and FOR + Bitpack, but `DESIGN.md` does not assign encoding
IDs, define nullable payloads, or specify deterministic selection thresholds. These details are
part of the persistent format and cannot be inferred by a reader.

## Decision

### Encoding descriptors

Footer payload v2 remains unchanged structurally. Its encoding descriptor table may contain:

| ID | Version | Name | Supported physical types |
| --- | --- | --- | --- |
| 0 | 1.0 | `plain` | all v0.1 types |
| 1 | 1.0 | `dictionary` | signed/unsigned integers, string, binary |
| 2 | 1.0 | `rle` | bool and signed/unsigned integers |
| 3 | 1.0 | `for_bitpack` | signed/unsigned integers and timestamp |

Readers accept legacy files whose descriptor table contains only Plain. A chunk may use an
encoding only when the descriptor is present and the physical type is supported. Unknown IDs or
newer incompatible versions fail explicitly.

All payload integers are little-endian. Lengths and counts are `uint64_t`. Canonical scalar bytes
are those defined by Decision 0002. Every decoder validates counts, lengths, padding, run totals,
dictionary indices, bit widths, and arithmetic before allocating or indexing.

### Dictionary payload

```text
validity_length:u64
dictionary_count:u64
index_width:u8 | reserved[7]
dictionary_offsets_length:u64
dictionary_values_length:u64
indices_length:u64
validity | dictionary_offsets | dictionary_values | indices
```

Dictionary entries are ordered by first non-null occurrence. Fixed-width entries are concatenated;
string/binary entries use `dictionary_count + 1` little-endian `uint64_t` offsets. Indices use the
smallest of 1, 2, 4, or 8 bytes. Null rows have a cleared validity bit and a deterministic zero
index. Dictionary values never contain nulls.

### RLE payload

```text
run_count:u64 | reserved:u64
repeated run_count times:
  count:u64 | valid:u8 | reserved[7] | canonical_value[physical_width]
```

Runs are over `(validity, value)`. Adjacent nulls form one run and carry deterministic zero value
bytes. Run counts are positive and must sum exactly to the chunk row count.

### FOR + Bitpack payload

```text
validity_length:u64
bit_width:u8 | reserved[7]
base_length:u64
packed_length:u64
base | validity | packed_deltas
```

The base is the minimum non-null value. Null rows use delta zero. Deltas are unsigned mathematical
differences from the base and are packed least-significant bit first with width 0..64. Padding bits
must be zero. All-null chunks use an all-zero base and bit width zero. Decoders reject reconstructed
values outside the physical type domain.

### Selection policy

`LayoutPolicy` exposes a fixed `encoding_sample_rows` (default 1024) and optional per-field forced
encoding overrides. Overrides exist for controlled tests, benchmarks, and applications that have
external distribution knowledge; invalid type/encoding combinations fail before opening output.

Without an override, the writer examines exactly the first `min(length, encoding_sample_rows)`
rows. It estimates payload sizes for every applicable encoding using the sampled distinct count,
run count, and integer range. A non-Plain encoding is selected only when its estimate is at most
90% of Plain. The smallest estimate wins, with ties resolved by ID. There is no randomness, clock,
address, hash iteration order, or hardware-dependent branch in selection.

The selected encoding ID and version are persisted in the existing chunk/footer metadata.
`uncompressed_length` records the exact Plain payload size for the same chunk.

### Verification artifacts

- Property tests force each encoding over random, skewed, repeated, monotonic, extreme, and nullable
  data and compare round-trip and IOPlan results.
- A standalone fuzz target accepts arbitrary bytes as a Segment and exercises Open, checksum, full
  decode, and a minimal scan. A deterministic smoke corpus is run by CTest under sanitizers; the
  same source can be compiled with libFuzzer.
- A standalone benchmark records distribution, row-group size, selectivity, hardware, build mode,
  file bytes, chunk bytes read, and median throughput over repeated iterations. Benchmark claims
  are made only from its raw output, never a single timing.

## Consequences

- Existing footer-v1/v2 Plain files remain readable.
- Codec additions do not change index or scan semantics.
- SIMD and multi-stage encoding chains remain outside v0.1.
