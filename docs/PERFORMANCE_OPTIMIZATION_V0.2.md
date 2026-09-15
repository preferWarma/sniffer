# v0.2 性能优化专项 TODO

**状态：** Proposed

**目标版本：** v0.2

**基线：** [`bench/BENCHMARK_V1.md`](../bench/BENCHMARK_V1.md)

## 1. 目标与边界

v0.2 聚焦现有 Segment writer、reader、codec、scan 和文件 I/O 路径的性能，不扩大
`sniffer-core` 的业务边界。优化必须保持 Arrow 语义、确定性输出、边界检查、checksum、结构化
错误以及 v0.1 文件的可读性。

本专项不默认引入 RocksDB、对象存储、SQL、RPC、事务、后台线程池或完整 nested type。SIMD、
多级编码链和新压缩算法只有在标量路径完成剖析和优化后才进入实现；任何新 encoding 都必须有
显式 ID、兼容策略、decision record 和未知编码失败测试。

## 2. v0.1 基线与 v0.2 目标

固定基线环境为 Apple M4、10 逻辑核、Arrow C++ 23.0.1、Apple Clang 21、Release、单线程。
以下结果来自 v1 benchmark，不代表跨机器承诺：

| 指标 | v0.1 基线 | v0.2 目标 |
|---|---:|---:|
| 性能场景写入吞吐 | 1.37 M rows/s | >= 4.0 M rows/s |
| 50% 选择率扫描吞吐 | 5.27 M rows/s | >= 15.0 M rows/s |
| 性能场景端到端吞吐 | 1.09 M rows/s | >= 3.0 M rows/s |
| 压缩场景合计编码写入 | 78.8 MiB/s | >= 250 MiB/s |
| 压缩场景合计解码读取 | 133.5 MiB/s | >= 400 MiB/s |
| 高基数随机字符串压缩比 | 0.874x | >= 0.98x，或明确记录格式开销原因 |

硬性非回归门槛：

- 每个 v1 压缩场景的压缩比不得下降超过 2%，除非 decision record 说明空间与速度取舍；
- 相同输入和配置仍生成字节完全一致的文件；
- 性能场景仍剪枝 6/13 Row Group，且读取 ColumnChunk 数和字节数不得增加；
- Reader 峰值内存继续受 output batch、当前 Row Group 和投影列约束；
- 普通测试、property/fuzz smoke、ASan 和 UBSan 全部通过；
- 不允许通过关闭 checksum、减少格式校验或放宽错误语义换取性能。

这些数值是专项方向目标。最终 v0.2 验收以同机、同数据、同编译参数的 before/after 重复测量为准，
不得使用单次 wall-clock 结果宣称提升。

## 3. 先建立可归因的测量

- [ ] benchmark 输出每轮原始耗时，并同时报告 P50、P95、最小值和变异系数；保留现有中位数，
      避免破坏脚本消费者。
- [ ] 增加机器可读结果格式（JSON 或 CSV），记录 commit、编译器、Arrow 版本、构建参数和运行命令。
- [ ] 将文件级 benchmark 与纯内存 codec microbenchmark 分开；后者分别测 Plain、Dictionary、
      RLE、FOR + Bitpack 的 encode/decode，不包含文件打开、footer、index 和 checksum。
- [ ] 增加 writer 分阶段计时：编码选择、Plain/非 Plain 编码、索引构建、checksum、文件写入、
      footer/trailer。
- [ ] 增加 reader 分阶段计时：Open/全文件校验、索引解析、chunk I/O、chunk checksum、解码、
      谓词、selection materialization、RecordBatch 拼接。
- [ ] 使用 Instruments 或等价 profiler 保存 CPU flamegraph 摘要和 allocation hot spots；先证明
      热点，再修改实现。
- [ ] 增加峰值 RSS、Arrow memory pool 峰值和每输入行分配次数指标。
- [ ] 扩展固定矩阵：Row Group 1K/8K/64K/256K，选择率 1%/10%/50%/100%，单列/多列/全列投影。
- [ ] 增加至少一个宽表和一个超过页缓存容量的数据集；分别报告 warm-cache 与 cold-cache 结果。

## 4. P0：消除已知重复工作和逐值对象开销

以下项目由当前实现的静态检查得出，是待 profiler 验证的高概率热点。

### 4.1 Writer

- [ ] 先执行编码选择，再生成最终 payload。当前 `WriteRowGroup()` 总是完整生成 Plain payload，
      非 Plain 编码命中时又编码一次；改为用无分配的长度计算得到 `uncompressed_length`。
- [ ] 合并编码选择采样、statistics、sort-key 和实际编码可复用的数据遍历，避免同一列重复
      `GetScalar()`、序列化和比较。
- [ ] 为整数、timestamp、bool、string/binary 增加 typed encoder 路径，避免逐值构造
      `arrow::Scalar` 和调用 `SerializeScalar()`。
- [ ] 为 `ByteWriter` 增加可计算的容量预留和批量 append，减少 vector 扩容及中间 payload 拷贝。
- [ ] 评估直接编码到最终 chunk buffer，并在一次顺序遍历中计算 chunk CRC32C；不得改变 CRC
      算法或落盘字节。

### 4.2 Reader 与 codec

- [ ] Dictionary decode 直接向 typed Arrow builder/buffer 写入，不构造
      `vector<shared_ptr<arrow::Scalar>>`。
- [ ] RLE decode 使用 run-aware 批量 append；selection 路径按 run 跳过未命中范围，不展开所有行。
- [ ] FOR decode 使用 typed base/delta 和按字/批量 bit unpack，移除每值 `ScalarFromBits()`。
- [ ] Plain selected decode 按物理类型直接 append，移除每个命中行的 `ParseScalar()` 和
      `AppendScalar()` 动态分派。
- [ ] `RowsToDecode()` 的全量路径改为顺序迭代视图，避免构造 `[0..row_count)` 临时 vector。
- [ ] 对每项 typed fast path 保留通用安全 fallback，并用 property tests 证明两条路径逐值一致。

## 5. P1：优化扫描执行

- [ ] 在计划校验时一次性把 `field_id` 解析为列下标，避免 predicate、projection 和逐行判断中
      重复线性调用 `FindFieldIndex()`。
- [ ] 为各基础类型实现 typed predicate kernel，直接读取 Arrow values/validity；避免逐行
      `GetScalar()`、虚调用和临时对象。
- [ ] 将多个 AND predicate 融合到同一次 selection 构建，优先执行成本低、选择性高的谓词。
- [ ] 比较 index vector、bitmap 和连续 range 表示在 1%/10%/50%/100% 选择率下的成本，使用
      确定性阈值选择表示。
- [ ] projection 与 predicate 是同一列时，直接从已解码列构造输出，提供 typed take/filter 路径。
- [ ] 减少跨 Row Group 输出时的 `ConcatenateRecordBatches()` 拷贝；优先返回合法 slice，只有确实
      需要单个连续 batch 时才合并。
- [ ] 针对 `limit` 做早停，确保不解码超过最后命中行所需的 projection 数据。
- [ ] 增加指标验证：谓词列、投影列、selection、拼接各阶段的行数、字节数和耗时可观测。

## 6. P1：优化文件 I/O 与 checksum

- [ ] Reader 生命周期内复用一个随机访问文件句柄。当前 `ReadRange()` 每次读取 chunk 都重新
      打开文件并 seek，应改为有明确所有权的 `RandomAccessFile`/等价 RAII 抽象。
- [ ] 合并相邻或距离很近的候选 chunk 读取，减少系统调用，同时保证未命中列块不会被读取。
- [ ] 评估 Arrow buffer、pread 和 mmap 三种 backend；默认实现必须可移植，mmap 只能作为可选
      backend，且所有 offset/length 仍先做溢出与边界检查。
- [ ] 避免文件 checksum 验证和后续 chunk 读取造成不必要的重复拷贝；允许缓存“已验证”状态，
      但每个新 Reader 仍必须按格式契约完成验证。
- [ ] 使用批量或硬件加速 CRC32C 前先建立独立 benchmark；实现必须与当前 CRC32C 字节结果一致。
- [ ] Writer 增大顺序写缓冲、减少小 write；`Finish()` 的 durability 语义不得被悄然改变。

## 7. P2：编码选择与空间效率

- [ ] 让选择器同时估算最终 payload 大小、编码 CPU 成本和预期解码成本，而不是仅按样本大小阈值
      决策；模型必须确定、可解释、可测试。
- [ ] 对所有候选编码加入“收益不足则 Plain”的保护，并记录选择原因供 benchmark 观测。
- [ ] 单独分析高基数 string/binary 的 64-bit offset 开销。如果引入 32-bit compact offset 或
      CompactPlain，必须分配新 encoding ID，旧 Reader 对未知 ID 明确失败，v0.2 Reader 保持读取
      v0.1 Plain 的能力。
- [ ] 评估 bool bitpack、delta-of-delta timestamp 和 dictionary index + RLE；只有现有 P0/P1
      优化完成、microbenchmark 证明收益后再进入格式设计。
- [ ] 新 encoding 必须补齐随机、极值、null、截断、错误 offset、checksum 和 selection decode
      测试，并更新 format decision record。

## 8. P3：可选向量化与并行

- [ ] 在标量 typed kernel 稳定后，为 bit unpack、predicate 和 bitmap 操作评估 NEON/AVX2；提供
      编译期/运行时能力检测和完全等价的标量 fallback。
- [ ] SIMD 实现不得使用不安全 reinterpret-cast 解析外部输入；先验证 buffer 边界和规范性。
- [ ] 评估跨 ColumnChunk/Row Group 的可选并行编码与解码。公共 API 默认语义、输出顺序和文件
      确定性必须保持不变，单线程仍是基准和 fallback。
- [ ] 线程数、任务粒度和内存预算必须由显式配置控制，不使用无上限异步任务。

## 9. 推荐实施顺序

1. 完成分阶段计时、allocation 指标和 codec microbenchmark，产出第一份 profile。
2. 优化 writer 的“双重编码”和 scalar 分配，逐项跑压缩比与 encode benchmark。
3. 优化 Dictionary/RLE/FOR/Plain typed decode，逐项跑 round-trip、fuzz 和 decode benchmark。
4. 复用 reader 文件句柄并优化 chunk 读取，再测 warm/cold cache。
5. 优化 typed predicate、selection 和 batch 拼接，再跑完整 IOPlan 矩阵。
6. 修正编码 cost model 和高基数字符串空间开销。
7. 只有标量路径仍是 CPU 热点时才实现 SIMD；只有单线程吞吐达标后才评估并行。

每个性能提交必须只处理一个可归因热点，并附同机 before/after 原始指标。若性能提升伴随空间、
内存或复杂度回退，提交说明和 v0.2 benchmark 报告必须显式列出取舍。

## 10. v0.2 完成定义

- [ ] 第 2 节正确性和非回归门槛全部满足；
- [ ] 至少达到第 2 节中的 writer、scan、encode、decode 四个吞吐目标；
- [ ] benchmark 矩阵覆盖第 3 节规定的 Row Group、选择率、投影和缓存维度；
- [ ] 所有性能结论都有 profiler 证据和至少 7 次重复测量；
- [ ] 生成 `bench/BENCHMARK_V2.md`，同时记录绝对值、相对 v1 的变化、压缩比、峰值内存和原始
      运行参数；
- [ ] 所有新增格式语义均有 decision record，v0.2 Reader 可读取 v0.1 Segment；
- [ ] 更新 README 的性能状态，不把合成 benchmark 结果表述为通用生产性能。
