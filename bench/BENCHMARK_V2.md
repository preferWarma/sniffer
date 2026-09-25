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

### 输出数组与 batch 的重复深度校验

2026-09-25，Apple M4 / Release / 单线程 / warm-cache；100,000 行、Row Group 8,192、
100% 选择率，投影 `id,group,value`，其中 `group` 为 string。解码/选择器各自确保
输出数组通过 `ValidateFull()` 后，输出 `RecordBatch` 只做结构校验 `Validate()`；
与原先 batch 再次逐列深度校验相比，11 次重复 P50 的 batch materialization
约 0.291 → 0.058 ms，scan 约 2.763 → 2.605 ms，第二轮 2.633 ms。
相同文件均为 675,943 字节。两列数值投影没有确认收益，不外推到其他分布。

同一三列场景修改前的 chunk I/O P50 为 0.127 ms，decode 为 1.799 ms，
scan 为 2.798 ms。相邻 chunk 合并读取的 warm-cache 上界较低，尚未测试 cold-cache。

### selection 表示的独立微基准

2026-09-25，Apple M4 / Release / 单线程，100,000 行。预先生成确定性 mask 和
values；计时仅含表示的构建与按命中行求和，不含谓词、codec、文件 I/O、Arrow builder。
`distribution:0` 是 hash 均匀散点，`:1` 是连续前缀；每 case 11 次重复、
`--benchmark_min_time=0.03s`。P50 单位为 µs：

| 分布 | 选择率 | 64-bit 行号 | bitmap | ranges |
|---|---:|---:|---:|---:|
| 均匀散点 | 1% | 31.2 | 28.6 | 33.7 |
| 均匀散点 | 10% | 47.9 | 36.5 | 52.6 |
| 均匀散点 | 50% | 178.1 | 104.9 | 221.6 |
| 均匀散点 | 100% | 62.3 | 157.6 | 74.1 |
| 连续前缀 | 1% | 24.0 | 24.8 | 24.7 |
| 连续前缀 | 10% | 28.5 | 38.0 | 28.9 |
| 连续前缀 | 50% | 42.9 | 92.0 | 48.7 |
| 连续前缀 | 100% | 62.4 | 157.1 | 74.3 |

这份诊断的行号 vector 按当前扫描逻辑预留整组容量：800,000 字节；bitmap 容量
12,504 字节。连续前缀只有一个 range、容量 16 字节；均匀散点的 range 容量
取决于连续命中的段数，例如 50% 为 524,288 字节。均匀 hash 的实际命中数与
目标百分比略有差异，按 benchmark JSON 中的 `selected_rows` 计。

bitmap 在均匀中高选择率下的时间和空间较好，但生产 scan 仍使用 index vector；
`limit`、selected-decode 和 Arrow 输出成本尚未计入，暂不设生产切换阈值。
当前全命中 Row Group 已用隐式连续表示，不应再构造上表的任一容器。复现：

```sh
./build-release/sniffer_core_selection_benchmark \
  '--benchmark_filter=^Selection/' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### Plain selected-decode 的 bitmap A/B

2026-09-25，Apple M4 / Apple Clang 21 / Arrow 23.0.1 / Release / 单线程 / warm-cache。
100,000 个 nullable int64，17 行中约 1 行为 null，非 null 值为确定性 hash；单个
Plain ColumnChunk，无 Row Group 边界。均匀散点与连续前缀各测 1%/10%/50%/100%；
11 次重复、每次至少 0.05 秒，下表为 CPU 时间 P50，单位 µs。计时从预建的选择
表示和已编码 payload 开始，包含完整 Plain selected-decode、Arrow builder 输出及
`ValidateFull()`，不含谓词、选择表示构建、I/O、CRC 和 batch 拼接。每个 case 在
计时前用两个解码器逐值核对。

| 分布 | 选择率 | 行号 | bitmap | bitmap / 行号 |
|---|---:|---:|---:|---:|
| 均匀散点 | 1% | 22.51 | 23.86 | 1.06 |
| 均匀散点 | 10% | 45.70 | 43.35 | 0.95 |
| 均匀散点 | 50% | 143.67 | 134.92 | 0.94 |
| 均匀散点 | 100% | 248.24 | 211.02 | 0.85 |
| 连续前缀 | 1% | 22.29 | 22.82 | 1.02 |
| 连续前缀 | 10% | 42.92 | 40.35 | 0.94 |
| 连续前缀 | 50% | 134.45 | 115.15 | 0.86 |
| 连续前缀 | 100% | 248.40 | 209.62 | 0.84 |

bitmap 容量为 12,504 字节；本 case 的行号 vector 容量从约 8 KiB（1% 连续前缀）
到 1 MiB（100%），是 benchmark 构建方式的实际容量，不等于生产 scan 的整组预留。
1% 解码略慢；中高选择率 Plain 解码较快，但这里未测 bitmap 构建、谓词列复用和
Dictionary/RLE/FOR 的 selected-decode，不能据此设生产阈值。生产全命中 Row Group
原本就不分配 selection，100% 一行只用于比较两个 decoder。复现：

```sh
./build-release/sniffer_core_selection_benchmark \
  '--benchmark_filter=^PlainSelectedDecode/' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### 连续谓词投影的零拷贝 slice

2026-09-25，Apple M4 / Apple Clang 21 / Arrow 23.0.1 / Release / 单线程 / warm-cache。
100,000 行，升序 `id`，按 `id >= threshold` 过滤且仅投影该谓词列；
`output_batch_rows = row_group_rows / 2 + 1`。每 case 7 次、至少 0.03 秒，
下表是 scan 耗时 P50（ms）。baseline 为提交 `40f1f2f`，改后独立跑两轮；
投影阶段计时包含指标开销，微秒级数值只用于定位瓶颈。

| Row Group | 选择率 | baseline | 改后第一轮 | 改后第二轮 | 文件字节 |
|---:|---:|---:|---:|---:|---:|
| 8,192 | 10% | 0.348 | 0.345 | 0.343 | 675,943 |
| 8,192 | 50% | 0.604 | 0.583 | 0.577 | 675,943 |
| 65,536 | 10% | 0.416 | 0.400 | 0.394 | 732,101 |
| 65,536 | 50% | 0.809 | 0.792 | 0.795 | 732,101 |

原先对部分命中组会逐值复制已解码的谓词列；现在连续 selection 直接保留其 Arrow
buffer，scan 的 projection 阶段在上述场景约从 0.0007–0.034 ms 降至
0.00007–0.00012 ms。完整 scan 收益较小，因为谓词、I/O 和 batch 拼接未变。
非连续选择仍沿用原 typed 路径；不改变 Row Group 剪枝、null、limit 和输出
batch 大小。此处有序命中易形成连续 selection，不能外推随机散点过滤。
复现：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceMatrix/Sniffer/rows:100000/row_group_rows:(8192|65536)/selectivity_percent:(10|50)/projection_columns:1/manual_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### bitmap 完整扫描 A/B（默认切换未采纳）

2026-09-25，Apple M4 / Apple Clang 21 / Arrow 23.0.1 / Release / 单线程 / warm-cache。
`FullScanSelection` 构造真实 Segment：100,000 行、Row Group 8,192、boolean
谓词列与 nullable int64 Plain 投影列，约每 17 行有 1 个 null；无索引剪枝，
`output_batch_rows=8192`。mask 是确定性 hash 均匀散点或连续前缀。
计时包含扫描、选择构建、ColumnChunk I/O/CRC、Plain 解码、Arrow 输出及
逐值消费，不含一次性写入；预检与独立 mask 参考核对行数、null、求和和
读取 chunk 数。两种表示处理的文件及读取 chunk 数完全相同。

下表为交错 index/bitmap/index/bitmap 四轮，每轮 11 次、至少 0.05 秒的
CPU P50，单位 ms。bitmap 是临时实验接入：仅 Plain 投影、无 `limit` 截断，
行号字节超过 Row Group bitmap 字节后切换；实验后已从生产代码撤回。

| 分布 | 选择率 | index 两轮 | bitmap 两轮 | bitmap 逻辑表示字节 |
|---|---:|---:|---:|---:|
| 均匀散点 | 10% | 0.965 / 0.990 | 0.986 / 0.995 | 13,840 |
| 均匀散点 | 50% | 1.723 / 1.739 | 1.762 / 1.795 | 19,344 |
| 连续前缀 | 10% | 0.662 / 0.668 | 0.654 / 0.659 | 1,024 |
| 连续前缀 | 50% | 0.984 / 0.994 | 0.984 / 0.987 | 1,024 |

作为对照，纯 index 路径的均匀 10%/50% 分别持有 10,083/50,051 个逻辑行号，
即 80,664/400,408 字节；bitmap 逻辑字节不含转换期间的临时 vector 或 allocator
开销。吞吐上均匀 10% 未稳定改善，50% 两轮均回退约 2%–3%；连续前缀收益较小。
故本轮不设置生产切换阈值。当前仓库保留的是 index 版本的完整扫描基准，
临时 bitmap 接入未保留，表中的 bitmap 数据是实验记录，不能直接由当前
默认构建复跑。index 路径复现：

```sh
./build-release/sniffer_core_selection_benchmark \
  '--benchmark_filter=^FullScanSelection/' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### 高基数字符串的 CompactPlain 格式 A/B

2026-09-25，Apple M4 / Apple Clang 21 / Arrow C++ 23.0.1 / Release / 单线程 /
warm-cache。100,000 行，每个字符串 24 字节，Row Group 4,096（25 个 chunk），
无查询谓词、选择率不适用；Arrow logical bytes 为 2,800,004。每个 case 11 次重复、
每次至少 0.03 秒；下表为两轮独立测量的完整写入/回读 CPU 时间 P50（ms），
包括文件及 checksum，不含输入构造或正确性比较。`Sniffer` 为自适应选择；
`SnifferPlain` / `SnifferCompact` 强制编码，便于单独归因新格式的成本。

| 格式 | 文件字节 | 压缩比 | 写入 P50 两轮 | 读取 P50 两轮 |
|---|---:|---:|---:|---:|
| Sniffer，自适应 CompactPlain | 2,803,815 | 0.999x | 4.220 / 4.002 | 2.491 / 2.480 |
| SnifferPlain，旧 64-bit offset | 3,203,894 | 0.874x | 3.679 / 3.490 | 2.656 / 2.683 |
| SnifferCompact，32-bit offset | 2,803,815 | 0.999x | 3.067 / 3.034 | 2.499 / 2.473 |
| Parquet，未压缩 | 2,957,506 | 0.947x | 4.542 / 4.523 | 1.704 / 1.711 |
| Parquet + ZSTD | 2,094,070 | 1.337x | 7.583 / 7.536 | 3.753 / 3.731 |

新编码相对强制旧 Plain 节省 400,079 字节（12.49%），文件比未压缩 Parquet
小约 5.2%；达到 0.98x 空间目标。强制 CompactPlain 的写入和读取均比强制 Plain
快，但自适应写入包含采样选择成本，不能把强制编码的收益直接外推为默认写入
提速。Parquet 未压缩读取仍明显更快，ZSTD 文件仍更小。低基数字符串仍选择
Dictionary，文件保持 117,044 字节。旧 Plain 文件继续可读；新编码使用 ID 4，
与 Plain v1 不共享字节语义，见
[`docs/decisions/0004-compact-plain-variable-offsets.md`](../docs/decisions/0004-compact-plain-variable-offsets.md)。

复现（构建和输入参数沿用本报告的 Release 配置）：

```sh
./build-release/sniffer_core_compression_benchmark \
  '--benchmark_filter=^Compression/high_cardinality_string_(Sniffer|SnifferPlain|SnifferCompact|Parquet|Parquet_ZSTD)/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### Plain 全非空变长列直接写最终 payload

2026-09-25，Apple M4 / Apple Clang 21 / Arrow 23.0.1 / Release / 单线程 / warm-cache。
`Compression/high_cardinality_string_Sniffer`：100,000 行、Row Group 4,096，25 个
chunk 均自适应选择 Plain。11 次重复、每次至少 0.03 秒；完整编码写入 P50 从
4.551 ms（改前）到 4.519 ms（改后），文件均为 3,203,894 字节，压缩比均为
0.874x。另一次独立的改后 21 次重复、每次至少 0.05 秒，P50 为 4.542 ms，
写入耗时 CV 12.4%。首次 A/B 差异约 0.7%，处于波动范围内，**不宣称端到端提速**。
实现减少了 offsets/values 中间 buffer 到最终 payload 的拷贝与分配；持久化字节
及 nullable 路径未变。切片、空数组和含 `0x00`/`0xff` 的 binary 与逐行参考编码
逐字节比较，并通过 Release 全量测试及 ASan 相关测试。复现完整写入测量：

```sh
./build-release/sniffer_core_compression_benchmark \
  '--benchmark_filter=^Compression/high_cardinality_string_Sniffer/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

### 扫描计划的谓词列紧凑槽位

2026-09-25，Apple M4 / Apple Clang 21 / Arrow 23.0.1 / Release / 单线程 / warm-cache。
100,000 行，确定性递增 `id`、32 个 `group` 值、nullable `value`；下表为
Sniffer scan P50（ms），每轮 11 次重复、至少 0.05 秒。改前逐 Row Group 建立
`unordered_map`；改后在 `IOPlan` 校验时为谓词和排序键列分配紧凑槽位，逐组只
分配对应数量的指针。三谓词 case 用 4,096 行 Row Group、50% 的第一谓词
选择率，输出 1,470 行；矩阵 case 用 1,024 行 Row Group，输出列数与选择率
见表。扫描计时包含 Reader Open、块读取与 CRC、解码、谓词、输出 batch；
不含本轮写入时间。

| 场景 | 改前 | 改后第一轮 | 改后第二轮 | ColumnChunk 读取数 |
|---|---:|---:|---:|---:|
| 三谓词，2 列投影 | 1.576 | 1.542 | 1.537 | 39 |
| 50% 命中，3 列投影 | 1.878 | 1.837 | 1.820 | 150 |
| 100% 命中，3 列投影 | 3.351 | 3.231 | 3.244 | 294 |

各 case 的输出行数、读取块数、拼接次数均不变；当前观察到约 2%–4% 的
扫描耗时下降，但这不是跨机器承诺，也没有解决跨组 Arrow 数组拼接。
额外试过对齐输出直接复用完整 `RecordBatch`：虽然 25 次 output slice 降为零，
100,000 行 / Row Group 4,096 / 两列无谓词全扫的 P50 从 1.017 ms 变成
1.037/1.051 ms，故撤回该路径。当前保持既有 batch 边界与拼接语义。
复现紧凑槽位测量：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceMatrix/Sniffer/rows:100000/row_group_rows:1024/selectivity_percent:(50|100)/projection_columns:3/manual_time$|^Performance/SnifferThreePredicates/100000/4096/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```
