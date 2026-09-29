# Sniffer Core v0.2：基准与 Parquet 对照（进行中）

本记录从 2026-09-23 起使用 Parquet 作为文件格式对照。v1 的 Arrow IPC 数字保留在
[`BENCHMARK_V1.md`](BENCHMARK_V1.md)，两版的 Row Group、代码和测量口径不同，不能直接以表中
绝对值推导版本间提速。本报告已补 16 列宽表、同机旧版对照和完整 warm-cache
参数矩阵；尚未覆盖超过页缓存的数据集和可信 cold-cache，因此不是 v0.2 最终验收报告。

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
Row Group、1%/10%/50%/100% 选择率及 1/2/3 列投影 case；完整 warm-cache
矩阵见文末和 [`V0.2_MATRIX_2026-09-28.tsv`](V0.2_MATRIX_2026-09-28.tsv)。

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

- 宽表的多列相关性与宽投影、超页缓存数据和 cold-cache 对照。
- 各格式可比的物理读取字节；下文旧场景的 RSS 是写入＋扫描同进程的
  high-water，文末新增 Reader-only 双进程基准，但仍只覆盖 warm-cache。
- 更广的旧版→当前矩阵复测；下文仅给出相同输入/Row Group 的固定场景与六种压缩分布。

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

### 变长列字典选择的下界提前终止

2026-09-25，Apple M4、Arrow C++ 23.0.1、Apple Clang 21、Release、单线程、
warm-cache；100,000 行、Row Group 4,096、每组最多采样 1,024 行；压缩写入不涉及
查询选择率。高基数字符串为确定性的 24 字节值，低基数字符串有 32 种值。
每 case 11 次、最短 0.03 秒，下表为完整写入与内部编码选择计时的 P50（ms）；
基线为提交 `cfaae72`，改后两轮独立测量。`encoding_selection_ms` 是可选 writer
指标，包含采样大小估算及候选编码判断，不含最终编码、索引和文件写入。
这三次运行复用了先前配置的构建目录，输出中的 `source_revision` 构建常量
仍显示旧值 `c2b96f7-dirty`；实际源码版本以本段的 baseline/改后说明为准。

| 数据 | 指标 | 基线 | 改后第 1 轮 | 改后第 2 轮 | 文件字节 |
|---|---|---:|---:|---:|---:|
| 高基数字符串 | 编码选择 | 0.947 | 0.816 | 0.820 | 2,803,815 |
| 高基数字符串 | 完整写入 | 4.030 | 3.847 | 3.955 | 2,803,815 |
| 低基数字符串 | 编码选择 | 0.267 | 0.274 | 0.284 | 117,044 |
| 低基数字符串 | 完整写入 | 1.307 | 1.317 | 1.370 | 117,044 |

高基数字符串的选择阶段两轮约减少 13%–14%；完整写入约减少 2%–5%，
但第二轮写入 CV 达 13%，因此只将选择阶段视为较可信的收益。低基数
选择阶段略增，需继续关注；它仍选 Dictionary，高基数仍选 CompactPlain，
两种分布的文件字节不变。优化只在已见不同值的最小字典开销超过当前候选
时终止，255/257/800/900 个不同值加后续重复/null 值均有选择器与文件
round-trip 测试；没有用估计基数替代精确选择。

复现：

```sh
./build-release/sniffer_core_compression_benchmark \
  '--benchmark_filter=^Compression/(high_cardinality_string|low_cardinality_string)_Sniffer/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.03s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### 多查询并发：共享 Reader 的独立 offset 读取

2026-09-25，Apple M4（10 逻辑核）、macOS arm64、Apple Clang 21、Arrow C++ 23.0.1、
Release、warm-cache。确定性 500,000 行双 int64 列：递增 `id`、`value=17*id+3`；
Row Group 8,192，Segment 文件 1,958,476 bytes。每个线程独立执行 `id >= 250000`、投影两列的完整 Scan，
每查询命中 250,000 行；所有线程同时复用同一不可变 Segment。
旧路径使用共享 `ifstream` 加互斥 seek/read；新路径在 macOS/Linux 使用单个
只读 fd 的 `pread`。独立 Reader 对照为每线程各打开一个 Reader；不含 Reader
构造和 Segment 写入时间。每 case 7 次重复、至少 0.05 秒，表中为每查询
wall-time P50（ms）；8 线程另做 11 次独立复测。

| Reader | 线程 | 流式互斥 P50 | `pread` P50 | 复测流式互斥 / `pread` |
|---|---:|---:|---:|---:|
| 共享 | 1 | 2.788 | 2.729 | 2.956 / 2.639 |
| 共享 | 2 | 3.015 | 2.825 | — |
| 共享 | 4 | 3.273 | 2.939 | — |
| 共享 | 8 | 6.228 | 4.036 | 7.075 / 3.986 |
| 独立 | 1 | 2.850 | 2.724 | 2.998 / 2.630 |
| 独立 | 2 | 2.953 | 2.788 | — |
| 独立 | 4 | 3.130 | 2.989 | — |
| 独立 | 8 | 4.372 | 4.001 | 4.519 / 3.962 |

8 线程共享 Reader 的两轮 P50 分别降低约 35% 和 44%；`pread` 共享与
独立 Reader 接近，说明旧共享流锁是该场景的可归因瓶颈。按 8 个线程各扫
500,000 输入行计算，总处理量约为 4,000,000 行 / 4 ms，即约 1.0 G
输入行/s；这是 warm-cache 合成查询的多请求吞吐，不是单请求 Scan 提速，
也不是 Parquet 对照。单线程变化较小，不能据此声称普遍加速。
扫描仍验证 ColumnChunk checksum、按索引剪枝且仅读投影/谓词列。
当前基准尚未覆盖 cold-cache、峰值 RSS、分配量或 Parquet 并发对照。

基线与新路径由同一源码构建，只有后端宏不同：

```sh
cmake -S . -B build-release-stream -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-DSNIFFER_FORCE_STREAM_IO \
  -DSNIFFER_BUILD_TESTS=OFF -DSNIFFER_BUILD_EXAMPLES=OFF
cmake --build build-release-stream -j --target sniffer_core_concurrency_benchmark
./build-release-stream/sniffer_core_concurrency_benchmark \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
./build-release/sniffer_core_concurrency_benchmark \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### 多查询并发：同条件 Parquet 对照

2026-09-27，在上述 Apple M4 / Arrow 23.0.1 / Apple Clang 21 / Release / warm-cache
环境下，使用相同的 500,000 行双 int64 输入、Row Group 8,192、
`id >= 250000` 和两列投影。Parquet 使用仓库已有 Arrow C++ reader/writer，
分别测试无 codec 压缩与 ZSTD（均保留默认 dictionary 设置）。每线程持有独立
Parquet Reader，Reader 构造和写文件不计时；Arrow reader 内部线程关闭。
每次查询用 `id` Row Group min/max 剪枝、仅读两列，再利用本数据全局有序且
非 null 的 `id` 对边界 batch 做零拷贝 slice。Sniffer 独立 Reader 是最直接的
对照，另保留共享 Reader 的服务端常见用法。基准计时外均先热身扫描并逐值验证
输出等于生成数据，计时内校验两边命中行数。两边均未使用内部并行扫描。

每 case 7 次、最短 0.05 秒，表中为 Google Benchmark `real_time` P50（ms）；
1/8 线程又各做 11 次独立复测。

| 格式 / Reader | 文件大小 | 1 线程 | 2 线程 | 4 线程 | 8 线程 | 复测 1 / 8 线程 |
|---|---:|---:|---:|---:|---:|---:|
| Sniffer / 共享 | 1.958 MB | 2.612 | 2.782 | 2.892 | 3.969 | 2.680 / 3.990 |
| Sniffer / 独立 | 1.958 MB | 2.634 | 2.696 | 2.887 | 4.049 | 2.726 / 3.999 |
| Parquet / 无 codec 压缩 | 9.654 MB | 1.104 | 1.268 | 1.531 | 2.797 | 1.094 / 2.830 |
| Parquet / ZSTD | 2.721 MB | 4.519 | 4.699 | 4.908 | 6.568 | 4.520 / 6.528 |

此数据集上，Parquet 无 codec 压缩以约 4.9 倍的文件大小换来最快的查询；
Sniffer 的文件比 Parquet ZSTD 约小 28%，且扫描更快。这个结果只说明该数据
分布和查询的速度/空间取舍，不代表对所有 Parquet 压缩或编码配置占优。
Parquet 基准适配器针对有序 `id` 使用 min/max + slice，而非通用表达式执行；
单次请求的延迟与多请求总吞吐应分开解读。未计入 cold-cache、Reader 构造、
写入、峰值 RSS 或分配量。

复现命令：

```sh
./build-release/sniffer_core_concurrency_benchmark \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
./build-release/sniffer_core_concurrency_benchmark \
  '--benchmark_filter=(ConcurrentScan/(SharedReader|IndependentReaders)|ParquetConcurrentScan/Parquet(_ZSTD)?)/real_time/threads:(1|8)$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### FOR decode 的密集值快路径

2026-09-27，Apple M4 / Apple Clang 21 / Arrow C++ 23.0.1 / Release / warm-cache。
上述 500,000 行、两列有序 int64、Row Group 8,192、50% 命中查询的
`ScanMetrics` 显示：改动前单线程总计时 P50 2.598 ms，其中 FOR 等解码
1.545 ms、谓词 0.744 ms、跨组拼接 0.101 ms（31 次）、chunk I/O
0.061 ms、CRC 0.091 ms。故跨组拼接虽频繁，却不是此数据集的主要耗时。

针对 FOR decoder，在预留完整输出容量后改用 Arrow builder 的 `UnsafeAppend`
/`UnsafeAppendNull`；无 null payload 单独走密集循环，避免每行检查 validity。
delta 域检查、payload 边界校验与最终 `ValidateFull()` 保持不变，不修改文件字节。
改动前 7 次、改动后 11 次，均为至少 0.05 秒的独立重复，以下为 P50：

| 路径 | 改动前 | 改动后 |
|---|---:|---:|
| 共享 Reader，1 线程查询 | 2.598 ms | 1.955 ms |
| 共享 Reader，8 线程查询 | 3.946 ms | 3.288 ms |
| 单线程解码阶段 | 1.545 ms | 0.932 ms |
| 单线程谓词阶段 | 0.744 ms | 0.694 ms |
| 单线程跨组拼接阶段 | 0.101 ms | 0.103 ms |

单线程端到端约降 25%，8 线程约降 17%；解码阶段约降 40%。这只是
有序双 int64/FOR 占主导的合成查询，不意味着其他 encoding 均提速。
FOR codec 的独立 100,000 行、128 值范围 decode microbenchmark 在改动后
11 次 P50 为 209.15 µs、100,040 encoded bytes；此处不拿历史不同版本的
microbenchmark 当作同条件 before 值。

复现：

```sh
./build-release/sniffer_core_concurrency_benchmark \
  '--benchmark_filter=^ConcurrentScan/(SharedReader|IndependentReaders)/real_time/threads:(1|8)$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
./build-release/sniffer_core_codec_benchmark \
  '--benchmark_filter=^Codec/for_bitpack_128_range_Decode$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### Statistics 证明整组命中，跳过逐行谓词

2026-09-27，同一 Apple M4 / Arrow C++ 23.0.1 / Apple Clang 21 / Release / warm-cache，
500,000 行双 int64，Row Group 8,192、`id >= 250000`、50% 命中、投影两列。
改动前为提交 `6d9e469`；改动后仅增加保守的 Row Group 全命中证明：
所有谓词均能由统计信息证明为真且没有 sort-key range 时，不再逐行检查。
缺失统计、比较列含 null/NaN 或不能证明时按原路径执行（`IS NULL` 与
`IS NOT NULL` 可直接利用 `null_count`）；谓词列读取/CRC、
解码、列投影和输出 batch 语义保持不变。文件仍为 1,958,476 bytes，
每查询跨组拼接 31 次。两版各运行 11 次、最短 0.05 秒，P50：

| 指标 | 改动前 | 改动后 |
|---|---:|---:|
| 共享 Reader，1 线程完整查询 | 1.942 ms | 1.273 ms |
| 共享 Reader，8 线程完整查询 | 3.294 ms | 2.203 ms |
| 独立 Reader，1 线程完整查询 | 1.961 ms | 1.281 ms |
| 独立 Reader，8 线程完整查询 | 3.260 ms | 2.168 ms |
| 单线程谓词阶段 | 0.696 ms | 0.022 ms |
| 单线程解码阶段 | 0.931 ms | 0.936 ms |

端到端约降 34%（1 线程）和 33%（8 线程）；解码耗时基本未动，
证明收益来自跳过已被索引证明的逐行判断。此结果依赖统计信息和查询分布，
不能推广到索引缺失或每个 Row Group 都混合命中的场景。测试另验证无统计
顺序回退、`limit`、全 null、含 NaN 和投影列与过滤列分离。

复现：

```sh
./build-release/sniffer_core_concurrency_benchmark \
  '--benchmark_filter=^ConcurrentScan/(SharedReader|IndependentReaders)/real_time/threads:(1|8)$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### 已证明整组命中时跳过未投影的谓词块

2026-09-27，同一 Apple M4 / Apple Clang 21 / Arrow C++ 23.0.1 / Release /
warm-cache，500,000 行有序双 int64、Row Group 8,192、`id >= 250000`，
仅投影 `value`。在上述整组命中证明基础上，未被投影且不需逐行过滤的
`id` ColumnChunk 不再读取、校验或解码；跨越阈值的 Row Group 仍按原路径
读取并过滤。投影谓词列时仍须读该列，并按投影解码路径计数。

变更前后各 9 次、每次至少 0.05 秒，以下为 P50。独立 Reader 的 1/8
线程也由 1.210/2.04 ms 降至 0.677/1.19 ms。

| 指标（共享 Reader） | 改动前 | 改动后 |
|---|---:|---:|
| 1 线程端到端 | 1.21 ms | 0.681 ms |
| 8 线程端到端 | 2.04 ms | 1.20 ms |
| 1 线程 decode 阶段 | 0.933 ms | 0.478 ms |
| 每查询读取 ColumnChunk | 64 | 33 |
| 每查询读取 chunk 字节 | 987,416 | 586,492 |
| 每查询谓词 ColumnChunk 解码 | 32 | 1 |

1 线程约降 44%，8 线程约降 41%；文件仍为 1,958,476 bytes。这里的
Parquet benchmark 固定双列投影，故此 value-only Sniffer 查询不能直接
拿来与其比较。无统计信息时继续逐行回退；单测还通过损坏一个全命中
Row Group 的未投影 `id` 块，验证该查询不读取它，而 `ReadAll()` 读到
该块仍报告 checksum 错误。收益依赖可证明整组命中、谓词列未投影的查询。

复现：

```sh
./build-release/sniffer_core_concurrency_benchmark \
  '--benchmark_filter=ConcurrentScan/(SharedReaderValueOnly|IndependentReadersValueOnly)/real_time/threads:(1|8)$' \
  --benchmark_repetitions=9 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

### Dense FOR 解码直接填充 Arrow 值缓冲区

2026-09-28，Apple M4 / Apple Clang 21 / Arrow C++ 23.0.1 / Release /
warm-cache。上一节的 value-only 查询解码阶段占约 0.478 ms。针对 FOR
无 null 且无 selection 的全量解码，预先分配 Arrow-owned 值缓冲区并逐值
填充；nullable 和 selected decode 仍使用 builder。所有 payload 边界、
delta 域检查与最终 `ValidateFull()` 保留，文件字节与查询读取量不变。

先尝试的 8 字节窗口 bit-unpack 在 7-bit nullable 和 23-bit dense 的
100,000 行 microbenchmark 都慢了约 3%，已撤回。以下是保留的缓冲区
快路径；改前 microbenchmark 11 次、完整扫描 9 次，改后均 11 次，
每轮至少 0.05 秒，取 P50：

| 指标 | 改动前 | 改动后 |
|---|---:|---:|
| FOR dense 23-bit，无 null，100,000 行解码 | 196 µs | 160 µs |
| FOR 7-bit，含 null，100,000 行解码 | 210 µs | 209 µs |
| 共享 Reader、value-only、1 线程完整查询 | 0.681 ms | 0.590 ms |
| 共享 Reader、value-only、8 线程完整查询 | 1.20 ms | 1.06 ms |
| 1 线程完整查询的解码阶段 | 0.478 ms | 0.387 ms |

完整查询为 500,000 行有序双 int64、Row Group 8,192、`id >= 250000`
（50% 命中）、仅投影 `value`；仍读取 33 个 ColumnChunk、586,492
chunk 字节，文件仍为 1,958,476 字节。单线程端到端约降 13%；结果
不能推广到 nullable、selected decode 或其他编码。全类型极值、所有
0–64 bit width、空列、selected fallback 和越界 delta 均有测试。

复现：

```sh
./build-release/sniffer_core_codec_benchmark \
  '--benchmark_filter=Codec/for_bitpack_(128_range|dense_23bit)_Decode$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
./build-release/sniffer_core_concurrency_benchmark \
  '--benchmark_filter=ConcurrentScan/SharedReaderValueOnly/real_time/threads:(1|8)$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=csv
```

## 2026-09-28：同机旧版回归、完整 warm-cache 矩阵与独立进程内存

以下复测均为 Apple M4 / macOS arm64 / AppleClang 21 / Arrow C++ 23.0.1 /
Release / 系统临时目录 / warm-cache。旧版取 v0.2 规划之前的提交
`aeb6a17857927f79227ad552a53287bd3cc6c376`，当前取 `de2741a`。
旧版在隔离目录构建；新旧都在同机运行。旧版使用自建 runner 的 11 次
median，当前使用 Google Benchmark 的 11 次 P50、每次至少 0.05 秒；
两代 runner 的分组、计时框架和迭代策略不同，因此以下倍数是工作负载级
回归参考，不是严格的同一 runner kernel A/B。

固定性能场景的输入、`id >= 50000`、`id,value` 投影、输出 batch
上限和 Row Group 8,192 一致。旧版与当前文件均为 675,943 字节，
都剪枝 6/13 个 Row Group、读取 14 个 ColumnChunk / 184,036 字节，
输出 50,000 行。

| 源码 | 写入 P50 ms | 扫描 P50 ms | 端到端 P50 ms |
| --- | ---: | ---: | ---: |
| v0.2 前 `aeb6a17` | 65.732 | 17.007 | 82.696 |
| 当前 `de2741a` | 3.584 | 0.438 | 4.042 |

当前重复测量的写入/扫描/端到端 CV 为 1.82%/3.08%/1.86%。
旧版 runner 只保留中位数、未报告 CV，不应将表中差异全部归因于某一个
优化。新版本文件布局及该查询读取量与旧版完全一致，支持“没有通过多读
数据换取速度”的判断。

六种 v1 压缩分布也用 100,000 行、Row Group 4,096 重新运行：旧版
7 次 median，当前 Google Benchmark 7 次 P50、每次至少 0.05 秒。
下表为 Sniffer 的文件字节和编码写入/解码读取耗时；输入构造与结果验证
均不计时，两边数据生成函数相同。

| 分布 | 旧/当前文件字节 | 旧写/读 ms | 当前写/读 ms |
| --- | ---: | ---: | ---: |
| 递增 int64 | 153,882 / 153,882 | 11.293 / 11.780 | 1.347 / 0.197 |
| 窄值域 int64 | 91,594 / 91,594 | 9.232 / 11.757 | 0.845 / 0.177 |
| 长 RLE int64 | 7,166 / 7,166 | 17.324 / 1.640 | 0.450 / 0.248 |
| nullable 偏斜 int64 | 54,094 / 54,094 | 7.328 / 9.578 | 0.623 / 0.268 |
| 低基数字符串 | 117,044 / 117,044 | 17.037 / 3.135 | 1.295 / 1.330 |
| 高基数字符串 | 3,203,894 / 2,803,815 | 30.990 / 13.686 | 3.962 / 2.176 |

六场景逻辑字节合计 7,181,258。以各场景 P50 耗时相加估算，旧版
编码/解码 73.48/132.79 MiB/s，当前约 803.69/1,558.17 MiB/s；
这不是一次合并计时。前五个场景压缩比不变；高基数字符串通过独立
CompactPlain 编码将文件缩小约 12.5%，压缩比 0.874x → 0.999x。
旧 Reader 不要求读取新 ID 4，新 Reader 的实际旧版 Segment 兼容性
另由 `V01ArtifactRemainsReadable` 固定样本测试覆盖。

完整 warm-cache 矩阵覆盖 Sniffer、Parquet 无压缩、Parquet ZSTD 各
4 个 Row Group 尺寸 × 4 个选择率 × 3 个投影宽度，共 144 个 case，
每个 7 次重复、至少 0.01 秒。逐 case 的写入、扫描、端到端 P50、
scan CV、文件大小与剪枝/读块指标保存在
[`V0.2_MATRIX_2026-09-28.tsv`](V0.2_MATRIX_2026-09-28.tsv)。
例如 Row Group 8,192、投影 `id,value` 时，scan P50（ms）：

| 选择率 | Sniffer | Parquet | Parquet ZSTD | Sniffer 块字节 |
| --- | ---: | ---: | ---: | ---: |
| 1% | 0.190 | 0.387 | 0.206 | 5,380 |
| 10% | 0.262 | 0.568 | 0.602 | 64,932 |
| 50% | 0.435 | 0.949 | 1.372 | 184,036 |
| 100% | 0.636 | 1.523 | 2.541 | 362,692 |

矩阵在一个进程中连续运行，其 RSS/Arrow pool high-water 不可按 case
比较。7/144 个 case 的 scan CV 超过 5%；其中 Sniffer 的
RG 1,024 / 1% / 2 列与 Parquet 的 RG 1,024 / 1% / 3 列受明显
异常值影响，另用独立进程 11 次、至少 0.05 秒复测，scan P50/CV
分别为 0.296 ms / 1.94% 与 0.590 ms / 5.06%。原始 TSV 保留
首轮结果，不以不稳定 case 声称格式优劣。

代表性内存 case 分别启动全新 benchmark 进程，7 次重复、每次至少
0.05 秒。`process_peak_rss_bytes` 是整个进程生命周期高水位，含输入
Arrow batch、Writer、Reader 和库初始化；`arrow_pool_peak_bytes`
也是进程级峰值，`arrow_total_allocated_bytes` 是每轮累计申请量，
三者不能混为 Reader 峰值或单次常驻内存。

| 独立进程 case | 进程 RSS 峰值 B | Arrow pool 峰值 B | Arrow 每轮申请 B |
| --- | ---: | ---: | ---: |
| 固定 3 列 / Sniffer | 12,959,744 | 3,086,272 | 1,211,712 |
| 固定 3 列 / Parquet | 16,728,064 | 3,907,840 | 38,647,892 |
| 固定 3 列 / Parquet ZSTD | 16,334,848 | 3,751,424 | 38,125,972 |
| 16 列窄投影 / Sniffer | 22,052,864 | 13,346,688 | 1,211,712 |

Sniffer 的两个极端矩阵 case 另测：RG 1,024 / 1% / 1 列的
RSS/pool 峰值为 12,140,544/3,086,272 B、仅读 2,200 B；
RG 262,144 / 100% / 3 列为 14,647,296/5,562,752 B、
读取 563,138 B。它们展示投影、选择率和当前 Row Group 对资源的影响，
但由于同进程写入和输入 batch 常驻，尚不能证明 Reader 峰值不随
整个文件大小增长。

新增独立 Reader-only Google Benchmark：生成器逐个 8,192 行 Row Group
构造两列递增 int64 文件；测量进程只执行 Open 与 `key >= rows/2`、
仅投影 `value` 的流式扫描，输出 batch 上限 4,096 行，不持有输入
batch 或 Writer。不同规模各起一个扫描进程，7 次重复、每次至少
0.05 秒，取 P50（扫描计时不含 Open）：

| 输入行数 | 文件字节 | Row Group | 扫描 P50 ms | 进程 RSS 峰值 B | Arrow pool 峰值 B |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 328,793 | 13 | 0.140 | 5,324,800 | 163,840 |
| 10,000,000 | 32,915,361 | 1,221 | 9.298 | 6,537,216 | 163,840 |

文件扩大约 100 倍，扫描进程 RSS 高水位增加约 1.21 MiB，Arrow pool
峰值不变；大文件 RSS 远低于 32.9 MB 文件本身，证明此数据路径没有
整文件驻留。RSS 包括库启动和目录，索引/目录内存仍随 Row Group 数
增长；本实验不能证明严格 O(1) 元数据内存，也不是 cold-cache 结果。

复现 Reader-only 基准（每个 `--segment` 命令都应在新进程中执行）：

```sh
sniffer_mem_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_mem_dir/100k.seg" --rows=100000
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_mem_dir/10m.seg" --rows=10000000
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_mem_dir/100k.seg" --rows=100000 \
  '--benchmark_filter=^ReaderOnlyScan/real_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_mem_dir/10m.seg" --rows=10000000 \
  '--benchmark_filter=^ReaderOnlyScan/real_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

复现命令（矩阵 TSV 从 Google Benchmark JSON 的 `median` 与 `cv`
行抽取；内存必须每个 `--benchmark_filter` 单独启动进程）：

```sh
sniffer_legacy_dir=$(mktemp -d)
git archive aeb6a17 | tar -x -C "$sniffer_legacy_dir"
cmake -S "$sniffer_legacy_dir" -B "$sniffer_legacy_dir/build" \
  -DCMAKE_BUILD_TYPE=Release -DSNIFFER_BUILD_TESTS=OFF -DSNIFFER_BUILD_BENCHMARK=ON
cmake --build "$sniffer_legacy_dir/build" -j 8
"$sniffer_legacy_dir/build/sniffer_core_performance_benchmark" \
  --rows=100000 --iterations=11 --row-group=8192
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceMatrix/Sniffer/rows:100000/row_group_rows:8192/selectivity_percent:50/projection_columns:2/manual_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^PerformanceMatrix/(Sniffer|Parquet|Parquet_ZSTD)/' \
  --benchmark_repetitions=7 --benchmark_min_time=0.01s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

旧版临时目录不是仓库依赖。所有数据仍为 warm-cache 合成场景。
Parquet 与 Sniffer 的 checksum、压缩和真实
物理 I/O 口径不同，不应将这里的扫描时间外推为生产格式优劣。

## 2026-09-28：SortKey 整组命中证明与单请求分片可行性

在上述 Reader-only 基准上补充 `ScanMetrics` 分阶段指标。Apple M4 / Release /
Arrow 23.0.1 / warm-cache，10,000,000 行、Row Group 8,192、谓词
`key >= rows/2`、仅投影 `value`，7 次 P50 扫描约 9.41 ms；其中
`decode_ns` 约 6.62 ms（约 70%），checksum 0.75 ms、chunk I/O
0.60 ms、batch materialization 0.66 ms。阶段计时是嵌套/独立
累计指标，不能简单相加推导总时间；它表明此大请求值得评估解码并行。

为检验上界，新增实验性的 `ShardedSortRangeScan/{1,2,4,8}`：一个逻辑
`[rows/2, rows)` 查询按排序键分为不重叠范围，每个 worker 通过同一
Reader 创建独立 Scan，线程数固定上限 8。每个分片在计时前逐值校验
行序与结果；线程创建、`Scan`、读取及合并行数均在计时区间内。各 case
在独立进程运行 7 次，每次至少 0.05 秒，取 real-time P50。它只是一种
公开 API 组合的可行性实验，不是 Reader 内部并行；不支持通用非排序
谓词、`limit` 的跨分片早停，也没有生产级在途 Row Group/内存预算或取消。

初版范围路径即使索引证明整个 Row Group 命中，仍读取并解码排序键列。
Reader 现增加“首尾排序键均在范围内”的整组证明：这种组不读取未投影
的排序键 ColumnChunk，边界组仍逐行过滤。测试覆盖半开/开区间、limit、
复合排序键、损坏但未读取的 key 块，结果与原执行一致。以下 before/after
只改变这个生产扫描热点；Segment 文件、查询、分片基准和计时参数不变。

| 10M 行分片 worker | 修改前 P50 ms | 修改后 P50 ms | 修改前/后读取 MB（约） | 修改后进程 RSS 峰值 MB（约） |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 44.196 | 9.518 | 16.308 / 8.167 | 6.83 |
| 2 | 23.213 | 5.339 | 16.335 / 8.208 | 7.98 |
| 4 | 12.893 | 3.310 | 16.388 / 8.288 | 12.75 |
| 8 | 11.210 | 2.998 | 16.495 / 8.448 | 20.89 |

修改后 1/2/4/8 worker 的 real-time CV 分别约 0.63%/0.79%/0.94%/1.71%。
RSS 为进程生命周期高水位，含计时前逐值校验和线程栈，不等同于单次扫描
的净峰值。相对修改后同机的普通单线程谓词扫描（约 9.4–9.5 ms、
8,167,472 B），8 worker 分片约 3.0 ms，但增加了边界读取及内存成本。
100,000 行时，普通单线程谓词扫描约 0.140 ms，修改后的 1/2/4/8
worker 分片 P50 为 0.182/0.208/0.206/0.323 ms；短查询不应自动开线程。

复现时需为每个 worker 数单独启动 benchmark 进程；`--generate`
生成相同两列递增数据，见上一节。示例：

```sh
for sniffer_workers in 1 2 4 8; do
  ./build-release/sniffer_core_reader_memory_benchmark \
    "--segment=$sniffer_mem_dir/10m.seg" --rows=10000000 \
    "--benchmark_filter=^ShardedSortRangeScan/${sniffer_workers}/real_time$" \
    --benchmark_repetitions=7 --benchmark_min_time=0.05s \
    --benchmark_report_aggregates_only=true --benchmark_format=csv
done
```

该实验不能替代真正的有界 Row Group 调度 A/B；单线程默认路径、非排序
谓词、Parquet 同条件单请求对照、cold-cache 与资源预算仍是 v0.2 待办。

## 2026-09-28：单 Row Group 扫描单元重构基线

为后续按物理 Row Group 调度，从串行遍历中提取 `ReadRowGroupAt(index, limit)`；
当前公开 `Scan` 仍单线程，数据格式、计划、剪枝与错误顺序不变。Reader-only
同机 Release / Apple M4 / Arrow 23.0.1 / warm-cache、两列递增 int64、
Row Group 8,192、`key >= rows/2`、仅投影 `value`、输出 batch 4,096 的
独立进程复测：

| 输入 | 重构前 P50 ms | 重构后 P50 ms | 列块读取量前/后 | 备注 |
| ---: | ---: | ---: | ---: | --- |
| 100,000 行 | 0.142（11 次） | 0.146（11 次） | 8 块 / 95,836 B | 本次约 3% 回退，不宣称优化 |
| 10,000,000 行 | 9.557（7 次） | 9.536（11 次） | 612 块 / 8,167,472 B | 前次 CV 15%，不作微小差异判断 |

两次均使用 `--benchmark_min_time=0.05s`；100K 的前/后 scan CV 为
0.85%/0.78%，10M 为 15.0%/1.67%。这是调度器前的结构准备，不是并行
速度结论。若正式并行路径不能带来足够收益，需要重新权衡这一步的短请求成本。

## 2026-09-29：单请求有界 Row Group 并行首版

Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
warm-cache，使用上述已生成的两列递增 int64 Segment；Row Group 8,192，
`key >= rows/2`，仅投影 `value`，输出 batch 4,096。`ReaderOnlyScan` 走原串行
API；`BoundedParallelReaderScan/{2,4,8}` 是同一逻辑计划的单请求内部调度，
最多 worker 数个在途 Row Group，64 MiB 估算预算。每个 case 独立进程，
打开 Reader 不计时；创建 Scan、启动线程、读取、解码、拼接均计入 real time。
本合成数据没有 null 或变长列，不能替代前述正确性矩阵。

10M 行，7 次独立进程 case 的 real-time P50 与资源指标：

| worker | P50 ms | CV | ColumnChunk / bytes | 进程 RSS 峰值 MB | Arrow pool 峰值 MB | 在途组 / 估算预留峰值 B |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 9.707 | 1.39% | 612 / 8,167,472 | 6.57 | 0.164 | 0 / 0 |
| 2 | 8.346 | 6.02% | 612 / 8,167,472 | 8.40 | 0.295 | 2 / 794,816 |
| 4 | 6.640 | 2.43% | 612 / 8,167,472 | 9.68 | 0.426 | 4 / 1,589,632 |
| 8 | 7.891 | 1.50% | 612 / 8,167,472 | 13.03 | 0.688 | 8 / 3,179,264 |

进一步在无其他构建/测试同时运行时，对串行和 4 worker 各取 20 次单次
benchmark 样本，`--benchmark_min_time=0.05s`；P95 为排序后第 19 个样本：

| 输入行数 | 串行 P50/P95 ms | 4 worker P50/P95 ms | 结论 |
| ---: | ---: | ---: | --- |
| 100,000 | 0.144/0.145 | 0.152/0.154 | 并行约慢 5%，短请求不应默认开启 |
| 10,000,000 | 9.425/9.847 | 6.670/6.956 | 该数据上约 1.41× P50 提升，但 RSS 增加 |

8 worker 比 4 worker 慢，说明解码并行之外已有调度、内存和线程开销；
不能把 8-worker 应用层排序范围分片的约 3 ms 结果当作同等内部调度收益。
这些指标是 warm-cache 合成输入；同计划 Parquet 单请求见下节，cold-cache 和
宽/变长列的 P50/P95/RSS 尚未补齐，不以此宣称通用格式优势。

复现示例（将路径替换为由本 benchmark 的 `--generate=... --rows=...`
生成、未覆盖的 Segment；每个 case 单独启动进程以测 RSS）：

```sh
./build-release/sniffer_core_reader_memory_benchmark \
  --segment=/path/to/rows10m.seg --rows=10000000 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-29：同计划 Parquet Reader-only 单请求对照

Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`，
系统临时目录、warm-cache。三个文件均由同一生成器逐组写入两列递增 int64，
Row Group 为 8,192 行；Parquet 分别为未压缩和 ZSTD，Arrow reader 内部线程关闭。
查询均为 `key >= rows/2`、仅投影 `value`、输出 batch 上限 4,096 行，
扫描前逐值核对期望行序和值。Parquet 借助 key min/max 剪枝；跨越边界的
一组读取 key 与 value 并按有序 key 截取，整组命中只读取 value。
Sniffer 保留原生 SortKey/统计剪枝和串行或显式 4-worker Scan。
每个 case 独立进程、20 次重复，`--benchmark_min_time=0.05s`；打开 Reader、
预先逐值校验和生成文件不计入 real time，创建 Scan/Parquet batch reader、
读取、解码、过滤和拼 batch 计入。P95 为排序后第 19 个样本。

| 行数 | 执行路径 | 文件 B | P50 / P95 ms | 候选列块 / 字节 | RSS / Arrow pool 峰值 MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100,000 | Sniffer 串行 | 328,793 | 0.145 / 0.146 | 8 / 95,836（实际列块读取） | 6.50 / 0.164 |
| 100,000 | Sniffer 4-worker | 328,793 | 0.149 / 0.154 | 8 / 95,836（实际列块读取） | 7.49 / 0.419 |
| 100,000 | Parquet 未压缩 | 1,930,850 | 0.202 / 0.204 | 8 / 568,307（元数据候选量） | 8.63 / 0.493 |
| 100,000 | Parquet ZSTD | 530,998 | 0.573 / 0.581 | 8 / 154,770（元数据候选量） | 8.55 / 0.371 |
| 10,000,000 | Sniffer 串行 | 32,915,361 | 9.506 / 9.636 | 612 / ≈8,167,470（实际列块读取） | 7.70 / 0.164 |
| 10,000,000 | Sniffer 4-worker | 32,915,361 | 6.786 / 7.171 | 612 / ≈8,167,470（实际列块读取） | 11.50 / 0.426 |
| 10,000,000 | Parquet 未压缩 | 193,084,376 | 18.911 / 19.339 | 612 / ≈48,268,900（元数据候选量） | 85.26 / 48.21 |
| 10,000,000 | Parquet ZSTD | 52,963,148 | 50.952 / 51.810 | 612 / ≈13,162,600（元数据候选量） | 26.62 / 13.31 |

两种格式均剪枝 100K 的 6/13 组、10M 的 610/1,221 组。Sniffer 字节来自
`ScanMetrics.column_chunk_bytes_read`；Parquet 字节是被选列的
`total_compressed_size` 元数据之和，**不是物理 I/O，也不能与 Sniffer
实际读取字节直接比较**。RSS 是进程高水位，包含库初始化、Reader 打开和
计时前逐值校验，不是每次迭代的增量；Arrow pool 不含其他分配。
这个合成有序、无 null、窄投影场景下，Sniffer 更小且查询更快；不能外推至
宽表、变长列、其他选择率或 cold-cache，也不能据此判定通用格式优劣。

复现时先分别生成 100K 或 10M 的三份文件，生成与测量分进程；每个
`--benchmark_filter` 也单独启动进程以保留 RSS 口径。例如：

```sh
sniffer_reader_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_reader_dir/data.seg" --rows=10000000
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet=$sniffer_reader_dir/data.parquet" --rows=10000000
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet-zstd=$sniffer_reader_dir/data.zstd.parquet" --rows=10000000
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_reader_dir/data.seg" \
  "--parquet=$sniffer_reader_dir/data.parquet" \
  "--parquet-zstd=$sniffer_reader_dir/data.zstd.parquet" --rows=10000000 \
  '--benchmark_filter=^ParquetReaderOnlyScan/ZSTD/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

将 filter 分别换成 `ReaderOnlyScan`、`BoundedParallelReaderScan/4`、
`ParquetReaderOnlyScan/Uncompressed` 可测另外三条路径。

## 2026-09-29：无关变长列不再占用并行调度预算

旧预算将 Row Group 的**所有** ColumnChunk 解码长度翻倍计入，即使计划只
触及 key/value；存在大而未投影的 binary 列时，1 MiB 预算可能让扫描整体
退回串行。现在预算只估算谓词、排序键和投影列的并集，仍为每个相关列预留
两份未压缩长度，加每行 16 B 与每组 4,096 B；这仍只是调度估算，
不是 RSS 硬上限。测试固定无关宽列下 worker 确实启动、输出与串行逐 batch
一致，同时在投影宽列时正确退回串行。

Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
系统临时目录、warm-cache，Row Group 8,192，`key >= rows/2`，只投影
int64 value。额外第三列是每行 256 B 的高基数 binary，不参与查询；
Sniffer 仍只读 100K 的 8 个或 1M 的 63 个候选列块。
每个 case 独立进程，11 次重复、每次至少 0.05 秒，报告 real-time P50：

| 输入 | 文件 B | 串行 P50 ms | 4-worker P50 ms | 并行 worker / 在途峰值 / 预算预留峰值 | RSS 峰值，串行 / 并行 MB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100K | 26,729,967 | 0.145 | 0.194 | 4 / 3 / 880,416 B | 6.44 / 7.41 |
| 1M | 267,302,463 | 0.985 | 1.135 | 4 / 3 / 826,656 B | 6.72 / 7.85 |

两组输入上并行虽不再错误退回串行，却仍分别慢约 34% 和 15%；默认继续
串行，不能将“worker 已启动”称为吞吐提升。这里是窄投影加一个无关变长列，
不是宽投影或变长列解码性能验收；完整矩阵和 cold-cache 仍未完成。
原 10M 双 int64 Segment 另做 7 次短回归，串行/4-worker P50 为
9.483/6.654 ms，候选列块仍为 612 个；与上一节的波动范围一致。
复现示例：

```sh
sniffer_wide_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_wide_dir/wide.seg" --rows=1000000 \
  --unprojected-binary-bytes=256
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_wide_dir/wide.seg" --rows=1000000 \
  --buffer-budget-bytes=1048576 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=11 --benchmark_min_time=0.05s --benchmark_format=csv
```

串行对照将 filter 换为 `^ReaderOnlyScan/real_time$`，同样新进程运行。

## 2026-09-29：nullable 高熵 binary 投影的同计划单请求对照

进一步把第二列从 int64 改为每个非 null 值 128 B 的确定性高熵 binary，
每 17 行一个 null；key 仍为递增 int64。Sniffer、Parquet 未压缩和 Parquet
ZSTD 由同一生成函数逐个 8,192 行 Row Group 写出。三者查询均为
`key >= rows/2`、仅投影 binary value、输出 batch 上限 4,096；计时前
分别按实际执行路径逐值验证全部命中行（包括 null、字节、顺序），
Parquet 按 key min/max 剪枝，边界组取 key+value，整组命中只取 value。
Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
系统临时目录、warm-cache；每个 case 独立进程，20 次重复、每次至少 0.05 秒，
P95 为排序后第 19 个样本。RSS/Arrow pool 另以相同参数 7 次独立进程
case 的 median counter 报告，均包含计时前的 Reader 和逐值校验高水位。

| 行数 | 执行路径 | 文件 B | P50 / P95 ms | 候选列块 / 字节 | RSS / Arrow pool 峰值 MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100,000 | Sniffer 串行 | 13,026,089 | 1.481 / 1.556 | 8 / ≈6.552 MB（实际列块读取） | 12.34 / 2.55 |
| 100,000 | Sniffer 4-worker | 13,026,089 | 0.718 / 0.740 | 8 / ≈6.552 MB（实际列块读取） | 36.29 / 5.74 |
| 100,000 | Parquet 未压缩 | 13,574,825 | 1.187 / 1.202 | 8 / ≈6.486 MB（元数据候选量） | 17.60 / 6.85 |
| 100,000 | Parquet ZSTD | 12,808,903 | 1.392 / 1.433 | 8 / ≈6.416 MB（元数据候选量） | 19.19 / 8.08 |
| 1,000,000 | Sniffer 串行 | 130,261,369 | 16.476 / 16.729 | 63 / ≈64.350 MB（实际列块读取） | 11.93 / 2.55 |
| 1,000,000 | Sniffer 4-worker | 130,261,369 | 6.428 / 6.687 | 63 / ≈64.350 MB（实际列块读取） | 41.09 / 5.80 |
| 1,000,000 | Parquet 未压缩 | 135,745,030 | 13.926 / 14.179 | 63 / ≈63.119 MB（元数据候选量） | 79.51 / 63.71 |
| 1,000,000 | Parquet ZSTD | 128,385,842 | 15.583 / 15.796 | 63 / ≈62.891 MB（元数据候选量） | 79.43 / 64.55 |

100K 剪枝 6/13 组，1M 剪枝 61/123 组；4-worker 的在途峰值为 4 组、
估算预留峰值约 9.49 MB，默认预算 64 MiB。高熵值令 ZSTD 的空间收益
较小：Parquet ZSTD 文件比 Sniffer 略小，而 Sniffer 4-worker 在此场景
延迟更低、RSS 高于串行。Sniffer 串行延迟并未胜过 Parquet 未压缩；
不能忽略这个取舍。Parquet 候选字节是元数据的 `total_compressed_size`
之和，**不是物理 I/O**；Parquet 当前适配器的 batch reader 与 Sniffer
流式 Reader 的分配策略不同，RSS 差异不可单独归因于文件格式。
这不是所有 binary 分布、宽投影或 cold-cache 的通用结论。

复现 1M 行的生成与一个测量 case；其他格式用相应生成/输入参数，
每个 case 单独启动进程：

```sh
sniffer_binary_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_binary_dir/data.seg" --rows=1000000 \
  --projected-binary-bytes=128
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet=$sniffer_binary_dir/data.parquet" --rows=1000000 \
  --projected-binary-bytes=128
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet-zstd=$sniffer_binary_dir/data.zstd.parquet" --rows=1000000 \
  --projected-binary-bytes=128
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_binary_dir/data.seg" \
  "--parquet=$sniffer_binary_dir/data.parquet" \
  "--parquet-zstd=$sniffer_binary_dir/data.zstd.parquet" \
  --rows=1000000 --projected-binary-bytes=128 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-29：15 列宽投影的同计划 Reader-only 对照

为避免将“文件含宽列但查询不读取”误作宽投影，生成相同的 16 列文件：
第一列 key 为递增 int64，后 15 列均为非 null int64；value 为行号，
第 `p` 个额外投影列的值为 `row * (p + 1) + p`。同一查询
`key >= rows/2` 投影全部 15 个 value 列，输出 batch 上限 4,096。
这组相关递增整数刻意构成可压缩宽表，不代表随机或变长宽表。
Sniffer 与 Parquet 未压缩/ZSTD 使用同一生成函数和 8,192 行 Row Group，
计时前分别逐列逐值验证实际执行路径，含边界组的排序过滤。
Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
系统临时目录、warm-cache。每 case 独立进程，20 次重复、每次至少
0.05 秒；P95 为排序后第 19 个样本。RSS/Arrow pool 为另外 7 次重复的
median counter，含 Reader 打开和计时前校验高水位，不是每轮增量。

| 行数 | 执行路径 | 文件 B | P50 / P95 ms | 候选列块 / 字节 | RSS / Arrow pool 峰值 MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100,000 | Sniffer 串行 | 3,177,944 | 1.543 / 1.579 | 106 / ≈1.536 MB（实际列块读取） | 11.68 / 2.46 |
| 100,000 | Sniffer 4-worker | 3,177,944 | 0.706 / 0.724 | 106 / ≈1.536 MB（实际列块读取） | 17.50 / 5.57 |
| 100,000 | Parquet 未压缩 | 15,443,727 | 1.854 / 1.879 | 106 / ≈7.420 MB（元数据候选量） | 16.92 / 7.62 |
| 100,000 | Parquet ZSTD | 4,261,187 | 7.058 / 7.133 | 106 / ≈2.033 MB（元数据候选量） | 13.78 / 4.15 |
| 1,000,000 | Sniffer 串行 | 31,828,504 | 14.815 / 15.151 | 931 / ≈15.055 MB（实际列块读取） | 12.57 / 2.46 |
| 1,000,000 | Sniffer 4-worker | 31,828,504 | 4.689 / 5.041 | 931 / ≈15.055 MB（实际列块读取） | 20.20 / 5.54 |
| 1,000,000 | Parquet 未压缩 | 154,457,173 | 20.574 / 21.018 | 931 / ≈72.361 MB（元数据候选量） | 106.66 / 72.58 |
| 1,000,000 | Parquet ZSTD | 42,599,665 | 69.397 / 70.134 | 931 / ≈19.850 MB（元数据候选量） | 32.90 / 21.96 |

100K 剪枝 6/13 组，1M 剪枝 61/123 组。Sniffer 4-worker 的在途峰值
4 组，估算预留峰值约 8.93 MB（配置预算 64 MiB），其 RSS 高于串行。
Parquet 候选字节来自列元数据 `total_compressed_size`，**不是物理 I/O**，
不能与 Sniffer 实际列块读取字节直接比较。Parquet batch reader 与
Sniffer 流式扫描的分配策略不同，内存差异不能归因于文件格式本身。
当前相关整数值使 Sniffer 的整数编码非常有效；不可外推到高熵宽列、
宽变长列或 cold-cache。尤其 Parquet ZSTD 的空间/解码取舍应按实际
生产数据分布重新测量。

复现 1M 行的生成和一个测量 case，其他 case 仅替换 filter，均新起进程：

```sh
sniffer_wide_projection_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_wide_projection_dir/data.seg" --rows=1000000 \
  --projected-columns=15
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet=$sniffer_wide_projection_dir/data.parquet" --rows=1000000 \
  --projected-columns=15
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet-zstd=$sniffer_wide_projection_dir/data.zstd.parquet" --rows=1000000 \
  --projected-columns=15
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_wide_projection_dir/data.seg" \
  "--parquet=$sniffer_wide_projection_dir/data.parquet" \
  "--parquet-zstd=$sniffer_wide_projection_dir/data.zstd.parquet" \
  --rows=1000000 --projected-columns=15 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-29：4 列 nullable 高熵 binary 宽投影

将前述单列 binary 场景扩展为 4 列、每个非 null 值 32 B：每列用不同的
确定性伪随机种子生成高熵字节，每 17 行一个 null。总共 5 列（包括递增
int64 key），查询 `key >= rows/2` 并投影全部 4 个 binary 列。Sniffer
与 Parquet 未压缩/ZSTD 使用同一生成函数、8,192 行 Row Group、输出 batch
上限 4,096；四条测量路径在计时前均逐列验证字节、null 和行序。
Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
系统临时目录、warm-cache。每 case 独立进程，20 次重复、每次至少 0.05 秒；
P95 为排序后第 19 个样本。RSS/Arrow pool 来自同参数另外 7 次重复的
median counter，包含 Reader 打开与计时前校验高水位。

| 行数 | 执行路径 | 文件 B | P50 / P95 ms | 候选列块 / 字节 | RSS / Arrow pool 峰值 MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100,000 | Sniffer 串行 | 13,866,903 | 3.050 / 3.093 | 29 / ≈6.979 MB（实际列块读取） | 11.99 / 2.81 |
| 100,000 | Sniffer 4-worker | 13,866,903 | 1.169 / 1.197 | 29 / ≈6.979 MB（实际列块读取） | 26.00 / 6.25 |
| 100,000 | Parquet 未压缩 | 15,240,976 | 3.232 / 3.341 | 29 / ≈7.329 MB（元数据候选量） | 20.04 / 8.15 |
| 100,000 | Parquet ZSTD | 13,800,765 | 7.153 / 7.252 | 29 / ≈6.891 MB（元数据候选量） | 21.94 / 9.09 |
| 1,000,000 | Sniffer 串行 | 138,666,963 | 32.193 / 32.466 | 249 / ≈68.545 MB（实际列块读取） | 12.03 / 2.81 |
| 1,000,000 | Sniffer 4-worker | 138,666,963 | 9.471 / 9.602 | 249 / ≈68.545 MB（实际列块读取） | 29.07 / 6.21 |
| 1,000,000 | Parquet 未压缩 | 152,410,904 | 34.105 / 34.633 | 249 / ≈71.430 MB（元数据候选量） | 86.92 / 72.40 |
| 1,000,000 | Parquet ZSTD | 138,299,872 | 75.746 / 76.460 | 249 / ≈67.840 MB（元数据候选量） | 86.28 / 70.03 |

100K 剪枝 6/13 组，1M 剪枝 61/123 组；4-worker 在途峰值 4 组、
估算预留峰值约 11.09 MB（配置预算 64 MiB）。高熵使三种文件的空间
差距收窄，Parquet ZSTD 文件比 Sniffer 略小；此场景下 Sniffer 4-worker
延迟更低，但 RSS 明显高于 Sniffer 串行。Parquet 候选字节来自元数据
`total_compressed_size`，**不是物理 I/O**，不得与 Sniffer 实际读取字节
直接比较。Parquet batch reader 与 Sniffer 流式 Reader 的分配策略不同，
RSS 差异不是单纯格式属性。此实验只覆盖一种宽度、值长度、null 分布和
选择率，不代表所有变长宽表或 cold-cache 结果。

复现 1M 行生成与一个测量 case；其他 case 只换 filter，均独立进程：

```sh
sniffer_binary_wide_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_binary_wide_dir/data.seg" --rows=1000000 \
  --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet=$sniffer_binary_wide_dir/data.parquet" --rows=1000000 \
  --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet-zstd=$sniffer_binary_wide_dir/data.zstd.parquet" --rows=1000000 \
  --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_binary_wide_dir/data.seg" \
  "--parquet=$sniffer_binary_wide_dir/data.parquet" \
  "--parquet-zstd=$sniffer_binary_wide_dir/data.zstd.parquet" \
  --rows=1000000 --projected-binary-bytes=32 --projected-columns=4 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-30：Row Group × 选择率（高熵 binary 宽投影）

继续使用 1M 行、4 列 nullable 高熵 32 B binary、递增 int64 key、每列每 17 行
一个 null 的 Reader-only 场景。分别生成 1,024、8,192、65,536、262,144 行
Row Group 的 Sniffer、Parquet 未压缩和 Parquet ZSTD 文件；四条扫描路径均在计时前
逐列校验字节、null 与行序。谓词为 `key >= floor(rows * (100 - selectivity_percent) / 100)`，
投影 4 个 binary 列，输出 batch 上限 4,096 行。Apple M4（10 逻辑核）、
AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、系统临时目录、warm-cache。
每个 case 独立进程，20 次重复、每次至少 0.05 秒；P95 为排序后的第 19 个样本。
这次实验只改变文件的 Row Group 大小与查询选择率，不是格式或库版本的前后对比。

| Row Group 行数 | 选择率 | Sniffer 串行 P50/P95 ms | Sniffer 4-worker P50/P95 ms | Parquet 未压缩 P50/P95 ms | Parquet ZSTD P50/P95 ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 | 1% | 0.811 / 0.854 | 2.860 / 2.906 | 1.473 / 1.511 | 2.397 / 2.457 |
| 1,024 | 50% | 36.627 / 37.110 | 13.659 / 14.519 | 44.145 / 45.229 | 87.507 / 89.257 |
| 8,192 | 1% | 0.904 / 0.911 | 0.883 / 0.897 | 1.182 / 1.213 | 2.532 / 2.583 |
| 8,192 | 50% | 32.645 / 33.051 | 9.634 / 9.685 | 34.797 / 36.337 | 77.168 / 78.137 |
| 65,536 | 1% | 0.929 / 0.941 | 0.998 / 1.009 | 1.092 / 1.116 | 2.292 / 2.315 |
| 65,536 | 50% | 33.217 / 33.786 | 13.679 / 13.933 | 31.729 / 33.516 | 62.890 / 64.514 |
| 262,144 | 1% | 8.957 / 9.345 | 8.904 / 9.038 | 10.736 / 10.898 | 23.780 / 24.572 |
| 262,144 | 50% | 42.859 / 43.448 | 42.904 / 44.794 | 36.934 / 37.947 | 78.812 / 79.999 |

| Row Group 行数 | Sniffer / Parquet / ZSTD 文件 MB | 总组数 | 剪枝组 1% / 50% | Sniffer 读取列块 1% / 50% | Sniffer 实际列块读取 MB 1% / 50% |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 | 138.787 / 151.895 / 138.617 | 977 | 966 / 488 | 45 / 1,957 | 1.48 / 68.58 |
| 8,192 | 138.667 / 152.411 / 138.300 | 123 | 120 / 61 | 13 / 249 | 2.34 / 68.55 |
| 65,536 | 138.978 / 149.910 / 133.625 | 16 | 15 / 7 | 5 / 37 | 2.35 / 74.27 |
| 262,144 | 139.223 / 146.471 / 130.344 | 4 | 3 / 1 | 5 / 13 | 29.73 / 101.66 |

文件 MB 与读取 MB 均为十进制 10⁶ B。1% 查询在 256K 分组上只命中尾组的一小部分，
但列块仍以整组为单位读取；Sniffer 实际读取约 29.73 MB，明显多于 8K 分组的
2.34 MB。1K 分组的 1% 查询只需 11 个候选组，4-worker 的调度成本超过工作量；
50% 查询则以 8K 分组的 4-worker P50 最低。Parquet 的候选列字节是 Footer 元数据
`total_compressed_size`，不是物理读取量，此处不与 Sniffer 实际 I/O 字节直接比较。
这些是同一分布的方向性结果，不能据此确定通用最优 Row Group。

默认 `--buffer-budget-bytes=67108864` 时，256K 分组的单组估算已超过预算，
4-worker case 实际 `parallel_workers_started=0`，安全回退串行。因此上表该行
不代表真正的并行吞吐。另在 256K、50% 场景显式增大预算，20 次重复的延迟与
另起进程 7 次重复的 median counters 如下：

| 4-worker 预算 MiB | 在途组峰值 | P50/P95 ms | 进程 RSS 高水位 MB | Arrow pool 峰值 MB |
| ---: | ---: | ---: | ---: | ---: |
| 64（默认，回退） | 0 | 42.904 / 44.794 | 121.27 | 67.12 |
| 128 | 1 | 42.017 / 43.268 | 144.34 | 67.12 |
| 256 | 3 | 18.921 / 19.297 | 197.05 | 69.05 |
| 512 | 4 | 18.919 / 19.384 | 164.00 | 69.05 |

256K 串行扫描的独立 RSS median 约 121.18 MB。预算是 Row Group 在途**估算**，
不是进程 RSS 上限；高水位包含 Reader 打开和计时前校验，独立进程的 RSS 还受分配器
与系统状态影响，故不能把 256/512 MiB 两行的 RSS 差异解释为预算增加降低内存。
此工作负载在 50% 时只有 3 个候选组，512 MiB 未带来可确认的额外速度收益。
默认仍保持串行，内部并行需明确 opt-in；cold-cache、其他宽度/变长分布仍未覆盖。

复现时每种 Row Group 大小分别生成三个文件，扫描传入相同的 `--row-group-rows`；
下面给出 1,024 行分组的一个 case，其他组合替换参数并新起进程：

```sh
sniffer_rg_dir=$(mktemp -d)
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate=$sniffer_rg_dir/data.seg" --rows=1000000 --row-group-rows=1024 \
  --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet=$sniffer_rg_dir/data.parquet" --rows=1000000 \
  --row-group-rows=1024 --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--generate-parquet-zstd=$sniffer_rg_dir/data.zstd.parquet" --rows=1000000 \
  --row-group-rows=1024 --projected-binary-bytes=32 --projected-columns=4
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_rg_dir/data.seg" "--parquet=$sniffer_rg_dir/data.parquet" \
  "--parquet-zstd=$sniffer_rg_dir/data.zstd.parquet" --rows=1000000 \
  --row-group-rows=1024 --projected-binary-bytes=32 --projected-columns=4 \
  --selectivity-percent=1 \
  '--benchmark_filter=^ReaderOnlyScan/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-29：同文件 1% / 10% / 50% / 100% 选择率

上述 1M 行、4 列 nullable 高熵 binary 文件不变（Sniffer 138,666,963 B，
Parquet 未压缩 152,410,904 B，Parquet ZSTD 138,299,872 B）。只改变
`key >= floor(rows * (100 - selectivity_percent) / 100)`；三种格式均用
同一阈值、相同投影、8,192 行 Row Group 和 4,096 行输出 batch。
计时前按每条实际路径逐列验证字节、null 和行序。
Apple M4（10 逻辑核）、AppleClang 21、Arrow C++ 23.0.1、Release `-O3`、
系统临时目录、warm-cache；每个 case 独立进程，20 次重复、每次至少
0.05 秒。P95 为排序后第 19 个样本。

| 选择率 | Sniffer 串行 P50/P95 ms | Sniffer 4-worker P50/P95 ms | Parquet 未压缩 P50/P95 ms | Parquet ZSTD P50/P95 ms |
| ---: | ---: | ---: | ---: | ---: |
| 1% | 0.879 / 0.890 | 0.871 / 0.882 | 1.166 / 1.216 | 2.496 / 2.555 |
| 10% | 6.581 / 6.669 | 2.451 / 2.478 | 7.473 / 7.577 | 16.143 / 16.862 |
| 50% | 32.791 / 33.183 | 9.776 / 9.917 | 34.833 / 35.443 | 76.804 / 78.228 |
| 100% | 63.800 / 65.340 | 17.755 / 18.960 | 70.619 / 71.416 | 152.574 / 155.561 |

| 选择率 | 剪枝 Row Group | 候选列块 | Sniffer 实际列块读取 MB | Parquet 未压缩候选列 MB | Parquet ZSTD 候选列 MB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1% | 120/123 | 13 | ≈2.337 | ≈2.497 | ≈2.302 |
| 10% | 109/123 | 57 | ≈14.681 | ≈15.349 | ≈14.521 |
| 50% | 61/123 | 249 | ≈68.545 | ≈71.430 | ≈67.840 |
| 100% | 0/123 | 492 | ≈136.984 | ≈142.620 | ≈135.514 |

Parquet 候选字节仍是列元数据 `total_compressed_size` 之和，**非物理 I/O**；
不能直接和 Sniffer 实际列块读取量比较。另以 7 次重复的 median counter
记录进程 RSS 高水位：1% 时 Sniffer 串行/4-worker 约 10.94/14.02 MB，
Parquet 未压缩/ZSTD 约 14.91/15.71 MB；100% 时依次约
8.73/26.17 MB 和 160.50/161.99 MB。这些值包含 Reader 打开和计时前
校验，Parquet 当前 batch reader 的分配策略也会影响峰值。

1% 只有末尾 3 个候选组，4-worker 与串行 P50 差约 0.008 ms，
不能据此认为并行值得开启；10% 及以上在这组高熵 binary 数据上收益
明显，但 RSS 增加。这里不能推导通用的自动开启阈值，默认仍为串行，
并行需显式 opt-in。这个矩阵仍是 warm-cache、单一 Row Group 大小与
一种数据分布；cold-cache 等 v0.2 项继续未完成。

复现时复用上一节生成的三个 1M 行文件，对每个选择率与格式 case
分别启动新进程，例如：

```sh
./build-release/sniffer_core_reader_memory_benchmark \
  "--segment=$sniffer_binary_wide_dir/data.seg" \
  "--parquet=$sniffer_binary_wide_dir/data.parquet" \
  "--parquet-zstd=$sniffer_binary_wide_dir/data.zstd.parquet" \
  --rows=1000000 --projected-binary-bytes=32 --projected-columns=4 \
  --selectivity-percent=1 \
  '--benchmark_filter=^BoundedParallelReaderScan/4/real_time$' \
  --benchmark_repetitions=20 --benchmark_min_time=0.05s --benchmark_format=csv
```

## 2026-09-30：并行扫描只调度候选 Row Group

此前并行 worker 对每个物理 Row Group 都创建任务，进入任务后才用索引剪枝；
1K/1% 场景的 977 组中有 966 组被剪枝，调度成本超过实际读取。现在调用线程
按原始组序、在有界窗口内先检查索引，仅把候选组交给 worker；剪枝组直接计入
扫描指标。候选列块读取、输出顺序、错误传播、`limit` 串行回退与格式均未改动。
完全剪枝时不启动 worker。新增回归先在旧实现复现“只有 1 个候选组，
在途峰值却为 3”，新实现在途峰值为 1。

Apple M4 / AppleClang 21 / Arrow 23.0.1 / Release `-O3` / warm-cache；
1M 行、4 列 nullable 高熵 32 B binary、4-worker、默认 64 MiB 预算。
每个 case 独立进程，20 次重复、每次至少 0.05 秒，取 P50 和第 19 个样本为 P95。
改前为上一节的提交 `a186ef6` 测量，改后为当前工作树；不是交错 A/B，
因此小幅差异仍可能包含系统波动。

| Row Group 行数 | 选择率 | 改前 P50/P95 ms | 改后 P50/P95 ms | 备注 |
| ---: | ---: | ---: | ---: | --- |
| 1,024 | 1% | 2.860 / 2.906 | 0.597 / 0.609 | 966/977 组剪枝；明显降低调度开销 |
| 1,024 | 50% | 13.659 / 14.519 | 14.516 / 14.757 | 高候选密度下出现约 6% P50 回退，需继续评估 |
| 8,192 | 1% | 0.883 / 0.897 | 0.630 / 0.643 | 120/123 组剪枝 |
| 8,192 | 50% | 9.634 / 9.685 | 9.432 / 9.576 | 小幅差异不归因 |
| 65,536 | 1% | 0.998 / 1.009 | 0.973 / 1.001 | 小幅差异不归因 |
| 65,536 | 50% | 13.679 / 13.933 | 13.746 / 14.075 | 小幅差异不归因 |

改后 1K/1% 的 7 次 median counter：剪枝 966/977 组、45 个 ColumnChunk、
实际读取约 1.484 MB、4 个 worker、候选组在途峰值 4；读取量和输出行数
与改前相同。1K/50% 的改后计数为剪枝 488/977 组、1,957 个 ColumnChunk、
实际读取约 68.581 MB，在途峰值同为 4。8K/100% 的改后 P50/P95 为
17.470/17.844 ms，改前上一轮为 17.755/18.960 ms；该两轮未交错，
不据此宣称全扫描提速。50% 高候选密度回退说明后续需要对调度与主线程剪枝
再做可归因的交错 A/B，不能把稀疏查询收益推广到所有选择率。

后续尝试以 16 个分散的索引探针估计候选密度，高密度时将剪枝移回 worker。
同机同文件、各 case 20 次重复的 P50/P95：1K/1% 为 0.593/0.604 ms，
1K/50% 为 15.711/15.949 ms，8K/1% 为 0.616/0.620 ms，8K/50% 为
9.563/9.694 ms。目标 1K/50% 比当前候选组调度的 14.516/14.757 ms 更慢；
试验代码已撤回，不采用该启发式，也不更新上述正式基准表。

## 2026-09-30：macOS 文件缓存旁路对照（非 cold-cache）

Reader-only 增加扫描专用 `--cache-bypass`：Sniffer 和 Parquet Reader 均在各自
只读文件描述符上启用 macOS `F_NOCACHE`。它不清空已有页缓存，也不控制设备缓存；
以下只代表**缓存旁路请求**，绝不作为真正 cold-cache 或超页缓存验收。默认路径
没有改动。使用上述 1M 行、8,192 行 Row Group、4 列 nullable 高熵 32 B binary、
50% 选择率、投影 4 列的同一批文件；Sniffer/Parquet 未压缩/ZSTD 文件分别为
138,666,963 / 152,410,904 / 138,299,872 B。四条路径在计时前逐列验证，
均输出 500,000 行；Sniffer 剪枝 61/123 组、读 249 个列块与约 68.545 MB，
两种缓存模式相同。

Apple M4（10 逻辑核）/ AppleClang 21 / Arrow C++ 23.0.1 /
Release `-O3` / 系统临时目录。每个 case 独立进程、7 次重复、每次至少
0.05 秒，计时包含创建扫描迭代器及读取/解码，不包含生成和计时前逐值校验。
下表 P50 与范围来自第二轮完整原始样本；不是交错 A/B，差异可能含系统波动，
不据此宣称格式性能变化。每组 7 个样本不足以可靠估计 P95。

| 执行路径 | 默认 P50 ms | 旁路 P50 ms | 默认范围 ms | 旁路范围 ms |
| --- | ---: | ---: | ---: | ---: |
| Sniffer 串行 | 31.295 | 32.391 | 30.735–31.963 | 31.987–32.637 |
| Sniffer 4-worker | 9.393 | 9.454 | 9.326–9.468 | 9.375–9.471 |
| Parquet 未压缩 | 34.627 | 35.303 | 34.286–36.176 | 34.823–35.840 |
| Parquet ZSTD | 75.239 | 75.817 | 74.056–76.633 | 75.306–76.966 |

排序后的原始 `real_time` 样本（ms）：

| 路径 | 默认 7 次 | 旁路 7 次 |
| --- | --- | --- |
| Sniffer 串行 | 30.7348, 31.0176, 31.2544, 31.2945, 31.6633, 31.7687, 31.9630 | 31.9868, 32.0283, 32.0611, 32.3909, 32.4494, 32.4546, 32.6370 |
| Sniffer 4-worker | 9.32621, 9.32961, 9.34323, 9.39323, 9.41792, 9.44854, 9.46754 | 9.37494, 9.40145, 9.45029, 9.45437, 9.45468, 9.45531, 9.47067 |
| Parquet 未压缩 | 34.2856, 34.3438, 34.5686, 34.6267, 34.6537, 35.9995, 36.1761 | 34.8226, 35.0524, 35.1546, 35.3025, 35.4919, 35.6000, 35.8402 |
| Parquet ZSTD | 74.0560, 74.3345, 74.7102, 75.2387, 75.3799, 75.8351, 76.6325 | 75.3056, 75.4495, 75.7425, 75.8167, 76.2486, 76.4548, 76.9663 |

复现时分别新起扫描进程，并只在 macOS pread 构建上加 `--cache-bypass`；
其他参数与上文 1M 行 binary 宽投影场景相同。缓存模式会写入 Google Benchmark
的 `cache_mode` context。方法与平台限制见
[`Decision 0006`](../docs/decisions/0006-macos-cache-bypass-measurement.md)。
