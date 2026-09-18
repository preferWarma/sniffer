# v0.2 CPU 与分配热点剖析

**日期：** 2026-09-17  
**对应源码：** `06b5d22`（core 与完成 trace 中的 `661a402-dirty` 相同；dirty 内容随后作为
`06b5d22` 提交，仅增加 benchmark 矩阵与内存指标）  
**环境：** Apple M4、10 逻辑核、macOS、Apple Clang 21、Arrow C++ 23.0.1、Release、单线程

## 1. 目标与场景

本轮不是吞吐排名，而是为下一项优化选择提供归因证据。固定场景为 100,000 行、Row Group 4,096、
`id >= 50,000`、投影 `id,value`，覆盖完整 Segment 写入和 50% 选择率扫描：

```text
Performance/Sniffer/100000/4096/manual_time
```

完成的 Instruments Time Profiler trace 持续 6.33 秒，目标进程正常退出。为排除启动与 benchmark
注册噪声，统计窗口从首个包含 `RunPerformance` 的样本开始，共计 4,582 ms 的采样权重。

## 2. CPU 热点

以下 self time 互斥，可用于判断 CPU 实际停留位置：

| 符号 | Self time | 占工作负载样本 |
|---|---:|---:|
| `EncodeNonPlain` | 1,472 ms | 32.13% |
| 未解析符号 | 1,163 ms | 25.38% |
| `HashArrayValuePair` | 218 ms | 4.76% |
| `BuildRowGroupIndex` | 216 ms | 4.71% |
| `DecodeForValues` / `DecodeForBitpack` | 142 ms | 3.10% |
| `CompareArrayRows` | 111 ms | 2.42% |
| `Crc32c` | 110 ms | 2.40% |
| `ArrayIntegralBits` | 66 ms | 1.44% |
| `SelectEncoding` | 61 ms | 1.33% |
| `write` syscall | 60 ms | 1.31% |
| `CompareIntegralBits` | 60 ms | 1.31% |

Inclusive time 会重叠，不能求和，但可说明调用路径归属：`EncodeNonPlain` 为 34.42%，
`SegmentWriter::Impl::WriteRowGroup` 为 17.00%，`BuildRowGroupIndex` 为 6.63%，`LoadIndexes` 为
2.38%，`ReadNextMatchingRowGroup` 为 1.53%。benchmark 自身的分阶段指标也显示写入约 14.5 ms，
扫描约 2.0 ms。因此当前完整路径的首要热点仍是 writer，尤其是非 Plain 编码及其之前的索引/
选择器遍历；CRC32C 不是下一项最高优先级。

## 3. Allocation hot spots

Instruments Allocations 在本机连续两次无法附加目标进程，因此改用 macOS
`MallocStackLogging=1` 和 `malloc_history`，在 benchmark 运行中抓取 live allocation 快照。
MallocStackLogging 会显著拖慢运行，下面只分析分配归属，不使用该次 wall-clock 结果：

| 最深 Sniffer frame | Live bytes | Live allocations |
|---|---:|---:|
| `EncodeNonPlain` | 57,344 | 2 |
| `BuildRowGroupIndex` | 10,864 | 8 |
| `SegmentWriter::Impl::WriteUntracked` | 5,120 | 1 |
| `SegmentWriter::Impl` / `ofstream` 初始化 | 5,120 | 1 |
| `WriteRowGroup` | 3,456 | 18 |
| Row Group metadata vector 增长 | 2,048 | 1 |
| `Append` | 992 | 9 |
| writer implementation object | 896 | 1 |
| `EncodeValidity` | 640 | 1 |
| `KeyAt` | 192 | 2 |
| `ValidateAndUpdateSortOrder` | 128 | 1 |

快照中的 8 个 1 GiB mimalloc 虚拟地址空间预留不是 live payload，已排除。该表是单时刻的 live
分配，不表示累计 allocation traffic；它仍与 CPU profile 一致地把非 Plain payload 和索引构建列为
主要的项目内分配来源。

## 4. 复现命令

Time Profiler：

```sh
xcrun xctrace record --no-prompt --template 'Time Profiler' --time-limit 8s \
  --output /tmp/sniffer-time.trace \
  --target-stdout /tmp/sniffer-time.stdout \
  --launch -- ./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^Performance/Sniffer/100000/4096/manual_time$' \
  --benchmark_min_time=2s
xcrun xctrace export --input /tmp/sniffer-time.trace --toc
```

Allocation 快照：

```sh
MallocStackLogging=1 ./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^Performance/Sniffer/100000/4096/manual_time$' \
  --benchmark_min_time=3s
malloc_history <pid> -allBySize
malloc_history <pid> -allByCount
```

## 5. 结论与下一项优化

第一项后续优化先处理 `EncodeNonPlain` 内最内层的 FOR bitpack：保持 base/delta 分析不变，将逐 bit
写入替换为按 byte 写入。该改动已把完整场景的 writer encoding P50 从 6.22 ms 降到 2.32 ms；详见
专项 TODO 的执行记录。

此后再处理 writer 的重复列遍历：让编码选择、statistics/Bloom/sort-key index 和最终编码复用一次
typed 分析结果，先从当前 benchmark 中的整数和字符串列落地。验收必须保持 payload 字节、checksum、
索引内容、压缩比和确定性输出完全不变，并用分阶段指标分别观察 encoding selection、index 与
encoding，而不是只看端到端吞吐。

暂不优先实现 SIMD 或 CRC32C 专项：当前最大已解析 self hotspot 是 `EncodeNonPlain`，而 CRC32C
只有 2.40%。也不根据本次 profile 给出精确的全路径百分比分解，因为仍有 25.38% 样本未完成符号
解析，且 inclusive time 存在重叠。

## 6. `e0e3467` 后的热点复查

ARM CRC32C、FOR bytewise pack 和 typed index 优化完成后，在相同的 100,000 行、Row Group 4,096、
50% 选择率场景上重新采样。`sample` 以 1 ms 周期采集 5 秒，目标主线程获得 4,280 个样本；
top-of-stack 前列为：

| 符号 | top-of-stack 样本 |
|---|---:|
| `HashArrayValuePair` | 451 |
| `EncodeNonPlain` | 395 |
| FOR typed decode | 277 |
| `BuildRowGroupIndex` | 231 |
| `Crc32c` | 92 |
| `SelectEncoding` | 89 |

对应的分阶段 P50 为：writer 约 7.00 ms，其中 encoding 2.39 ms、encoding selection 1.80 ms、
index 1.62 ms、checksum 0.12 ms。CRC 已不再是优先瓶颈；writer 的重复列分析仍是最明确的下一项。

第一步只让 FOR encoder 复用已持久化 statistics 的 minimum，跳过自身的 base 查找遍历；没有
statistics 的字段继续使用原安全路径。11 次同机 Release P50 显示 encoding 从 2.3872 ms 降至
1.7444 ms（-26.9%），writer 从 6.9960 ms 降至 6.3436 ms（-9.3%），端到端从 8.2511 ms
降至 7.6080 ms（-7.8%）。文件仍为 663,679 字节，Arrow 分配、剪枝结果、ColumnChunk 读取数与
读取字节均不变。

第二小步让非排序整数 statistics 的 typed 遍历同时生成 selector sample。selection P50 从
1.8225 ms 降至 1.0945 ms，index 因接管相同工作从 1.6207 ms 升至 2.1369 ms；两阶段合计仍从
3.4432 ms 降至 3.2314 ms（-6.2%），writer 降至 6.1797 ms，端到端降至 7.4572 ms。该结果
确认遍历融合方向有效，同时说明后续报告必须观察阶段合计，不能把工作迁移误报为净收益。

Bloom string 与 selector 的完整遍历融合在两版实现中均未产生稳定净收益，已撤销。随后只绑定
Bloom 的字段、array 和 physical type，保留原哈希字节与双种子算法；index P50 从 2.1369 ms 降至
1.9700 ms，writer 从 6.1797 ms 降至 6.0562 ms，端到端从 7.4572 ms 降至 7.3221 ms。下一轮
profile 应基于该版本重新采样，不再沿用本节开头 `e0e3467` 的热点占比。

## 7. `984d3c5` 后的热点复查

在相同的 100,000 行、Row Group 4,096、50% 选择率 Release 场景上重新采样，主线程主要
top-of-stack 样本为：

| 符号 | top-of-stack 样本 |
|---|---:|
| `ArrayValuePairHasher::Hash` | 455 |
| FOR typed decode | 321 |
| `BuildRowGroupIndex` | 298 |
| `EncodeNonPlain` | 273 |
| file write syscall | 219 |
| UTF-8 validation | 114 |
| sort validation | 105 |
| string hash-map insertion | 104 |
| CRC32C | 94 |
| `BuildStatistics` | 93 |
| `ArrayIntegralBits` | 92 |
| `SelectEncoding` | 76 |

Bloom 排名第一，但该路径现在主要执行格式要求的实际双哈希；FOR decode 则仍逐 bit 读取 delta，
因此选择后者作为下一项局部优化。将 unpack 改为首尾 partial byte 加中间 whole bytes 后，FOR decode
microbenchmark P50 从 383 us 降至 314 us（-18.0%），吞吐从 1.979 GiB/s 升至 2.412 GiB/s。

完整固定场景 11 次 P50 中，scan 从 1.2681 ms 降至 0.9628 ms，端到端从 7.3221 ms 降至
6.8913 ms，吞吐从 13.657 M rows/s 升至 14.511 M rows/s。文件字节、分配、剪枝和读取量均不变；
全部 bit width 0–64 的 full/selected decode 测试用于验证非对齐读取与 null 语义。

## 8. `6747ebf` 后的热点复查

FOR bytewise unpack 合入后，在固定场景重新采样。排除 benchmark 启动期的 CPU 信息探测和 dyld
样本后，主要 top-of-stack 为：

| 符号 | top-of-stack 样本 |
|---|---:|
| `ArrayValuePairHasher::Hash` | 294 |
| `BuildRowGroupIndex` | 228 |
| `EncodeNonPlain` | 221 |
| file write syscall | 149 |
| allocator free | 119 |
| `memcmp` | 115 |
| FOR typed decode | 109 |
| sort validation | 107 |
| `BuildStatistics` | 83 |
| `ArrayIntegralBits` | 71 |
| CRC32C | 71 |
| `EncodeValidity` | 69 |

FOR decode 已明显下降，writer 的 `EncodeNonPlain` 与逐行 `ArrayIntegralBits` 仍可局部消除。将 FOR
delta 的 type switch 移到循环外后，encode microbenchmark P50 从 670 us 降至 575 us（-14.2%）；
固定场景 writer encoding 从 1.7211 ms 降至 1.5797 ms，端到端从 6.8913 ms 降至 6.7926 ms。
Bloom 双哈希仍是首位，但当前样本主要对应实际哈希工作，后续若继续优化必须保留完全相同的哈希字节
与双种子结果。

## 9. Bloom known-valid 快路径

对 `6747ebf` 后的索引调用链检查显示，Bloom 外层循环与 bound hasher 都验证了一次 null；hasher
还为每个已知有效的 row 构造 `Result<pair<uint64_t, uint64_t>>`。保留安全 `Hash()` 入口，同时让
索引循环在检查 row 后调用无重复验证的 `HashKnownValid()`。

全部平铺类型 reference test 证明两个入口与 Scalar reference 的双种子结果一致。固定场景的两轮
11 次测试中，writer index P50 分别为 1.7269 ms 和 1.7161 ms；相对 1.9879 ms 基线稳定降低约
13%。第二轮 wall/CPU CV 为 0.46%/0.47%，端到端 P50 从 6.7926 ms 降至 6.4689 ms，吞吐从
14.722 M rows/s 升至 15.459 M rows/s。文件字节、分配、剪枝和读取量不变。

## 10. `f291d30` 后的热点复查与 validity bitmap

Bloom known-valid 快路径合入后重新采样，排除 benchmark 启动样本后的 top-of-stack 前列为：

| 符号 | top-of-stack 样本 |
|---|---:|
| `EncodeNonPlain` | 229 |
| `ArrayValuePairHasher::HashKnownValid` | 225 |
| `BuildRowGroupIndex` | 211 |
| file write syscall | 169 |
| allocator free | 140 |
| `memcmp` | 121 |
| FOR typed decode | 107 |
| `BuildStatistics` | 93 |
| CRC32C | 87 |
| sort validation | 87 |
| `EncodeValidity` | 77 |

`EncodeValidity` 每个 nullable chunk 逐行构造 bitmap，但 Arrow 已持有相同位图。改为从 Arrow
bitmap 批量复制并支持非 byte-aligned slice 后，nullable FOR encode P50 从 575 us 降至 400 us
（-30.4%），writer encoding 从 1.5698 ms 降至 1.4255 ms。完整场景端到端从 6.4561 ms 降至
6.3531 ms；文件字节、分配、剪枝和读取量不变。

## 11. `07b2302` 后的 statistics sample 分配

validity bitmap 优化合入后重新采样，`BuildRowGroupIndex` 以 256 个 top-of-stack 样本成为首位。
调用树显示 `BuildStatistics` 的整数 sample distinct 统计产生大量 `unordered_set<uint64_t>` node
分配和释放；默认每个 Row Group 对 nullable value 列分析 1,024 行。

小 sample 改为预留连续 vector 并在遍历后排序去重；超过 4,096 行继续使用原 hash-set fallback。
固定场景的三轮 writer index P50 为 1.2281、1.2274 和 1.2402 ms，相对 1.7176 ms 基线保守降低
27.8%。最终确认运行的 writer 从 5.3795 ms 降至 4.9502 ms，端到端从 6.3531 ms 降至
5.9501 ms，吞吐从 15.740 M rows/s 升至 16.807 M rows/s。持久化 bytes、编码选择、分配指标、
剪枝和读取量不变。
