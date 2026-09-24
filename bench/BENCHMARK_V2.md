# Sniffer Core v0.2：Parquet 对照初版

本记录从 2026-09-23 起使用 Parquet 作为文件格式对照。v1 的 Arrow IPC 数字保留在
[`BENCHMARK_V1.md`](BENCHMARK_V1.md)，两版的 Row Group、代码和测量口径不同，不能直接以表中
绝对值推导版本间提速。本报告已补 16 列宽表，但尚未覆盖超过页缓存的数据集和 cold-cache，因此不是 v0.2
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

复现当前七种压缩分布（下方初版表格保留当时的六种）：

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

- 宽表的多列相关性与宽投影、超页缓存数据、cold-cache 对照与完整矩阵汇总。
- 各格式更细的读取量和峰值内存定义；当前 RSS 是进程高水位，不是每个 case 的独立峰值。
- 使用相同源码状态与明确 v0.1 构建的回归复测，给出可信的相对 v1 数字。

## 后续增量测量

2026-09-23，提交 `2df58d6` 之后继续优化全非空变长列的 Plain 大小估算。低基数字符串
Sniffer case（100,000 行、Row Group 4,096）的写入 P50 从 1.533 ms（7 次）变为
1.379 ms（11 次），文件均为 117,044 字节。该数字与本报告初版表格不是同一轮实验；
改动范围、测试和限制见 [`PERFORMANCE_OPTIMIZATION_V0.2.md`](../docs/PERFORMANCE_OPTIMIZATION_V0.2.md)。

同日新增无谓词 full-scan case。100,000 行、Row Group 4,096、投影 `id,value`，Sniffer
scan P50 从 1.277 ms（7 次）降至 1.128 ms（11 次），文件大小及读取量不变。相同改动前
Parquet/Parquet + ZSTD full-scan scan P50 分别为 1.629/2.534 ms；改动后重测分别为
1.669/2.590 ms。两轮运行次数和温度条件不同，不能据此判断 Parquet 性能变化。

同日 Bloom 构建外提非浮点列的 NaN 类型检查。固定 50% 选择率场景的 Sniffer writer index
P50 从 1.210 ms 降至 1.156 ms（各 11 次）；完整端到端两轮结果方向不一致，暂不宣称
全路径提速。持久化字节、剪枝和读取量保持不变。

## 16 列宽表：首轮 warm-cache 对照

源码 `984e9cb-dirty`，Apple M4 / Release / 单线程，100,000 行，Row Group 4,096。
输入为 `id/group/value` 加 13 列按行号线性生成的非空 int64；谓词 `id >= 50000`，
投影 `id,value`。此场景只覆盖窄投影，不代表宽投影或大于页缓存的读取。
`--benchmark_repetitions=11 --benchmark_min_time=0.05s`，下表为 P50：

| 格式 | 写入 ms | 扫描 ms | 端到端 ms | 文件字节 | 剪枝 Row Group | 候选列块 |
|---|---:|---:|---:|---:|---:|---:|
| Sniffer | 22.297 | 1.305 | 23.599 | 3,104,713 | 12/25 | 26 |
| Parquet | 40.439 | 1.852 | 42.256 | 14,374,323 | 12/25 | 26 |
| Parquet + ZSTD | 59.152 | 2.405 | 61.443 | 3,869,950 | 12/25 | 26 |

三个端到端 wall-time CV 分别为 0.95%、0.96%、1.21%。Sniffer 实际读取 172,228 字节；
Parquet 未报告可比的物理读取字节。进程 RSS 为高水位而非 case 独立峰值，Arrow 分配统计也
受各格式内部实现影响，不能直接当作相同内存成本比较。复现：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceWide/(Sniffer|Parquet|Parquet_ZSTD)/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

## 采样偏斜整数：实际编码大小保护

2026-09-23，源码 `8895187-dirty`，Apple M4 / Release / 单线程；100,000 行、Row Group
4,096，每组前 1,024 行为 0、后续行互异。默认 1,024 行样本将 Sniffer 引向 RLE，
但完整 RLE payload 大于等价 Plain。新增实际 payload 大小检查后，仅在未强制指定编码且
编码结果不小于 Plain 时回退；文件格式和已有编码 ID 均不变。

| 格式/状态 | 写入 P50 ms | 读取 P50 ms | 文件字节 | 压缩比 |
|---|---:|---:|---:|---:|
| Sniffer，改动前 | 3.846 | 2.499 | 1,789,694 | 0.447x |
| Sniffer，改动后 | 3.550 | 0.833 | 803,694 | 0.995x |
| Parquet | 2.048 | 0.383 | 713,005 | 1.122x |
| Parquet + ZSTD | 2.980 | 0.754 | 194,328 | 4.117x |

改动前 Sniffer 和 Parquet 对照为 7 次重复，改动后 Sniffer 为 11 次重复，均使用
`--benchmark_min_time=0.03s`；改动后写入/读取 CV 为 0.77%/0.65%。原有六种
Sniffer 压缩场景的文件字节数全部保持不变。回退发生在非 Plain 编码已完成之后，
这不是最终的 CPU/解码成本模型；采样分布变化仍需更早识别以避免无效编码开销。

### 编码决策诊断计数

同日基于 `70d3b8a` 继续加入可选 writer 指标，并在 Sniffer 压缩基准中输出平均每次
写入的 chunk 计数及回退前后 payload 字节数。100,000 行、Row Group 4,096：

| 场景 | 直接 Plain | 保留非 Plain | 大小回退 | 强制编码 | 被弃 payload / Plain payload 字节 |
|---|---:|---:|---:|---:|---:|
| `sample_skew_int64` | 0 | 0 | 25 | 0 | 1,786,600 / 800,600 |
| `long_rle_int64` | 0 | 25 | 0 | 0 | 0 / 0 |

两个场景的文件字节数保持 803,694 / 7,166。改动前 7 次、改动后 11 次重复，
`--benchmark_min_time=0.03s`：偏斜场景写入 P50 3.610 / 3.582 ms，长 RLE 为
0.455 / 0.467 ms。重复次数不同、耗时差异较小，不据此宣称速度变化。当前计数解释
最终选用编码的路径，尚未解释选择器内部每个候选的估算值，也未评估 CPU/解码成本。

### 选择器采样 null 统计去重

同日基于 `88ce2b1`：标准 100,000 行、Row Group 4,096、50% 选择率场景，
11 次重复，`--benchmark_min_time=0.03s`。writer 编码选择 P50 1.028 → 0.998 ms，
写入 P50 4.283 → 4.233 ms，文件始终 663,679 字节。低基数字符串写入 P50
1.402 → 1.377 ms，文件始终 117,044 字节。高基数字符串结果波动较大，未形成
可归因的速度结论；本轮主要确认重复 bitmap 计数已消除、编码结果及文件格式未变。

### 无谓词整组扫描免建 selection

2026-09-24，基于 `43dd99b`，Apple M4 / Release / 单线程、100,000 行、Row Group
4,096、投影 `id,value`、无谓词、warm-cache、11 次重复、
`--benchmark_min_time=0.03s`。full-scan scan P50 改前 1.116 ms，改后两轮
1.068/1.091 ms；文件均为 663,679 字节，读取均为 50 个 ColumnChunk、
339,076 字节。改前 CV 2.35%，改后两轮 CV 1.22%/1.30%。该优化只在整个
Row Group 全命中且未被 limit 截断时绕过 selection；目前只视为方向性结果，
不据此宣称稳定提速。相同代码状态的过滤扫描 P50 为 0.930 ms。

### 输出 batch 对齐诊断（Sniffer-only）

2026-09-24，Apple M4 / Release / 单线程、warm-cache、100,000 行、Row Group 4,096、
无谓词、投影 `id,value`。两种 case 使用相同输入及文件，仅输出 batch 大小不同；
各独立运行 11 次、`--benchmark_min_time=0.03s`，均报告 P50：

| case | 输出 batch 行数 | scan ms | batch materialization ms | Arrow allocations | 文件字节 |
|---|---:|---:|---:|---:|---:|
| `Performance/SnifferFullScan/100000/4096` | 2,049 | 1.052 | 0.0449 | 172 | 663,679 |
| `PerformanceAligned` | 4,096 | 1.011 | 0.0038 | 100 | 663,679 |

scan CV 分别为 2.12%/2.20%。对齐 case 是 Sniffer 的 batch 边界诊断，**不是**
与 Parquet 的格式对比，也不能把不同输出规格的耗时差当作代码优化收益。曾试验
单组输出时跳过 `Slice()`，同计数条件下基线/快路径 P50 为 1.041/1.039 ms，
快路径第二轮为 1.075 ms；未见稳定收益，故未保留。

复现：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^(PerformanceAligned|Performance/SnifferFullScan/100000/4096)/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### 谓词全命中 Row Group 的隐式连续范围

2026-09-25，Apple M4 / Release / 单线程 / warm-cache，100,000 行、Row Group 8,192、
升序 `id >= threshold`、投影 `id,value`；1%/10%/50%/100% 选择率各 11 次，
`--benchmark_min_time=0.03s`。下表为同一批新增计数存在时的改前/改后 P50；
`selection bytes` 是显式行号数量乘 8，不是 Arrow 内存池分配量。

| 选择率 | scan 改前 / 改后 ms | 改后 CV | selection bytes 改前 / 改后 | 文件字节 |
|---:|---:|---:|---:|---:|
| 1% | 0.244 / 0.243 | 1.64% | 8,000 / 8,000 | 675,943 |
| 10% | 0.424 / 0.395 | 1.46% | 80,000 / 896 | 675,943 |
| 50% | 0.940 / 0.817 | 1.73% | 400,000 / 58,752 | 675,943 |
| 100% | 1.652 / 1.347 | 1.25% | 800,000 / 0 | 675,943 |

完整命中的 Row Group 才不生成行号；部分命中、`limit` 截断依然走现有 selected-decode。
该有序阈值分布便于出现整组命中，不能外推到随机稀疏过滤。复现：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceMatrix/Sniffer/rows:100000/row_group_rows:8192/selectivity_percent:(1|10|50|100)/projection_columns:2/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```
