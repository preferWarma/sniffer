# Decision 0006: Opt-in macOS file-cache bypass for scan measurement

- Status: Implemented as optional v0.2 measurement path
- Date: 2026-09-30

## Context

The Reader-only benchmark runs each case in a fresh process, but repeated scans of the same
file are not cold-cache measurements. Purging the machine-wide cache would disturb unrelated
work, and page-cache eviction hints cannot prove that subsequent reads hit physical storage.
We need a reproducible I/O-sensitive comparison without changing the Segment format or default
Reader behavior.
Apple documents [`F_NOCACHE`](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/FileSystem/Articles/FilePerformance.html)
as a per-descriptor way to disable data caching.

## Decision

- Add an explicit `OpenWithOptions(path, ReaderOpenOptions, metrics)` entry point. The existing
  `Open(path, metrics)` remains unchanged and uses the OS default. On macOS pread builds, set `F_NOCACHE`
  on the Reader's own read-only file descriptor before reading the envelope. On unsupported
  platforms or the forced stream-I/O fallback, return `NotImplemented` instead of silently
  ignoring the request.
- The Parquet Reader-only benchmark sets `F_NOCACHE` on an Arrow `ReadableFile` descriptor
  before handing it to the Parquet reader. Both formats use the same benchmark input and plan.
- Label results **cache-bypass**, never cold-cache. This option neither evicts already-resident
  pages nor controls device, filesystem, or storage-controller caches. It is not a process RSS
  bound. Do not check off the v0.2 cold-cache/over-page-cache acceptance item on this basis.
- Verify that default and bypass readers produce identical batches and checksums. Keep the
  benchmark option scan-only and add platform-gated smoke coverage.

## Consequences

The option adds a small, explicit public API surface but no format-version or file-byte change.
It can expose an I/O-sensitive regime on macOS without a destructive system-wide purge. Linux
and stream-I/O builds can still run the default benchmark; their bypass request fails explicitly.
True cold-cache validation remains a separate, controlled experiment.
