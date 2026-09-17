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
