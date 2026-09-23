# Sniffer Core v0.2：Parquet 对照初版

本记录从 2026-09-23 起使用 Parquet 作为文件格式对照。v1 的 Arrow IPC 数字保留在
[`BENCHMARK_V1.md`](BENCHMARK_V1.md)，两版的 Row Group、代码和测量口径不同，不能直接以表中
绝对值推导版本间提速。本报告尚未覆盖宽表、超过页缓存的数据集和 cold-cache，因此不是 v0.2
最终验收报告。

## 环境与口径

- Apple M4，10 逻辑 CPU；macOS arm64；AppleClang 21；Arrow/Parquet C++ 23.0.1；Release。
- 源码：`dea10762a375-dirty`，包含单列 sort-key 校验优化与 Parquet benchmark 改动。
- Google Benchmark 1.9.4，`--benchmark_repetitions=7 --benchmark_min_time=0.01s`；下表为
  7 次重复的 P50。输入构造、结果行数检查和文件大小查询不计入手工计时。
- Sniffer 使用其确定性编码选择器、索引和格式规定的 checksum。Parquet 使用默认 dictionary
  设置，分别关闭通用压缩或使用默认级别 ZSTD；Arrow 读写关闭多线程。Parquet 文件写入时持久化
  Arrow schema，并按与 Sniffer 相同的 4,096 行切分为 25 个 Row Group。
- 查询使用 Parquet Footer 中 `id` 的 min/max statistics 剪枝 Row Group，读取候选 Row Group 的
  必需列，再用类型化循环执行相同谓词和投影。Parquet 未启用 Bloom 或 Page Index，也未配置
  与 Sniffer 等价的 checksum；因此读取字节数、校验语义及内部内存分配不可视为完全同质指标。
- 临时文件位于系统临时目录；重复读取很可能命中页缓存，以下仅代表 warm-cache 合成场景。

复现固定性能场景：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^Performance/(Sniffer|Parquet|Parquet_ZSTD)/100000/4096/manual_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.01s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

复现六种压缩分布：

```sh
./build-release/sniffer_core_compression_benchmark \
  --benchmark_repetitions=7 --benchmark_min_time=0.01s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

## 固定性能场景

100,000 行，递增 `id`、32 个 `group` 值、每 17 行一个 null 的 `value`；谓词 `id >= 50000`，
投影 `id,value`，输出 50,000 行。

| 格式 | 写入 P50 ms | 扫描 P50 ms | 端到端 P50 ms | 端到端 M rows/s | 文件字节 | 剪枝 Row Group | 候选列块 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Sniffer | 4.477 | 0.927 | 5.431 | 18.41 | 663,679 | 12/25 | 26 |
| Parquet | 7.476 | 1.042 | 8.528 | 11.73 | 1,955,982 | 12/25 | 26 |
| Parquet + ZSTD | 10.035 | 1.375 | 11.459 | 8.73 | 514,667 | 12/25 | 26 |

三项端到端 wall-time CV 分别为 1.27%、1.02%、2.36%。Sniffer 实际读取 172,228 字节；Parquet
目前只记录候选列块数，不报告可比的实际 I/O 字节。相同配置下 Parquet + ZSTD 的文件比 Sniffer
小约 22.5%，而 Sniffer 的写入和该查询的端到端时间较短。

`SnifferThreePredicates`、`SnifferSortKeyRange` 与对应的 Parquet/Parquet + ZSTD case 均已注册；
三谓词输出 1,470 行，范围查询输出 50,000 行。性能矩阵还提供三种格式各自的 1K/8K/64K/256K
Row Group、1%/10%/50%/100% 选择率及 1/2/3 列投影 case；本初版尚未发布完整矩阵统计。

## 压缩专项

`logical_bytes` 为输入 Arrow RecordBatch 引用的去重 buffer 总大小。压缩比为
`logical_bytes / file_bytes`。每轮在计时结束后对完整回读结果逐 batch 验证值、null 与行序。

| 分布 | Sniffer 字节 / 比率 | Parquet 字节 / 比率 | Parquet + ZSTD 字节 / 比率 |
|---|---:|---:|---:|
| 递增 int64 | 153,882 / 5.20x | 955,917 / 0.84x | 259,206 / 3.09x |
| 窄值域 int64 | 91,594 / 8.73x | 119,073 / 6.72x | 14,565 / 54.93x |
| 长 RLE int64 | 7,166 / 111.64x | 7,140 / 112.04x | 6,995 / 114.37x |
| nullable 偏斜 int64 | 54,094 / 15.02x | 50,787 / 16.00x | 9,724 / 83.56x |
| 低基数字符串 | 117,044 / 9.99x | 77,567 / 15.07x | 9,948 / 117.49x |
| 高基数字符串 | 3,203,894 / 0.87x | 2,957,506 / 0.95x | 2,094,070 / 1.34x |

各场景编码写入 / 解码读取 P50（ms）：

| 分布 | Sniffer | Parquet | Parquet + ZSTD |
|---|---:|---:|---:|
| 递增 int64 | 1.410 / 0.441 | 2.655 / 0.468 | 3.456 / 0.904 |
| 窄值域 int64 | 0.890 / 0.394 | 1.132 / 0.269 | 1.286 / 0.290 |
| 长 RLE int64 | 0.453 / 0.307 | 0.957 / 0.173 | 1.005 / 0.184 |
| nullable 偏斜 int64 | 0.667 / 0.427 | 1.554 / 0.487 | 1.599 / 0.491 |
| 低基数字符串 | 1.619 / 1.443 | 2.282 / 0.809 | 2.299 / 0.848 |
| 高基数字符串 | 4.836 / 2.734 | 4.684 / 1.794 | 7.699 / 3.731 |

Parquet 未压缩的递增 int64 case 在该轮出现 35.6% wall-time CV；该 case 的耗时结论需要独立
复测。高基数字符串依然是 Sniffer 的空间弱项；其文件比未压缩 Parquet 还大，后续应优先评估
Plain 回退与 offset 开销。

## 待补齐

- 宽表、多列相关性、超页缓存数据、cold-cache 对照与完整矩阵汇总。
- 各格式更细的读取量和峰值内存定义；当前 RSS 是进程高水位，不是每个 case 的独立峰值。
- 使用相同源码状态与明确 v0.1 构建的回归复测，给出可信的相对 v1 数字。

## 后续增量测量

2026-09-23，提交 `2df58d6` 之后继续优化全非空变长列的 Plain 大小估算。低基数字符串
Sniffer case（100,000 行、Row Group 4,096）的写入 P50 从 1.533 ms（7 次）变为
1.379 ms（11 次），文件均为 117,044 字节。该数字与本报告初版表格不是同一轮实验；
改动范围、测试和限制见 [`PERFORMANCE_OPTIMIZATION_V0.2.md`](../docs/PERFORMANCE_OPTIMIZATION_V0.2.md)。
