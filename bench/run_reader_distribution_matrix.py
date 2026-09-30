#!/usr/bin/env python3
"""Run the reproducible reader-only cross-distribution matrix.

Output is append-only JSONL, one complete Google Benchmark result per cell.
The input directory is created uniquely and never overwritten or deleted here.
"""

import argparse
import json
import pathlib
import subprocess
import tempfile


SHAPES = (
    ("legacy-int", "legacy", 0),
    ("high-entropy-binary", "legacy", 32),
    ("low-cardinality-string", "low-cardinality-string", 0),
    ("variable-string", "variable-string", 0),
    ("long-run-int", "long-run-int", 0),
    ("narrow-int", "narrow-int", 0),
)
ROW_GROUPS = (1024, 8192, 65536, 262144)
SELECTIVITIES = (1, 10, 50, 100)
PROJECTIONS = (1, 4)
def invoke(argv, *, json_output=False, repetitions=7):
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {argv}\n{result.stdout}\n{result.stderr}")
    if not json_output:
        return None
    payload = json.loads(result.stdout)
    if len([entry for entry in payload["benchmarks"] if entry.get("run_type") == "iteration"]) != 4 * repetitions:
        raise RuntimeError(f"missing iterations: {argv}\n{result.stdout}\n{result.stderr}")
    errors = [entry for entry in payload["benchmarks"] if entry.get("error_occurred")]
    if errors:
        raise RuntimeError(f"benchmark errors: {errors}")
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--min-time", default="0.01s")
    parser.add_argument("--shapes", nargs="*", default=[x[0] for x in SHAPES])
    parser.add_argument("--row-groups", type=int, nargs="*", default=ROW_GROUPS)
    parser.add_argument("--selectivities", type=int, nargs="*", default=SELECTIVITIES)
    parser.add_argument("--projections", type=int, nargs="*", default=PROJECTIONS)
    parser.add_argument("--existing-data-dir", type=pathlib.Path)
    parser.add_argument("--cache-bypass", action="store_true")
    parser.add_argument("--skip-preflight", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; refusing to overwrite or mix runs")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data_dir = args.existing_data_dir or pathlib.Path(
        tempfile.mkdtemp(prefix="sniffer-distribution-matrix-"))
    if args.existing_data_dir and not data_dir.is_dir():
        parser.error("existing data directory does not exist")
    print(f"data_dir={data_dir}", flush=True)
    shape_map = {item[0]: item for item in SHAPES}
    selected = [shape_map[name] for name in args.shapes]
    total = len(selected) * len(args.row_groups) * len(args.selectivities) * len(args.projections)
    completed = 0
    with args.output.open("x", encoding="utf-8") as output:
        for name, shape, width in selected:
            for row_group in args.row_groups:
                stem = f"{name}-rg{row_group}"
                paths = {
                    "segment": data_dir / f"{stem}.seg",
                    "parquet": data_dir / f"{stem}.parquet",
                    "parquet-zstd": data_dir / f"{stem}.zstd.parquet",
                }
                common = [f"--rows={args.rows}", f"--row-group-rows={row_group}",
                          "--projected-columns=4", f"--value-shape={shape}"]
                if width:
                    common.append(f"--projected-binary-bytes={width}")
                if not args.existing_data_dir:
                    for mode, path in paths.items():
                        invoke([str(args.exe), f"--generate{'' if mode == 'segment' else '-' + mode}={path}", *common])
                elif not all(path.is_file() for path in paths.values()):
                    raise RuntimeError(f"missing generated files for {stem} in {data_dir}")
                sizes = {key: path.stat().st_size for key, path in paths.items()}
                for selectivity in args.selectivities:
                    for projection in args.projections:
                        argv = [str(args.exe), f"--segment={paths['segment']}",
                                f"--parquet={paths['parquet']}",
                                f"--parquet-zstd={paths['parquet-zstd']}",
                                f"--rows={args.rows}", f"--row-group-rows={row_group}",
                                f"--projected-columns={projection}",
                                f"--selectivity-percent={selectivity}",
                                f"--value-shape={shape}",
                                f"--benchmark_repetitions={args.repetitions}",
                                f"--benchmark_min_time={args.min_time}",
                                "--benchmark_format=json",
                                "--benchmark_filter=^(ReaderOnlyScan|BoundedParallelReaderScan/4|ParquetReaderOnlyScan/Uncompressed|ParquetReaderOnlyScan/ZSTD)/real_time$"]
                        if width:
                            argv.append(f"--projected-binary-bytes={width}")
                        if args.cache_bypass:
                            argv.append("--cache-bypass")
                        if args.skip_preflight:
                            argv.append("--skip-preflight")
                        payload = invoke(argv, json_output=True, repetitions=args.repetitions)
                        output.write(json.dumps({"shape": name, "rows": args.rows,
                                                 "row_group_rows": row_group,
                                                 "selectivity_percent": selectivity,
                                                 "projected_columns": projection,
                                                 "file_bytes": sizes,
                                                 "data_dir": str(data_dir),
                                                 "cache_bypass": args.cache_bypass,
                                                 "skip_preflight": args.skip_preflight,
                                                 "command": argv,
                                                 "result": payload}, separators=(",", ":")) + "\n")
                        output.flush()
                        completed += 1
                        print(f"{completed}/{total} {name} RG={row_group} selectivity={selectivity}% projection={projection}", flush=True)
    print(f"results={args.output}", flush=True)


if __name__ == "__main__":
    main()
