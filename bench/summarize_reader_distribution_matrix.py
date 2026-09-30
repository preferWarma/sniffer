#!/usr/bin/env python3
"""Flatten raw matrix JSONL into one reproducible row per benchmark case."""

import argparse
import gzip
import json
import math
import pathlib
import statistics


FIELDS = (
    "cache_mode", "shape", "row_group_rows", "selectivity_percent",
    "projected_columns", "benchmark", "source_revision", "rows", "file_bytes",
    "p50_ms", "p95_ms", "min_ms", "cv_percent", "peak_rss_bytes",
    "arrow_pool_peak_bytes", "row_groups_pruned", "chunk_bytes_read",
    "candidate_column_bytes",
)


def percentile(samples, percent):
    samples = sorted(samples)
    position = (len(samples) - 1) * percent / 100
    lower = math.floor(position)
    upper = math.ceil(position)
    return samples[lower] + (samples[upper] - samples[lower]) * (position - lower)


def summarize(path):
    opener = gzip.open if path.suffix == ".gz" else pathlib.Path.open
    with opener(path, "rt", encoding="utf-8") as source:
        for line in source:
            cell = json.loads(line)
            payload = cell["result"]
            context = payload["context"]
            groups = {}
            for result in payload["benchmarks"]:
                if result.get("run_type") == "iteration":
                    groups.setdefault(result["name"], []).append(result)
            if len(groups) != 4:
                raise ValueError(f"expected four benchmarks in {path}")
            for name, results in groups.items():
                if len(results) != 7 or any(item.get("error_occurred") for item in results):
                    raise ValueError(f"invalid samples: {path} {name}")
                times = [entry["real_time"] for entry in results]
                if any(entry["time_unit"] != "ms" for entry in results):
                    raise ValueError(f"unexpected time unit: {path} {name}")
                representative = results[0]
                counter = lambda key: (
                    statistics.median(entry[key] for entry in results)
                    if key in representative else "NA"
                )
                yield {
                    "cache_mode": context["cache_mode"],
                    "shape": cell["shape"],
                    "row_group_rows": cell["row_group_rows"],
                    "selectivity_percent": cell["selectivity_percent"],
                    "projected_columns": cell["projected_columns"],
                    "benchmark": name,
                    "source_revision": context["source_revision"],
                    "rows": cell["rows"],
                    "file_bytes": representative["file_bytes"],
                    "p50_ms": round(statistics.median(times), 3),
                    "p95_ms": round(percentile(times, 95), 3),
                    "min_ms": round(min(times), 3),
                    "cv_percent": round(statistics.stdev(times) / statistics.mean(times) * 100, 2),
                    "peak_rss_bytes": counter("process_peak_rss_bytes"),
                    "arrow_pool_peak_bytes": counter("arrow_pool_peak_bytes"),
                    "row_groups_pruned": counter("row_groups_pruned"),
                    "chunk_bytes_read": counter("chunk_bytes_read"),
                    "candidate_column_bytes": counter("candidate_column_bytes"),
                }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    with args.output.open("x", encoding="utf-8") as output:
        output.write("\t".join(FIELDS) + "\n")
        for path in args.inputs:
            for row in summarize(path):
                output.write("\t".join(str(row[field]) for field in FIELDS) + "\n")


if __name__ == "__main__":
    main()
