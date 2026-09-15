# Decision 0002: Phase-two indexes and scan contract

- Status: Accepted
- Date: 2026-09-15

## Context

Phase two adds row-group pruning and `IOPlan` scans. `DESIGN.md` names the indexes and
execution order but intentionally does not define their byte representation, scalar ordering,
Bloom parameters, or the observable scan counters needed by pruning tests.

## Decision

### Footer compatibility

The footer payload version is raised from 1 to 2. Readers accept both versions. Version 1 means
that no phase-two layout or index metadata is present. Version 2 appends the following data after
the existing row-group directory:

```text
LayoutPolicy
  target_row_group_rows:u32
  sort_key_count:u32 | statistics_count:u32 | bloom_count:u32
  sort_key_field_ids:u32[]
  statistics_field_ids:u32[]
  bloom_field_ids:u32[]

RowGroupIndexDirectory[row_group_count]
  offset:u64 | length:u64 | checksum:u32 | reserved:u32
```

Each row group has exactly one index block, written after its column chunks and before the next
row group. An empty index block is still present so the directory remains uniform. Index offsets
and lengths use `uint64_t`; each block has its own CRC32C. The existing file checksum also covers
all index blocks.

### Row-group index block

An index block is little-endian and contains:

```text
version:u16 = 1 | reserved:u16 = 0
statistics_count:u32 | bloom_count:u32 | sort_key_count:u32 | reserved:u32 = 0
StatisticsEntry[]
BloomEntry[]
SortKeyEntry[]
```

A statistics entry stores `field_id`, `null_count`, a `has_min_max` flag, and length-prefixed
canonical scalar bytes for min and max. Min/max exclude nulls. Floating columns containing any
NaN omit min/max, preventing unsafe pruning.

A Bloom entry stores `field_id`, bit count, hash count, and the bitset. Bloom filters contain only
non-null values. They use two stable 64-bit hashes over the physical type ID plus canonical scalar
bytes and derive the configured probes by double hashing. The phase-two writer uses seven probes
and a power-of-two bit count of at least 256 bits and approximately ten bits per row.

A sort-key entry stores one field ID and the first and last canonical scalar value in that row
group. Configured sort-key fields must be non-nullable, contain no null or NaN, and input must be
globally lexicographically non-decreasing across `Append` calls. These constraints make boundary
pruning deterministic without defining a null or NaN total order.

Canonical scalar bytes use the same physical representation as Plain values: explicit
little-endian fixed-width values, one byte for Boolean, raw bytes for string/binary, and timestamp
units inherited from the schema. Ordinary string/binary comparison is unsigned-byte
lexicographic. Floating predicate evaluation follows C++/Arrow comparison behavior; NaN disables
statistics pruning.

### IOPlan validation and semantics

- Projection field IDs are unique and preserve caller order. An empty projection produces
  zero-column batches with the correct filtered row count.
- Predicates are ANDed. Comparison predicates require a valid, non-null scalar whose Arrow type
  exactly equals the field type. `IS NULL` and `IS NOT NULL` carry no scalar.
- Sort-key bounds must match the configured key arity and exact types and cannot contain null or
  NaN.
- `output_batch_rows` must be positive. `limit = 0` returns an empty iterator.
- Missing statistics or Bloom metadata always degrades to scanning. Bloom filters are consulted
  only for equality predicates.

The scan iterator processes one row group at a time. It first prunes from indexes, then reads and
fully decodes only predicate columns. It builds a selection vector, reads projection-only chunks
only if rows remain, and decodes only selected values from those chunks. Output is sliced into the
requested batch size and `limit` is applied in logical row order.

`ScanMetrics` is an optional caller-owned observation object. It counts considered and pruned row
groups, chunk reads, decoded predicate/projection chunks, and chunk bytes read. Counters describe
completed iterator work, not merely iterator construction.

## Consequences

- Existing footer-v1 phase-one files remain readable and scan by sequential fallback.
- New writers emit footer v2 and self-contained row-group index blocks.
- Adding a new index representation requires a new block or footer version; readers never infer
  unknown layouts.
- Row-group indexes remain metadata-sized and are loaded during `Open`; column chunks remain lazy.
