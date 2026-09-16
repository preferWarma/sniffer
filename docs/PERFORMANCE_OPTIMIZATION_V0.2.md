# v0.2 性能优化专项 TODO

**状态：** In Progress

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

- [x] benchmark 输出每轮原始耗时，并同时报告 P50、P95、最小值和变异系数；保留现有中位数，
      避免破坏脚本消费者。
- [x] 增加机器可读 JSON 结果格式，记录 commit、编译器、Arrow 版本、构建参数和运行命令。
- [x] 将文件级 benchmark 与纯内存 codec microbenchmark 分开；后者分别测 Plain、Dictionary、
      RLE、FOR + Bitpack 的 encode/decode，不包含文件打开、footer、index 和 checksum。
- [x] 增加 writer 分阶段计时：编码选择、Plain/非 Plain 编码、索引构建、checksum、文件写入、
      footer/trailer。
- [x] 增加 reader 分阶段计时：Open/全文件校验、索引解析、chunk I/O、chunk checksum、解码、
      谓词、selection materialization、RecordBatch 拼接。
- [ ] 使用 Instruments 或等价 profiler 保存 CPU flamegraph 摘要和 allocation hot spots；先证明
      热点，再修改实现。
- [x] 增加峰值 RSS、Arrow memory pool 峰值和每输入行分配次数指标。
- [x] 扩展固定矩阵：Row Group 1K/8K/64K/256K，选择率 1%/10%/50%/100%，单列/多列/全列投影。
- [ ] 增加至少一个宽表和一个超过页缓存容量的数据集；分别报告 warm-cache 与 cold-cache 结果。

## 4. P0：消除已知重复工作和逐值对象开销

以下项目由当前实现的静态检查得出，是待 profiler 验证的高概率热点。

### 4.1 Writer

- [x] 先执行编码选择，再生成最终 payload。当前 `WriteRowGroup()` 总是完整生成 Plain payload，
      非 Plain 编码命中时又编码一次；改为用无分配的长度计算得到 `uncompressed_length`。
- [ ] 合并编码选择采样、statistics、sort-key 和实际编码可复用的数据遍历，避免同一列重复
      `GetScalar()`、序列化和比较。
- [x] 为整数、timestamp、bool、string/binary 增加 typed encoder 路径，避免逐值构造
      `arrow::Scalar` 和调用 `SerializeScalar()`。
- [x] 为 `ByteWriter` 增加可计算的容量预留和批量 append，减少 vector 扩容及中间 payload 拷贝。
- [ ] 评估直接编码到最终 chunk buffer，并在一次顺序遍历中计算 chunk CRC32C；不得改变 CRC
      算法或落盘字节。

### 4.2 Reader 与 codec

- [x] Dictionary decode 直接向 typed Arrow builder/buffer 写入，不构造
      `vector<shared_ptr<arrow::Scalar>>`。
- [x] RLE decode 使用 run-aware 批量 append；selection 路径按 run 跳过未命中范围，不展开所有行。
- [x] FOR decode 使用 typed base/delta 和按字/批量 bit unpack，移除每值 `ScalarFromBits()`。
- [x] Plain selected decode 按物理类型直接 append，移除每个命中行的 `ParseScalar()` 和
      `AppendScalar()` 动态分派。
- [x] `RowsToDecode()` 的全量路径改为顺序迭代视图，避免构造 `[0..row_count)` 临时 vector。
- [ ] 对每项 typed fast path 保留通用安全 fallback，并用 property tests 证明两条路径逐值一致。

## 5. P1：优化扫描执行

- [x] 在计划校验时一次性把 `field_id` 解析为列下标，避免 predicate、projection 和逐行判断中
      重复线性调用 `FindFieldIndex()`。
- [x] 为各基础类型实现 typed predicate kernel，直接读取 Arrow values/validity；避免逐行
      `GetScalar()`、虚调用和临时对象。
- [x] 在计划校验时为 predicate 绑定 typed evaluator，并按 predicate 顺序对齐已解码列指针；
      逐行判断不再执行类型 switch 或 `unordered_map` 查找。
- [x] 将多个 AND predicate 融合到同一次 selection 构建，优先执行成本低、选择性高的谓词。
- [x] 在计划校验时为 sort-key range 绑定 typed comparator，逐行范围判断不再调用 `GetScalar()`
      或构造临时 key vector。
- [ ] 比较 index vector、bitmap 和连续 range 表示在 1%/10%/50%/100% 选择率下的成本，使用
      确定性阈值选择表示。
- [x] projection 与 predicate 是同一列时，直接从已解码列构造输出，提供 typed take/filter 路径。
- [ ] 减少跨 Row Group 输出时的 `ConcatenateRecordBatches()` 拷贝；优先返回合法 slice，只有确实
      需要单个连续 batch 时才合并。
- [ ] 针对 `limit` 做早停，确保不解码超过最后命中行所需的 projection 数据。
- [ ] 增加指标验证：谓词列、投影列、selection、拼接各阶段的行数、字节数和耗时可观测。

## 6. P1：优化文件 I/O 与 checksum

- [x] Reader 生命周期内复用一个随机访问文件句柄。当前 `ReadRange()` 每次读取 chunk 都重新
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
- [x] 至少达到第 2 节中的 writer、scan、encode、decode 四个吞吐目标；
- [ ] benchmark 矩阵覆盖第 3 节规定的 Row Group、选择率、投影和缓存维度；
- [ ] 所有性能结论都有 profiler 证据和至少 7 次重复测量；
- [ ] 生成 `bench/BENCHMARK_V2.md`，同时记录绝对值、相对 v1 的变化、压缩比、峰值内存和原始
      运行参数；
- [ ] 所有新增格式语义均有 decision record，v0.2 Reader 可读取 v0.1 Segment；
- [ ] 更新 README 的性能状态，不把合成 benchmark 结果表述为通用生产性能。

## 11. 执行记录

### 2026-09-17：Arrow allocation 与峰值 RSS 指标

- performance benchmark 在计时窗口外读取 Arrow default memory pool 和进程 high-water RSS；每轮
  记录 Arrow 累计申请字节、allocation/reallocation 次数，并派生 bytes/input-row 与
  allocations/input-row。采样不会进入 manual benchmark 时间。
- `arrow_total_allocated_bytes` 和 `arrow_allocations` 是每轮前后差值，可在同一进程的多个 case 间
  比较；`arrow_pool_peak_bytes` 与 `process_peak_rss_bytes` 是进程生命周期高水位。峰值比较必须让
  每个目标 case 在独立进程中运行，不能直接比较同一进程内后续矩阵 case 的高水位。
- RSS 使用 macOS/Linux `getrusage()`，macOS 按字节、Linux 从 KiB 转为字节；其他平台保留字段并
  输出 0。JSON smoke 强制验证所有内存字段存在，并要求 Arrow 指标为正值。

独立 Release dry-run、10 万行、Row Group 4,096、50% 选择率、2 列投影的观测值：

| 指标 | 值 |
|---|---:|
| Arrow 累计申请字节 | 1,624,448 bytes |
| Arrow allocations/reallocations | 114 |
| Arrow bytes/input-row | 16.24448 |
| Arrow allocations/input-row | 0.00114 |
| Arrow pool 进程峰值 | 3,086,272 bytes |
| 进程峰值 RSS | 10,567,680 bytes |

这些数字用于验证指标管线，不是完整矩阵的内存结论；最终 `BENCHMARK_V2.md` 需要对选定 case 分别
启动进程并重复采样。

### 2026-09-17：Row Group、选择率与投影矩阵

- performance benchmark 新增独立 `PerformanceMatrix/Sniffer`，覆盖 Row Group 1,024/8,192/
  65,536/262,144，选择率 1%/10%/50%/100%，以及 1/2/3 列投影，共 48 个组合；既有 Sniffer、
  三 predicate、sort-key range、Arrow IPC 和 Arrow IPC ZSTD case 名称保持不变。
- 每个矩阵 case 在计时循环内验证输出行数，并将选择率、投影列数、Row Group 数、剪枝数、chunk
  读取数/字节和各阶段耗时写入 Google Benchmark JSON；参数名称直接持久化在 benchmark 名称中。
- Release dry-run 对 48 个组合逐项执行，输出行数严格匹配 1,000/10,000/50,000/100,000，未出现
  skipped case。dry-run 只用于正确性验证，不作为性能结论；最终报告仍需按至少 7 次重复测量运行。

该矩阵完成第 3 节的 Row Group、选择率和窄表投影维度；宽表、超过页缓存的数据集、warm/cold cache
和峰值内存仍是独立待办，不能据此勾选完整 benchmark 验收项。

### 2026-09-17：预绑定 typed sort-key comparator

- sort-key range 在计划校验阶段按每个 key 字段绑定 typed comparator；逐行 lexicographic 比较直接
  读取 Arrow array value，不再调用 `GetScalar()`、分配 `Scalar` 或构造临时 key vector。lower/upper
  inclusive 语义、复合排序键顺序、Row Group 剪枝与落盘格式均未改变。
- Google Benchmark 新增独立 `SnifferSortKeyRange` case：10 万有序行、Row Group 4,096、范围
  `[25,000, 75,000)`、50% 选择率和 `id,value` 投影，避免把该热点混入普通 predicate case。
- GTest 新增全部 14 种首期平铺类型的范围结果与通用 Scalar reference 对照；既有复合排序键测试继续
  验证 lexicographic 半开区间。Release 与 ASan+UBSan 均为 46/46 通过。

同机 Release、21 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| sort-range 行级比较阶段 | 3.1375 ms | 0.2796 ms | -91.1% |
| scan | 4.9702 ms | 2.1065 ms | -57.6% |
| 端到端 | 19.5778 ms | 16.8039 ms | -14.2% |

两侧均考虑 25 个 Row Group、剪枝 12 个、读取 26 个 ColumnChunk 和 180,752 字节；输出均为
50,000 行。结果属于 warm-cache 合成场景，最终 v0.2 结论仍以完整选择率、Row Group 和投影矩阵为准。

### 2026-09-17：确定性 AND predicate 执行顺序

- 多 predicate 计划在校验阶段生成独立执行序：`EQ/IS NULL` 优先，其次范围比较、`NE`、
  `IS NOT NULL`；同一选择性等级中 fixed-width 先于 string/binary，完全相同时保留用户原始顺序。
  该规则只改变 AND 短路顺序，不改变公共 `IOPlan`、输出行序、null/NaN 语义或落盘格式。
- 0/1 predicate 保留原直接循环 fast path，多 predicate 才读取重排数组。曾尝试增加逐值求值计数，
  同机 A/B 发现它会拖慢单 predicate 热路径，因此在最终实现中移除，没有为可观测性接受性能回退。
- GTest 使用低选择性 predicate 在前的输入计划，验证混合 int64/string/nullable 字段仍输出同一结果，
  并重复执行确认计划结果确定；全类型、全部比较操作、null、NaN、Bloom 与 sort-key 测试继续通过。

同一时段分别构建父提交 `03e40ac` 与当前工作树，Release、10 万行、Row Group 4,096、21 次 P50：

| 场景 | 指标 | Before | After | 变化 |
|---|---|---:|---:|---:|
| 三 predicate | predicate 阶段 | 0.4223 ms | 0.3476 ms | -17.7% |
| 三 predicate | scan | 2.7888 ms | 2.6523 ms | -4.9% |
| 单 predicate | predicate 阶段 | 0.1786 ms | 0.1822 ms | +2.0% |
| 单 predicate | scan | 1.9279 ms | 1.9701 ms | +2.2% |

单 predicate 差异处于当轮系统波动范围，不作为稳定回退；三 predicate 的 Row Group 剪枝数、
ColumnChunk 读取数和读取字节保持不变。启发式不是数据分布统计模型，后续 cost model 仍需在完整
选择率矩阵中校准。

### 2026-09-17：预绑定 typed predicate evaluator

- 计划校验为每个比较谓词按 Arrow 类型绑定 evaluator；null predicate 使用独立通用 evaluator。
  Row Group 解码后按 predicate 顺序建立非 owning 列指针视图，逐行 AND 短路不再查哈希表或进行
  Arrow 类型 switch。
- Google Benchmark 新增 `SnifferThreePredicates`：10 万行、`id >= 50000 AND group = group-0 AND
  value IS NOT NULL`，输出 1,470 行；原单 predicate 和 Arrow IPC 对照保持独立 case。
- GTest 增加同字段重复 predicate、int64/string/nullable int32 混合 evaluator 和 null 比较语义覆盖；
  既有全部基础类型、NaN、null、sort-key 与 Bloom 测试继续覆盖语义非回归。

同机 Release、Row Group 4,096、21 次 P50：

| 场景 | 指标 | Before | After | 变化 |
|---|---|---:|---:|---:|
| 单 predicate | predicate 阶段 | 0.2168 ms | 0.1773 ms | -18.2% |
| 单 predicate | scan | 1.9213 ms | 1.9101 ms | -0.6% |
| 三 predicate | predicate 阶段 | 0.5141 ms | 0.4552 ms | -11.5% |
| 三 predicate | scan | 2.9876 ms | 2.7823 ms | -6.9% |

两种场景的 Row Group 剪枝数、ColumnChunk 读取数和读取字节均保持不变。三 predicate before 样本
受系统噪声影响较大，因此阶段耗时与 scan 提升只作为当前方向证据；最终数字仍以完整隔离矩阵为准。

### 2026-09-17：IOPlan field index 预解析

- `Scan()` 在校验计划时一次性把 projection、predicate 和 sort-key 的稳定 `field_id` 解析为列下标，
  `ScanState` 随后只使用已验证的下标；逐行 predicate、Row Group 解码、投影和 Bloom 剪枝不再线性
  扫描 schema。
- 新增 GTest 覆盖同一字段的多个 AND predicate、非 schema 顺序 projection，并复用既有 sort-key、
  Bloom、空 projection、未知字段和类型校验测试。文件格式和公共 `IOPlan` API 均未改变。

同机 Release、10 万行、Row Group 4,096、单 predicate、11 次 before 与 21 次 after 的 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| predicate 阶段 | 0.2666 ms | 0.2168 ms | -18.7% |
| scan | 1.9755 ms | 1.9213 ms | -2.7% |
| Row Group / chunk 观测 | 25 / 26 / 172.228k bytes | 25 / 26 / 172.228k bytes | 不变 |

当前标准场景只有 3 列和 1 个 predicate，因此端到端收益较小；宽表和多 predicate 场景预计更敏感，
但仍需在第 3 节完整矩阵中验证，不能从该合成场景外推生产收益。

### 2026-09-16：RLE typed/run-aware decode

- RLE decoder 先完整校验 run header、count、reserved bytes、bool 规范值、null value bytes 和总行数，
  再直接写入对应的 Arrow typed builder；不再为每个 run 和输出行构造 `arrow::Scalar`。
- 全量路径按 run 追加值或批量追加 null；selection 路径利用严格递增的 row id 单向跳过未命中 run，
  只 materialize 命中行。RLE v1 payload、checksum 和错误语义保持不变。
- GTest 覆盖 bool 和全部 8 种整数类型的全量/selected decode、null、负数与极值，并继续通过强制
  RLE round-trip、scan 和随机 property 测试。codec benchmark 新增 10 万输入行、1% selection 场景。

同机 Release、10 万个 int64、128 个近似等长 run、7 次 P50：

| 场景 | Before | After | 变化 | payload 字节 |
|---|---:|---:|---:|---:|
| RLE 全量 decode | 1.031 ms / 739.8 MiB/s | 0.210 ms / 3,554.8 MiB/s | 约 4.9x | 3,112 -> 3,112 |
| RLE 1% selected decode | 未单独测量 | 0.00534 ms / 1,000 输出行 | 新增基线 | 3,112 |

selected 场景仍会解析并验证全部 run metadata，因此 5.34 us 只表示该合成 payload 的内存内解码
延迟，不代表文件 I/O 吞吐。最终结论仍需纳入 Row Group、选择率和 cold/warm cache 完整矩阵。

### 2026-09-16：Dictionary typed decode

- Dictionary decoder 不再为 dictionary entries 和输出行构造 `arrow::Scalar` shared pointer；整数
  直接 little-endian 读取到 typed builder，string/binary 直接按已验证 offset append。
- 完整读取与 selection decode 共用 typed 路径，同时仍完整校验 validity、全部 index、offset、
  null 行 canonical index 和 payload 边界。
- GTest reference 覆盖全部 8 种整数、string、binary，以及 null、极值、空值和 `{0,2,4}` selected
  decode；编码 payload 字节保持不变。
- Release、10 万行、32 值字符串 dictionary 的短时 microbenchmark：decode 中位数从
  `1.948 ms / 556.1 MiB/s`（3 次）降至 `1.108 ms / 977.8 MiB/s`（7 次），约 `1.76x`。两次
  payload 均为 `113,058` 字节；这是定向 microbenchmark，不替代最终 v0.2 完整矩阵。

### 2026-09-16：GoogleTest 与 Google Benchmark 迁移

- 核心测试直接使用 GoogleTest `TEST` 与 `EXPECT_*` 宏，并通过 `gtest_discover_tests()` 映射到
  CTest；原有 37 个测试场景保持不变，仅保留 Arrow `Status/Result<T>` 失败适配辅助。
- 性能、压缩和 codec 三个程序使用 `BENCHMARK_CAPTURE` 声明固定 case，由 Google Benchmark
  管理迭代、重复测量、聚合、过滤和 JSON 输出；删除动态注册层、自建样本统计与 JSON writer。
- 保留 Sniffer/Arrow IPC/Arrow IPC + ZSTD 对照、压缩比和内部阶段指标，并以 benchmark counters
  输出。codec 的 Encode 与 Decode 拆分为独立 case，便于单独过滤和 profile。
- CTest smoke 使用 `--benchmark_dry_run`，并校验标准 Google Benchmark JSON 的 `context`、
  `benchmarks` 和构建复现元数据。

### 2026-09-16：测量输出与 writer 重复编码

- 两个 benchmark 已保留每轮原始耗时，并输出 min、P50、P95 和变异系数；旧的 `*_ms` 字段
  继续表示 P50。
- 增加 `--output-format=json`，记录源码 revision/dirty 状态、编译器、Arrow 版本、构建模式、
  硬件线程、完整参数、测量结果和扫描指标。
- `WriteRowGroup()` 在选择非 Plain 编码后不再先生成完整 Plain payload；`uncompressed_length`
  改由带溢出检查的长度计算得到。

同机 Release before/after（性能 11 次、压缩 7 次，均取 P50）：

| 指标 | Before | After | 变化 | 文件字节变化 |
|---|---:|---:|---:|---:|
| 性能场景 Sniffer 写入 | 69.9727 ms / 1.429 M rows/s | 68.2313 ms / 1.466 M rows/s | 约 +2.6% | 0 |
| 压缩套件 Sniffer 合计编码 | 89.4024 ms / 76.604 MiB/s | 87.1047 ms / 78.625 MiB/s | 约 +2.6% | 0 |

性能场景 before/after 的 CV 分别为 1.32% 和 4.77%，压缩套件分别为 3.40% 和 3.74%。当前提升
接近运行波动，不能单独视为稳定性能结论；该改动的确定收益是消除非 Plain 路径的一份完整
payload 分配和写入，同时保持文件大小与格式语义不变。下一步应通过 codec microbenchmark 和
分阶段 profile 继续定位 `GetScalar()`、`SerializeScalar()`、索引构建与实际编码的占比。

### 2026-09-16：纯内存 codec benchmark

- 新增 `sniffer_core_codec_benchmark`，直接调用生产 Plain、Dictionary、RLE、FOR + Bitpack
  encode/decode 路径，不包含文件 I/O、索引和 checksum。
- 每轮均验证完整 Arrow round-trip，验证位于计时区间外；文本和 JSON 都输出 payload 大小、
  压缩比、吞吐、原始样本、P50/P95/min/CV 与构建元数据。
- Plain 的生产 encode/decode 入口已提升为 `internal` 共享入口，writer/reader 和 benchmark 使用
  同一实现，没有复制 benchmark 专用 codec。
- 新增文本与 JSON CTest smoke。下一步用 Release 大样本结果确认各 codec 的热点优先级，并增加
  writer/reader 分阶段计时。

Release、10 万值、11 次运行的第一轮 profile 随后驱动了三个 typed fast path：

| 路径 | 优化前 P50 | 优化后 P50 | 吞吐变化 | payload 字节 |
|---|---:|---:|---:|---:|
| Dictionary string encode | 11.3188 ms | 1.1085 ms | 95.7 -> 977.4 MiB/s | 113,058 -> 113,058 |
| RLE int64 encode | 13.5230 ms | 0.2273 ms | 56.4 -> 3,356.7 MiB/s | 3,112 -> 3,112 |
| FOR + Bitpack int64 decode | 11.5800 ms | 0.4182 ms | 66.9 -> 1,852.8 MiB/s | 100,040 -> 100,040 |

Dictionary 和 RLE encoder 现在直接读取 Arrow typed values，避免逐值 `GetScalar()`、
`SerializeScalar()` 和临时 value vector；FOR decoder 直接写入 typed Arrow builder，避免每行
构造 Scalar。新增 reference tests 将 Dictionary/RLE typed encoder 与原 scalar 算法逐字节比较，
覆盖全部支持的整数宽度、bool、string/binary、负数、null 和连续 run；现有随机 property tests
继续覆盖 encode/decode 与 selection 语义。极短 RLE 测量容易受计时分辨率影响，后续矩阵应增加
更大数据量，但优化前后的数量级差异已经明确。

相同优化已经穿透文件级路径，文件大小、剪枝和读取字节保持不变：

| 文件级指标 | v0.1 参考值 | 当前值 | 变化 |
|---|---:|---:|---:|
| 性能场景写入 | 72.9057 ms / 1.372 M rows/s | 54.2313 ms / 1.844 M rows/s | 吞吐约 +34% |
| 50% 选择率扫描 | 18.9773 ms / 5.269 M rows/s | 6.8883 ms / 14.517 M rows/s | 吞吐约 2.76x |
| 性能场景端到端 | 91.4441 ms / 1.094 M rows/s | 61.2957 ms / 1.631 M rows/s | 吞吐约 +49% |
| 压缩套件合计编码 | 86.9556 ms / 78.760 MiB/s | 62.1259 ms / 110.237 MiB/s | 吞吐约 +40% |
| 压缩套件合计解码 | 51.3035 ms / 133.492 MiB/s | 22.7979 ms / 300.404 MiB/s | 吞吐约 2.25x |

性能场景仍剪枝 6/13 Row Group、读取 14 个 ColumnChunk 和 184,036 字节，Segment 文件仍为
675,943 字节；压缩套件合计仍为 3,622,610 字节。

### 2026-09-16：typed 索引、选择器、FOR encode 与 CRC32C

- 单排序键校验直接比较 Arrow typed values，每个 batch 只为跨 batch 边界保留首尾 Scalar；
  复合排序键继续使用原通用路径。
- statistics 直接在 Arrow array 中跟踪 min/max 行，Bloom 直接散列 Arrow value bytes；双种子 Bloom
  哈希在一次 value 遍历中完成。新增测试证明 typed hash 与原 `SerializeScalar()` 哈希完全一致。
- encoding selector 改为 typed 采样；Dictionary distinct、RLE runs 与 FOR extrema 不再逐值创建和
  序列化 Scalar，选择阈值与 tie-break 规则不变。
- FOR encoder 改为 typed base/delta；与旧 Scalar reference 对全部整数宽度、timestamp、null、极值
  逐字节比较。微基准从 175.9 MiB/s 提升至 710.6 MiB/s，payload 保持 100,040 字节。
- Plain variable-width 无 null 路径批量复制连续 Arrow value buffer，`ByteWriter` 按可计算长度预留
  容量；切片 string 与 nullable binary 均有逐字节 reference test。
- CRC32C 从逐字节、逐位实现改为相同 Castagnoli 多项式的 256 项查表实现。测试中的独立逐位实现
  保持不变并验证所有既有 header/footer/chunk checksum；未关闭或减少任何 checksum。该项尚未建立
  独立 CRC microbenchmark，收益来自完整文件 benchmark 的 before/after，应在后续分阶段测量中补齐。

同机 Release 最终测量（性能 11 次、压缩 7 次，均取 P50）：

| 指标 | v0.1 | 本轮优化前 | 当前 | v0.2 目标 |
|---|---:|---:|---:|---:|
| 性能场景写入 | 1.372 M rows/s | 1.844 M rows/s | 6.957 M rows/s | >= 4.0 M rows/s |
| 50% 选择率扫描 | 5.269 M rows/s | 14.517 M rows/s | 16.111 M rows/s | >= 15.0 M rows/s |
| 性能场景端到端 | 1.094 M rows/s | 1.631 M rows/s | 4.852 M rows/s | >= 3.0 M rows/s |
| 压缩套件合计编码 | 78.760 MiB/s | 110.237 MiB/s | 270.085 MiB/s | >= 250 MiB/s |
| 压缩套件合计解码 | 133.492 MiB/s | 300.404 MiB/s | 433.734 MiB/s | >= 400 MiB/s |

当前性能场景 P50 为写入 14.3747 ms、扫描 6.2069 ms、端到端 20.6113 ms。仍剪枝 6/13 Row
Group、读取 14 个 ColumnChunk 和 184,036 字节，Segment 为 675,943 字节。压缩套件合计仍为
3,622,610 字节，所有场景压缩比不变。

高基数 string 的 0.874x 压缩比没有变化：v0.1 Plain 为每行持久化一个 64-bit offset，且每个 Row
Group 都有 chunk/index 元数据和 checksum；该场景本身不可字典化，因此 2,800,004 logical bytes
对应 3,202,118 file bytes。这是现有 v0.1 格式开销，不是本轮速度优化造成的回退；若要改善需按
第 7 节为 compact offset 分配新 encoding ID，不能静默改变 Plain v1 字节语义。

本轮验证：Debug、Release、ASan+UBSan 三套构建的 8/8 CTest 均通过；其中包含普通单元测试、
随机 property/fuzz smoke、三个 benchmark 文本 smoke 和三个 JSON schema smoke。`clang-format` 与
`git diff --check` 通过。v0.2 尚未完成：分阶段计时、RSS/allocation、完整 Row Group/选择率/宽表矩阵、
warm/cold cache、reader 文件句柄复用以及 `bench/BENCHMARK_V2.md` 仍按第 3、5、6、10 节继续推进。

### 2026-09-16：分阶段计时与 typed scan kernel

- `SegmentWriter::Open()` 和 `SegmentReader::Open()` 增加可选 metrics；不传 metrics 时计时器不读取
  时钟。`ScanMetrics` 增加扫描阶段耗时，原有剪枝、chunk 数和读取字节计数保持兼容。
- performance benchmark 对每个阶段记录 11 轮原始样本及 min/P50/P95/CV，JSON smoke 同时校验
  `phase_stats`。writer 覆盖校验、索引、编码选择、编码、checksum、文件写入、footer；reader 覆盖
  envelope/index、剪枝、chunk I/O/checksum、解码、谓词、投影及 batch materialization。
- 第一轮 profile 显示 writer P50 主要为编码 7.9038 ms、checksum 2.5296 ms、索引 2.2205 ms；
  reader 的谓词为 2.1866 ms、已解码列投影为 1.9985 ms，而 chunk I/O 仅 0.2033 ms。因此下一项
  选择 typed predicate/projection，而未优先改造文件句柄。
- predicate 现在对全部首期平铺类型直接读取 Arrow typed value；string/binary 使用无分配字节比较，
  float/double 保留 NaN 和 signed-zero 语义。谓词列同时参与 projection 时通过 typed builder 选择，
  不再逐行 `GetScalar()`/`AppendScalar()`。
- 新增全部 14 种首期类型的 equality、null projection reference test，并覆盖 NaN `!=` 语义和可选
  metrics 的 reset/累加行为。

同机 Release、10 万行、8,192 行 Row Group、11 次 P50：

| 指标 | typed scan 前 | typed scan 后 | 变化 |
|---|---:|---:|---:|
| 扫描总耗时 / 吞吐 | 6.1711 ms / 16.205 M rows/s | 2.3491 ms / 42.570 M rows/s | 吞吐约 2.63x |
| predicate 阶段 | 2.1866 ms | 0.2801 ms | 约 -87% |
| projection 阶段 | 1.9985 ms | 0.1171 ms | 约 -94% |
| 端到端吞吐 | 4.767 M rows/s | 5.956 M rows/s | 约 +25% |

文件仍为 675,943 字节，仍剪枝 6/13 Row Group、读取 14 个 ColumnChunk 和 184,036 字节。

### 2026-09-16：Reader 持久化随机访问文件句柄

- `SegmentReader::Open()` 只打开一次只读文件句柄；envelope、footer、索引、ColumnChunk、
  `ReadAll()` 和文件 checksum 验证都通过同一个带边界检查的 `ReadAt()`/顺序 checksum 接口读取。
- 文件句柄使用共享所有权，保证 scan iterator 可以安全地晚于 `SegmentReader` 销毁；底层 stream
  使用互斥保护 seek/read 状态，多个 iterator 不会竞争同一个文件位置。
- `ReaderMetrics::file_handles_opened` 提供可观测验证；测试覆盖 open、scan、ReadAll 和 checksum
  验证全过程仅打开一个句柄。格式字节、checksum 语义、剪枝数量和 chunk 读取量均未改变。

同机 Release、10 万行、8,192 行 Row Group、50% 选择率、11 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| 扫描总耗时 | 2.5044 ms | 2.0638 ms | -17.6% |
| 扫描吞吐 | 39.930 M rows/s | 48.454 M rows/s | +21.3% |
| envelope I/O | 0.1739 ms | 0.1305 ms | -24.9% |
| index I/O | 0.1912 ms | 0.0665 ms | -65.2% |
| chunk I/O | 0.2244 ms | 0.0607 ms | -73.0% |

扫描 CV 从 10.65% 降至 8.47%，P95 从 3.1982 ms 降至 2.6335 ms；结果仍属于 warm-cache
合成场景，后续完整矩阵需要把 cold-cache 和不同 Row Group 数量分开记录。Release 8/8 CTest、
fuzz smoke 和 benchmark JSON smoke 均通过。

### 2026-09-16：非 Plain 全量解码移除 row-id 临时数组

- Dictionary、RLE 和 FOR + Bitpack 全量解码不再构造 `[0..row_count)` 的 `uint64_t` vector；
  连续行与 selection 行使用分别实例化的顺序循环，selection 的严格递增和边界校验保持不变。
- 10 万行每次全量解码减少约 0.76 MiB row-id 临时分配。新增测试覆盖 Dictionary、RLE、FOR
  对重复、降序和越界 selection 的拒绝路径。
- 第一版通用 visitor 导致 FOR 明显回退，已在提交前弃用；最终实现保留紧凑 typed 循环。

同机 Release、10 万值、11 次 P50：

| 解码场景 | Before | After | 吞吐变化 |
|---|---:|---:|---:|
| Dictionary string | 2.0998 ms / 515.96 MiB/s | 1.9987 ms / 542.05 MiB/s | +5.1% |
| RLE int64 | 1.0138 ms / 752.56 MiB/s | 0.9974 ms / 764.92 MiB/s | +1.6% |
| FOR + Bitpack int64 | 0.3886 ms / 1,993.85 MiB/s | 0.3925 ms / 1,973.96 MiB/s | -1.0% |

FOR 的 1.0% 差异低于本轮运行波动，不作为稳定回退或提升结论；该小步的确定收益是消除与
Row Group 行数线性增长的临时内存，同时保持 payload、输出数组和错误语义不变。

### 2026-09-16：Plain selected typed decode

- Plain projection selection 按 physical type 直接读取 little-endian bytes，并写入对应 Arrow
  typed builder；不再为每个命中值调用 `ParseScalar()`、构造 Scalar 和动态 `AppendScalar()`。
- bool 保留 0/1 规范性检查，string/binary 保留完整 offset 单调性与边界校验，所有类型继续校验
  null bitmap、selection 严格递增、数组完整性及 Arrow 长度上限；Plain v1 payload 未改变。
- codec benchmark 新增 `plain_selected_int64_50pct` 场景，明确记录输入 10 万行、输出 5 万行；
  reference test 覆盖全部 14 种首期类型、null、整数极值、浮点 signed zero、空/UTF-8 字符串、
  binary 与重复 selection 拒绝路径。

同机 Release、10 万值、50% selection、21 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| Plain selected decode | 1.8018 ms | 0.1548 ms | -91.4% |
| 按输入 logical bytes 吞吐 | 430.0 MiB/s | 5,005.8 MiB/s | 约 11.6x |
| P95 | 2.1270 ms | 0.1736 ms | -91.8% |

该 microbenchmark 不包含文件 I/O、索引或 checksum；收益只归因于 selected decode kernel。
