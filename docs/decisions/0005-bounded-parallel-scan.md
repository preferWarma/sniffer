# Decision 0005: Opt-in, bounded parallel Scan execution

- Status: Accepted for v0.2 implementation; no parallel Reader API exists yet
- Date: 2026-09-28

## Context

The existing `Scan(IOPlan, metrics)` returns a lazy, single-threaded Arrow batch iterator.
Independent iterators may read one immutable Segment concurrently, but this is not internal
parallelism for one request. On a 10-million-row sorted input, an experimental application-level
range sharding of one query reached about 3.0 ms with eight workers versus about 9.5 ms with one;
on 100,000 rows, adding workers was slower. This is evidence to try opt-in internal parallelism,
not evidence that it is safe for every plan. In particular, unbounded prefetch can change `limit`
early stopping, read a corrupt future chunk before it is needed, and make memory grow with the
whole file. `IOPlan` v0.1 is intentionally limited to logical query semantics.

## Decision

- Preserve the existing `Scan(IOPlan, metrics)` behavior and default of no background threads.
  Put execution knobs in a separate, opt-in `ScanExecutionOptions` argument/overload, **not** in
  `IOPlan` or the Segment format. Validate nonzero worker, in-flight Row Group, and buffered-byte
  budgets before creating an iterator. A one-worker request uses the existing serial path.
- Schedule physical Row Groups, not synthetic sort-key subqueries. Each task follows the current
  index pruning → predicate-column decode → selection → projected-column decode sequence. This
  supports unsorted Segments and avoids re-reading keys merely to partition work.
- Start workers only on the first `Next()`. Keep at most the configured number of Row Groups in
  flight, and at most the configured budget of completed output buffers waiting for emission.
  One active Row Group per worker is the other hard envelope. A Row Group that cannot fit the
  conservative scheduling estimate uses the serial path; the byte budget is **not** a promise
  about whole-process RSS or Arrow allocator overhead. Report observed RSS separately.
- Emit results in original Row Group and row order, then apply `output_batch_rows` exactly as the
  serial iterator does. For v0.2's first parallel path, any `IOPlan.limit` falls back to serial
  execution: it must retain exact early-stop behavior and must not read future chunks merely to
  fill a worker queue. Small/empty inputs can also fall back without changing query results.
- Worker results and errors are staged by Row Group position. Surface the first relevant error
  in logical order as a structured `Status`, stop dispatching, cancel queued work, and join all
  workers before iterator destruction completes. Dropping an unlimited iterator may leave up to
  the configured in-flight tasks already started; it must not schedule new work after drop.
- Keep each worker's scan metrics private and aggregate them without data races. Public `Next()`
  remains single-caller; independent iterators remain concurrently usable. No exception may
  escape the public library API, including worker creation failure.

## Acceptance before enabling the parallel path

- Compare serial and parallel Arrow batches field-by-field for 1/2/4/8 workers across sorted and
  unsorted files, missing optional indexes, projection disjoint from filters, nullable and
  variable-width data, multiple Row Groups, empty output, and corrupt candidate/pruned chunks.
- Verify Row Group order, batch size, exact `limit` fallback and early stop, bounded in-flight
  count/buffers, destruction during an unfinished scan, error/cancellation propagation, and TSan.
- Measure single-request P50/P95, read bytes, allocation and independent-process RSS at 100K and
  10M rows against the serial path and a same-plan Parquet baseline. If the added complexity or
  memory cost outweighs the large-request gain, keep the opt-in path disabled and record why.

## Consequences

The first implementation may be faster only for large scans and may use more memory proportional
to its explicit worker/in-flight budgets. The existing API and file bytes remain unchanged.
Until the acceptance checks pass, this record is an execution contract, **not** a claim that
single-request parallel Scan is implemented or that v0.2 is complete.
