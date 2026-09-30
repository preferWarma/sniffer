#!/usr/bin/env python3
"""Measure first read, warmed read and F_NOCACHE read of an over-RAM corpus.

This does not clear the global macOS cache; first-read is a best-effort proxy,
not proof of physical cold-cache. The corpus and raw JSONL are retained.
"""

import argparse
import json
import pathlib
import subprocess
import tempfile


CASES = (
    ("segment", "ReaderOnlyScan/real_time"),
    ("parquet", "ParquetReaderOnlyScan/Uncompressed/real_time"),
    ("parquet-zstd", "ParquetReaderOnlyScan/ZSTD/real_time"),
)


def run(argv, json_output=False):
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {argv}\n{result.stdout}\n{result.stderr}")
    if json_output:
        payload = json.loads(result.stdout)
        measured = [row for row in payload["benchmarks"] if row.get("run_type") == "iteration"]
        if len(measured) != 1 or measured[0].get("error_occurred") or measured[0]["iterations"] != 1:
            raise RuntimeError(f"invalid single-scan result: {result.stdout}\n{result.stderr}")
        return payload
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--rows", type=int, default=50_000_000)
    parser.add_argument("--row-group-rows", type=int, default=65_536)
    parser.add_argument("--selectivity-percent", type=int, default=50)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--physical-ram-bytes", type=int, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; refusing to overwrite")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data_dir = pathlib.Path(tempfile.mkdtemp(prefix="sniffer-over-ram-cache-"))
    print(f"data_dir={data_dir}", flush=True)
    paths = {"segment": data_dir / "data.seg", "parquet": data_dir / "data.parquet",
             "parquet-zstd": data_dir / "data.zstd.parquet"}
    common = [f"--rows={args.rows}", f"--row-group-rows={args.row_group_rows}",
              "--projected-binary-bytes=32", "--projected-columns=4"]
    for mode, path in paths.items():
        generate = f"--generate{'' if mode == 'segment' else '-' + mode}={path}"
        run([str(args.exe), generate, *common])
        print(f"generated {mode}: {path.stat().st_size} bytes", flush=True)
    sizes = {name: path.stat().st_size for name, path in paths.items()}
    if sum(sizes.values()) <= args.physical_ram_bytes:
        raise RuntimeError("corpus does not exceed physical RAM")
    with args.output.open("x", encoding="utf-8") as output:
        for name, benchmark_name in CASES:
            base = [str(args.exe), f"--segment={paths['segment']}",
                    f"--parquet={paths['parquet']}",
                    f"--parquet-zstd={paths['parquet-zstd']}", *common,
                    f"--selectivity-percent={args.selectivity_percent}",
                    "--skip-preflight", "--benchmark_min_time=1x",
                    "--benchmark_repetitions=1", "--benchmark_format=json",
                    f"--benchmark_filter=^{benchmark_name}$"]
            for sample in range(1 + args.repetitions * 2):
                # Alternate modes so thermal drift is not confounded with cache mode.
                label = "first_read" if sample == 0 else "warm" if sample % 2 else "F_NOCACHE"
                argv = [*base, *(["--cache-bypass"] if label == "F_NOCACHE" else [])]
                payload = run(argv, json_output=True)
                output.write(json.dumps({"format": name, "mode": label, "sample": sample,
                                         "rows": args.rows,
                                         "row_group_rows": args.row_group_rows,
                                         "selectivity_percent": args.selectivity_percent,
                                         "file_bytes": sizes, "corpus_bytes": sum(sizes.values()),
                                         "physical_ram_bytes": args.physical_ram_bytes,
                                         "data_dir": str(data_dir), "command": argv,
                                         "result": payload}, separators=(",", ":")) + "\n")
                output.flush()
                print(f"{name} {label} {sample}: {payload['benchmarks'][0]['real_time']:.3f} ms", flush=True)
    print(f"results={args.output}", flush=True)


if __name__ == "__main__":
    main()
