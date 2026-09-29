# v0.2 性能优化专项 TODO

**状态：** In Progress

**目标版本：** v0.2

**基线：** [`bench/BENCHMARK_V1.md`](../bench/BENCHMARK_V1.md)

当前文件格式对照改为 Parquet；v1 Arrow IPC 数据保留作历史性能回归参考，新的对照口径见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

## 1. 目标与边界

v0.2 聚焦现有 Segment writer、reader、codec、scan 和文件 I/O 路径的性能，不扩大
`sniffer-core` 的业务边界。优化必须保持 Arrow 语义、确定性输出、边界检查、checksum、结构化
错误以及 v0.1 文件的可读性。

本专项不默认引入 RocksDB、对象存储、SQL、RPC、事务、默认启用的后台线程池或完整 nested type。
SIMD、多级编码链和新压缩算法只有在标量路径完成剖析和优化后才进入实现；任何新 encoding 都必须有
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
- [x] 使用 Instruments 或等价 profiler 保存 CPU flamegraph 摘要和 allocation hot spots；先证明
      热点，再修改实现。
- [x] 增加峰值 RSS、Arrow memory pool 峰值和每输入行分配次数指标。
- [x] 扩展固定矩阵：Row Group 1K/8K/64K/256K，选择率 1%/10%/50%/100%，单列/多列/全列投影。
- [x] 增加 16 列宽表的 Sniffer/Parquet 对照，报告 warm-cache 写入、扫描、文件大小和内存指标。
- [ ] 增加超过页缓存容量的数据集，并分别报告 warm-cache 与 cold-cache 结果。

## 4. P0：消除已知重复工作和逐值对象开销

以下项目由当前实现的静态检查得出，是待 profiler 验证的高概率热点。

### 4.1 Writer

- [x] 先执行编码选择，再生成最终 payload。当前 `WriteRowGroup()` 总是完整生成 Plain payload，
      非 Plain 编码命中时又编码一次；改为用无分配的长度计算得到 `uncompressed_length`。
- [ ] 合并编码选择采样、statistics、sort-key 和实际编码可复用的数据遍历，避免同一列重复
      `GetScalar()`、序列化和比较。
  - [x] 第一小步：FOR encoder 复用 statistics minimum，移除一次完整的 base 查找遍历；无统计列
        保留原回退路径，所有整数宽度、timestamp 和 all-null 均有逐字节等价测试。
  - [x] 第二小步：非排序整数、bool 和 timestamp statistics 在原 typed min/max 遍历中同时生成
        selector sample 摘要；排序键 endpoint 快速路径和无 statistics 字段不额外扫描。
  - [x] 第三小步：单列 sort-key 校验按 Arrow type 绑定循环，将 null/NaN 检查与局部有序检查融合
        为一次遍历；跨 batch 边界检查和结构化错误保持不变。
  - [x] 第四小步：全非空 string/binary 的 Plain 大小估算直接使用 Arrow 首尾 offset，移除逐行
        `value_length()` 求和；含 null 列保留逐行安全路径。
  - [x] 第五小步：选择器对采样区间仅计算一次 null bitmap 状态，Plain 样本大小与候选编码
        复用；有 bitmap 时直接按 Arrow bit offset 计数，不再创建临时 slice。覆盖切片、样本
        内外 null 的等价测试，文件字节不变。
- [x] Bloom 构建按列绑定数组类型和 physical type，逐行双哈希不再重复做类型一致性检查与
      `PhysicalTypeFor()`；安全入口和绑定入口的哈希结果逐值一致。
- [x] Bloom 构建按列确定是否可能含 NaN；非浮点列跳过逐行类型检查，float/double 仍按原规则
      排除 NaN，Bloom bit pattern 与 Scalar reference 一致。
- [x] 为整数、timestamp、bool、string/binary 增加 typed encoder 路径，避免逐值构造
      `arrow::Scalar` 和调用 `SerializeScalar()`。
- [x] 为 `ByteWriter` 增加可计算的容量预留和批量 append，减少 vector 扩容及中间 payload 拷贝。
- [ ] 评估直接编码到最终 chunk buffer，并在一次顺序遍历中计算 chunk CRC32C；不得改变 CRC
      算法或落盘字节。
  - [x] 全非空 string/binary 的 Plain payload 直接写入最终 buffer；offset 仍显式按
        little-endian 归一化，Arrow values 只复制一次。nullable 路径维持原实现，chunk
        CRC32C 的融合尚未做。切片、空值、binary 非文本字节与参考编码逐字节一致；
        高基数字符串完整写入没有可确认的速度收益，见 benchmark 记录。

### 4.2 Reader 与 codec

- [x] Dictionary decode 直接向 typed Arrow builder/buffer 写入，不构造
      `vector<shared_ptr<arrow::Scalar>>`。
- [x] RLE decode 使用 run-aware 批量 append；selection 路径按 run 跳过未命中范围，不展开所有行。
- [x] FOR decode 使用 typed base/delta 和按字/批量 bit unpack，移除每值 `ScalarFromBits()`。
- [x] Plain selected decode 按物理类型直接 append，移除每个命中行的 `ParseScalar()` 和
      `AppendScalar()` 动态分派。
- [x] `RowsToDecode()` 的全量路径改为顺序迭代视图，避免构造 `[0..row_count)` 临时 vector。
- [ ] 对每项 typed fast path 保留通用安全 fallback，并用 property tests 证明两条路径逐值一致。
  - [x] FOR 无 null、无 selection 的全量解码直接填充 Arrow 值缓冲区；nullable
        和 selected decode 保留 builder 路径。全类型极值、0–64 bit width、
        空列与坏 delta 测试通过；其他 codec 的 fallback 仍待逐项审查。

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
- [x] 无谓词且无 sort-key range 的全量扫描直接构造连续 selection，跳过逐行空匹配调用；
      `limit` 仍只选择所需行数。
  - [x] 整个 Row Group 均命中且未被 `limit` 截断时，不再分配连续 selection，投影列走
        已有全量解码路径；部分 Row Group 保留 selected-decode，batch 大小与早停语义不变。
- [ ] 比较 index vector、bitmap 和连续 range 表示在 1%/10%/50%/100% 选择率下的成本，使用
      确定性阈值选择表示。
  - [x] 对全命中的谓词 Row Group 使用隐式连续范围，跳过 identity selection；部分命中
        保持 index vector，`limit` 截断的前缀仍显式生成所需行号。bitmap 与通用连续 range
        表示尚未比较。
  - [x] 独立 Google Benchmark 比较 index vector、bitmap、range 在 1%/10%/50%/100%
        选择率、均匀散点和连续前缀数据上的构建＋消费成本与容器容量；结果见
        [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。生产 scan 尚只传递
        index vector，必须完成完整 codec/scan A/B 后才能选择阈值。
  - [x] Plain selected-decode 增加内部 bitmap 候选和 1%/10%/50%/100% 的
        nullable int64 A/B；各 Plain 类型、null、跨 word、非法 bitmap 逐值测试。
        生产 scan 尚未切换；非 Plain codec 的 bitmap 接入尚未评估。
  - [x] 增加真实 SegmentWriter/Reader 的 Plain 投影完整扫描基准，覆盖均匀散点和
        连续前缀的 1%/10%/50%/100%，核对行数、null、值和 ColumnChunk 读取数。
        与临时自适应 bitmap 接入做交错 A/B：10%/50% 均匀场景无稳定速度收益，
        50% 有约 2%–3% 回退，故撤回生产切换；保留 codec 候选与可重复运行的
        index 基准，详见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
- [x] projection 与 predicate 是同一列时，直接从已解码列构造输出，提供 typed take/filter 路径。
  - [x] 当同一谓词列的命中行号恰好连续时，直接返回已校验 Arrow 数组的 `Slice()`，
        保留 validity 与值缓冲区；非连续选择仍走 typed take/filter。
- [ ] 减少跨 Row Group 输出时的 `ConcatenateRecordBatches()` 拷贝；优先返回合法 slice，只有确实
      需要单个连续 batch 时才合并。
  - [x] 已验证对齐输出整块复用 `RecordBatch` 的候选：虽然可将该场景 25 次 slice
        降为零，但完整扫描复测 1.017 → 1.037/1.051 ms，无速度收益，已撤回。
        多列跨组输出若保持当前 `output_batch_rows` 的 batch 边界，仍需生成连续 Arrow
        数组；不改变公开分批语义来规避拷贝。
- [x] 将谓词/排序键解码列的逐组哈希表改为计划期确定的紧凑槽位，重复谓词、排序键和
      投影共享同一份解码结果；不扩大无过滤扫描的逐组临时容器。1K Row Group 和
      三谓词扫描的 Release A/B、完整测试结果见 benchmark 记录。
- [x] 针对 `limit` 做早停：selection 达到剩余 limit 后停止逐行匹配，只为这些命中行解码
      projection，并且不读取后续 Row Group；已有谓词测试和无谓词 `limit=0/1/7/30` 测试覆盖。
- [ ] 增加指标验证：谓词列、投影列、selection、拼接各阶段的行数、字节数和耗时可观测。
  - [x] 第一小步：记录谓词/投影列块读取字节、解码/输出行数、谓词检查行数、显式 selection
        行号数、输出 slice/拼接次数与拼接耗时；benchmark 同时报告行号逻辑字节数。
        尚缺各阶段实际分配字节及 bitmap/range 成本。

## 6. P1：优化文件 I/O 与 checksum

- [x] Reader 生命周期内复用一个随机访问文件句柄。当前 `ReadRange()` 每次读取 chunk 都重新
      打开文件并 seek，应改为有明确所有权的 `RandomAccessFile`/等价 RAII 抽象。
- [ ] 合并相邻或距离很近的候选 chunk 读取，减少系统调用，同时保证未命中列块不会被读取。
- [ ] 评估 Arrow buffer、pread 和 mmap 三种 backend；默认实现必须可移植，mmap 只能作为可选
      backend，且所有 offset/length 仍先做溢出与边界检查。
- [ ] 避免文件 checksum 验证和后续 chunk 读取造成不必要的重复拷贝；允许缓存“已验证”状态，
      但每个新 Reader 仍必须按格式契约完成验证。
- [x] 使用批量或硬件加速 CRC32C 前先建立独立 benchmark；实现必须与当前 CRC32C 字节结果一致。
- [ ] Writer 增大顺序写缓冲、减少小 write；`Finish()` 的 durability 语义不得被悄然改变。

## 7. P2：编码选择与空间效率

- [ ] 让选择器同时估算最终 payload 大小、编码 CPU 成本和预期解码成本，而不是仅按样本大小阈值
      决策；模型必须确定、可解释、可测试。
  - [x] 对变长列字典候选用已见不同值计算严格大小下界，确定无胜出可能时提前结束采样；
        保留原选择阈值和相同的最终 encoding ID。
- [x] 对所有自适应非 Plain 编码加入实际 payload 不小于 Plain 则回退的保护；显式强制编码
      不受影响，复现采样偏斜时 RLE 膨胀并验证确定性、round-trip 和格式 ID。
- [ ] 记录编码选择/回退原因供 benchmark 观测，并评估超过“恰好不比 Plain 大”的收益阈值。
  - [x] 第一小步：writer 可选 metrics 按 chunk 记录直接 Plain、保留非 Plain、实际大小回退、
        显式强制编码，以及回退时被弃 payload 与 Plain payload 的字节数；压缩 benchmark 输出
        对应计数。不改变编码策略，CPU/解码成本与更高收益阈值仍待评估。
- [x] 单独分析高基数 string/binary 的 64-bit offset 开销。如果引入 32-bit compact offset 或
      CompactPlain，必须分配新 encoding ID，旧 Reader 对未知 ID 明确失败，v0.2 Reader 保持读取
      v0.1 Plain 的能力。
- [ ] 评估 bool bitpack、delta-of-delta timestamp 和 dictionary index + RLE；只有现有 P0/P1
      优化完成、microbenchmark 证明收益后再进入格式设计。
- [x] 新 encoding 必须补齐随机、极值、null、截断、错误 offset、checksum 和 selection decode
      测试，并更新 format decision record。

## 8. P2：并发能力；P3：可选向量化

**执行顺序：** 先完成单线程优化及其基准，再启动本节的并发工作；列入 v0.2 TODO 不代表当前开始实现。

当前公开 API 未定义线程安全契约，benchmark 也仅覆盖单线程。Reader 内部文件句柄的互斥锁
只保护 seek/read 操作，不能单独作为“支持并发”的验收依据。并发读与内部并行执行是不同能力，
均列入 v0.2 工作范围；默认单线程行为和无后台线程的嵌入方式必须保留。

### 8.1 并发访问契约与并发读取（P2）

- [ ] 明确并测试对象级线程安全边界：多个 Reader 读同一不可变 Segment、同一 Reader 创建的多个
      独立 Scan iterator 并发读取应正确；同一 iterator 的并发 `Next()`、同一 Writer 的并发
      `Append()`/`Finish()` 若不支持，应在 API 文档中明确禁止，不能含糊承诺。
  - [x] 已在公开 API 和 README 明确独立 iterator/Reader 可并发、单个 iterator 和 Writer
        不可并发；四种 IOPlan 的同一 Reader 并发测试与独立 Reader 基准已覆盖基本边界。
- [ ] 审核共享文件句柄、footer/index、Arrow 内存池及可选 metrics 的所有权和同步。避免把所有
      并发 scan 串行化在一个 seek/read 互斥锁上；评估每 scan 独立句柄或有界 `pread` 路径，
      同时保留 offset 检查、checksum、索引剪枝与“只读候选列块”契约。
  - [x] macOS/Linux 共享只读 fd 改为 `pread`，其他平台保留互斥流回退；ScanState 自有
        footer/index 副本和独立 metrics，同一 Reader 多扫描与损坏块剪枝测试已覆盖。
- [ ] 补多线程正确性与压力测试：不同 IOPlan、projection、limit、空结果和损坏文件同时扫描；
      与单线程结果逐字段比较，并在可用平台运行 ThreadSanitizer。明确 Reader/iterator 销毁、
      错误传播与取消时的生命周期语义。
  - [x] 4 线程 × 40 轮不同计划、损坏块与剪枝隔离、Reader 销毁后继续消费 iterator、
        同时 `ReadAll()`/checksum；ThreadSanitizer 全量单测 66/66 通过。
        内部并行任务的取消/预算语义仍待设计。

### 8.2 受控的内部并行（P2）

- [ ] 评估跨 ColumnChunk/Row Group 的可选并行编码：并行任务只生成独立结果，最终按原始列和
      Row Group 顺序写入；同一输入及配置的 Segment 字节、checksum 和索引必须与单线程一致。
- [ ] 评估候选 Row Group 的可选并行读取、解码与过滤；输出行序、`limit` 早停、谓词列/投影列
      分离解码和剪枝结果必须与单线程一致，不得为未命中列块发起额外读取。
  - [x] 先以同一 Reader 的独立排序范围 Scan 做 1/2/4/8 worker 可行性 A/B：10M 行
        在整组 SortKey 证明优化后，单 worker 9.518 ms、8 worker 2.998 ms；100K 行则
        多 worker 均慢于普通单线程。此实验不是正式内部并行，不能勾选父项。
  - [x] 新的 `Scan(IOPlan, ScanExecutionOptions, metrics)` 已按物理 Row Group 实现
        opt-in、有界在途、保序输出；排序/无索引、nullable/binary、空结果、过滤与投影
        分离、候选/剪枝损坏块、提前销毁及 `limit` 串行回退有回归。父项仍待
        更宽数据分布和 Parquet/cold-cache 验收。
- [ ] 线程数、任务粒度、在途 Row Group 数和内存预算由显式配置限制；提供单线程 fallback、
      错误/取消传播，不创建无上限异步任务。若需要新公共配置，先确定向后兼容的 API 语义。
  - [x] [`Decision 0005`](decisions/0005-bounded-parallel-scan.md) 已固定 opt-in 执行选项、
        Row Group 调度、预算、保序、`limit` 串行回退及销毁/错误契约；现有串行路径补了
        “提前丢弃迭代器或命中 limit 不读后续损坏块”基线测试。调度器与 API
        现已实现，默认旧接口仍为串行；预算为调度估计，不是 RSS 保证。
  - [x] 预算估算改为仅计谓词/排序键/投影列并集，不因无关宽列退回串行；
        1 MiB 预算下的 nullable/binary 回归与 100K/1M Reader-only 基准已记录。
        worker 在该窄投影场景可启动但比串行慢，故保持显式 opt-in。
- [ ] 增加 1/2/4/8 线程吞吐、单请求延迟、峰值 RSS 和分配量 benchmark；并发多查询与单查询
      内部并行分别报告，在相同硬件、数据、Row Group、选择率与线程预算下对照 Parquet。
  - [x] 多查询 1/2/4/8 线程的 Sniffer 共享/独立 Reader、Parquet 无 codec 压缩/ZSTD
        对照已测；逐值验证 Parquet 结果。单请求内部并行的 100K/10M 同计划
        Reader-only P50/P95、RSS、Arrow pool 峰值和读取量已测，见 benchmark 记录；
        同条件 Parquet 未压缩/ZSTD 单请求已测。另补含 null 的高熵 binary
        窄投影同计划对照；另补 15 列相关递增整数的宽投影对照。
        cold-cache、高熵/变长宽投影及完整选择率矩阵仍未测。

### 8.3 可选向量化（P3）

- [ ] 在标量 typed kernel 稳定后，为 bit unpack、predicate 和 bitmap 操作评估 NEON/AVX2；提供
      编译期/运行时能力检测和完全等价的标量 fallback。
- [ ] SIMD 实现不得使用不安全 reinterpret-cast 解析外部输入；先验证 buffer 边界和规范性。

## 9. 推荐实施顺序

1. 完成分阶段计时、allocation 指标和 codec microbenchmark，产出第一份 profile。
2. 优化 writer 的“双重编码”和 scalar 分配，逐项跑压缩比与 encode benchmark。
3. 优化 Dictionary/RLE/FOR/Plain typed decode，逐项跑 round-trip、fuzz 和 decode benchmark。
4. 复用 reader 文件句柄并优化 chunk 读取，再测 warm/cold cache。
5. 优化 typed predicate、selection 和 batch 拼接，再跑完整 IOPlan 矩阵。
6. 修正编码 cost model 和高基数字符串空间开销。
7. 只有标量路径仍是 CPU 热点时才评估 SIMD。
8. 最后建立并发访问契约和多查询并发读基线；内部并行编码/扫描按 CPU profile 与内存预算逐步
   实现，不提前打断单线程专项。

每个性能提交必须只处理一个可归因热点，并附同机 before/after 原始指标。若性能提升伴随空间、
内存或复杂度回退，提交说明和 v0.2 benchmark 报告必须显式列出取舍。

## 10. v0.2 完成定义

- [ ] 第 2 节正确性和非回归门槛全部满足；
- [x] 至少达到第 2 节中的 writer、scan、encode、decode 四个吞吐目标；
- [ ] benchmark 矩阵覆盖第 3 节规定的 Row Group、选择率、投影和缓存维度；
- [ ] 第 8 节的并发读取契约、多线程正确性/竞态测试和 1/2/4/8 线程基准完成；内部并行路径
      如因实测收益不足而未启用，需记录数据、取舍和单线程 fallback；
- [ ] 所有性能结论都有 profiler 证据和至少 7 次重复测量；
- [ ] 生成 `bench/BENCHMARK_V2.md`，同时记录绝对值、相对 v1 的变化、压缩比、峰值内存和原始
      运行参数；
- [x] 所有新增格式语义均有 decision record，v0.2 Reader 可读取 v0.1 Segment；
- [x] 更新 README 的性能状态，不把合成 benchmark 结果表述为通用生产性能。

## 11. 执行记录

### 2026-09-29：15 列宽投影同计划对照

新增 `--projected-columns=15`：生成一列递增 key 与 15 列相关递增 int64，
Sniffer 和 Parquet 未压缩/ZSTD 使用相同 Row Group、50% 选择率和全部 15 列
投影；计时前对串行、4-worker 和两种 Parquet 路径逐列逐值验证。
Apple M4、Release、warm-cache，1M 行独立进程 20 次重复的 P50 为
Sniffer 串行 14.815 ms、4-worker 4.689 ms、Parquet 未压缩 20.574 ms、
ZSTD 69.397 ms。文件分别为 31.83、154.46、42.60 MB；4-worker
RSS 高水位约 20.20 MB，串行约 12.57 MB。P95、读取量口径、内存、
100K 行场景及复现命令见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
这只验证了有规律整数宽投影；高熵/变长宽列与 cold-cache 仍未完成。

### 2026-09-29：nullable binary 投影的同计划对照

Reader-only 生成器新增 128 B/值的确定性高熵 binary 与每 17 行一个 null；
Sniffer 和 Parquet 未压缩/ZSTD 使用相同值、Row Group、50% 选择率和
仅投影 value 的计划。计时前对每个实际执行路径逐值校验 null、字节和行序。
Apple M4、Release、warm-cache，独立进程 20 次重复的 1M 行 P50：
Sniffer 串行 16.476 ms、4-worker 6.428 ms、Parquet 未压缩 13.926 ms、
ZSTD 15.583 ms。文件大小依次为 130.3、135.7、128.4 MB；4-worker
RSS 高水位约 41.1 MB，串行约 11.9 MB。具体 P95、读取量、Arrow pool、
100K 场景与复现参数见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
这补齐一个真实变长投影场景，但不能据此勾选宽投影、其他分布或 cold-cache。

### 2026-09-29：并行预算与无关变长列

发现并行调度对所有列的 `uncompressed_length` 做估算，即使查询只用
key/value，无关大 binary 列也会强迫串行 fallback。先用回归测试复现
`parallel_workers_started=0`，再改为只估算谓词、排序键、投影列并集；
被投影的大 binary 列在预算不足时仍退回串行。格式和查询结果不变。
独立进程 Release 基准在 1 MiB 预算、每行额外 256 B 无关 binary、
8,192 行 Row Group 下，100K 行串行/4-worker P50 为 0.145/0.194 ms，
1M 行为 0.985/1.135 ms（各 11 次）。调度可用但本场景并未提速，
保留默认串行。数据、内存与复现命令见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)；这不关闭宽投影、
变长列解码或 cold-cache 验收项。

### 2026-09-29：单请求有界并行扫描首版

新增 `ScanExecutionOptions`：默认旧接口仍串行；显式 worker 数大于 1、无
`limit` 且 Row Group 估算可放入预算时，首次 `Next()` 才启动 worker。按物理
Row Group 有界调度、保序取回并沿用原 batch 拼接；`limit`、单组和预算过小
走串行。每个 worker 的 `ScanMetrics` 独立，主线程汇总；提前丢弃或错误后
停止分派并 join。完成但未输出的预取工作仍计入实际 I/O 指标。新增 in-flight
组数与预留字节峰值指标；预算是保守调度估计，非进程 RSS 硬上限。

Release 与 ASan+UBSan 全量 94/94 通过。Apple Arrow 23 静态库默认 mimalloc
未用 TSan 插桩，默认分配池下出现地址回收的 TSan 报警；TSan 构建的 CTest
改用 Arrow system memory pool，定向用例连续 20 次及全量 76/76 通过。
详细 100K/10M、1/2/4/8 worker 数据见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。该路径仍是显式 opt-in，
Parquet 同计划单请求见下方新增记录；cold-cache 等维度尚未验收，v0.2 未收尾完成。

### 2026-09-29：同计划 Parquet Reader-only 对照

在双 int64、Row Group 8,192、`key >= rows/2`、仅投影 value 的 warm-cache
场景，三个格式由同一生成器写出；Sniffer/Parquet 均逐值校验查询输出。
10M 行、独立进程 20 次测量的 P50/P95：Sniffer 串行 9.506/9.636 ms，
4-worker 6.786/7.171 ms；Parquet 未压缩 18.911/19.339 ms，ZSTD
50.952/51.810 ms。对应文件分别为 32.9、193.1、53.0 MB；具体 RSS、
Arrow pool 和读取量口径见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
这只关闭同计划单请求 warm-cache 对照缺口；Parquet 的列字节数来自元数据，
不是实际物理读取量。cold-cache、宽/变长列、选择率矩阵及资源预算仍待验收。

### 2026-09-28：按物理 Row Group 的处理单元

在 Decision 0005 和早停基线提交 `d0d6d6a` 后，将内部单组读取/剪枝/过滤/
投影抽为 `ScanState::ReadRowGroupAt(index, remaining_limit)`；原串行遍历只负责
选择下一组，公开 API 与文件字节不变。100K/10M Reader-only before/after
见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)：10M 无可信小幅差异，
100K 本轮约慢 3%，故不将重构称作性能提升；后续必须以有界调度收益覆盖
该成本。Row Group 处理单元仍只由串行路径调用，正式内部并行尚未接入。
Release 与 ASan+UBSan 跨构建并发运行 CTest 时，benchmark smoke 曾因固定临时
文件名互相覆盖而失败；单个 CTest 的资源锁不能覆盖不同构建目录。性能与压缩
基准现使用含进程 ID 和时戳的独立临时路径，不改变库扫描行为。

### 2026-09-28：有界内部并行的执行契约与早停基线

提交 `cc8e55a` 已同步远端，开始本项时工作树无待提交改动。上一轮 10M 行
实验性排序范围分片有收益，但生产 `Scan` 仍是单线程迭代器。先在
[`Decision 0005`](decisions/0005-bounded-parallel-scan.md) 明确：并行配置与
逻辑 `IOPlan` 分离、默认单线程、任务按物理 Row Group 保序、有界在途任务与
缓存、`limit` 暂退回串行、销毁时停止调度并 join。新增 GTest 证明当前串行
迭代器仅消费第一组后销毁以及 `limit=5` 时，都不读取后续损坏列块；完整
扫描仍以结构化 checksum 错误失败。这是未来调度器必须维持的基线，不代表
内部并行已交付；Segment 格式及公共 API 均未改变。Release CTest 91/91
（含 fuzz/benchmark smoke），新增用例在 ASan/UBSan 和 TSan 下定向通过。

### 2026-09-28：v0.2 收尾验收快照

结论：**尚不能按第 10 节定义宣布 v0.2 完成**。这不是编码正确性阻塞，而是
发布级性能与兼容性证据尚未闭环。当前已完成的生产路径保持单线程 fallback；
同一 Reader 的独立 Scan 可并发，但单请求内部并行尚未实现或证明不值得实现。

| 验收项 | 当前证据 | 状态 |
| --- | --- | --- |
| 正确性与竞态 | 当前工作树 Release 91/91；提交 `cc8e55a` 的 ASan/UBSan 90/90、TSan 72/72 全量通过，新增早停用例在两种 sanitizer 下定向通过；含 fuzz/benchmark smoke | 本轮通过 |
| 固定性能场景 | Apple M4、Arrow 23.0.1、Release、100,000 行、RG 4,096、50% 选择率、投影 2 列、warm-cache；7 次 P50：写入 4.215 ms、scan 0.450 ms、端到端 4.668 ms；12/25 组剪枝、26 块/172,228 字节读取 | 仅此场景达第 2 节对应吞吐门槛 |
| 高基数字符串空间 | 100,000 行、24 字节高基数字符串，同环境 7 次复测：2,803,815 文件字节，0.999x 压缩比 | 达 0.98x 门槛 |
| 缓存/规模 | 三格式各 48 case 的完整 RG × 选择率 × 投影 warm-cache 矩阵已跑 7 次并保留逐 case P50/CV；尚无超页缓存数据和可信 cold-cache 结果 | warm-cache 完成；cold-cache 未完成 |
| 并发 | 多查询 1/2/4/8 线程基准与同 Reader 正确性/TSan 已覆盖；单请求内部并行、预算及取消语义没有 A/B | 未完成 |
| 版本回归 | `aeb6a17`（v0.2 前）原版 example 生成的 2,052 字节 Segment 已固定为 hex 样本；当前 Reader 的 schema、全量读取、checksum、过滤/范围/limit 测试通过。相同输入/RG 的旧版与当前固定场景及六种压缩分布也已同机复测；两代 runner 不同，结果仅作工作负载级回归参考 | 文件兼容和代表性性能回归已验证；全矩阵旧版对照未完成 |
| 内存与完整报告 | 代表性 case 已逐进程测 RSS/Arrow pool，并保留 144 case warm-cache 矩阵；另外新增 Reader-only 双进程基准，文件从 328,793 B 增至 32,915,361 B 时，扫描进程 RSS 峰值从 5,324,800 B 到 6,537,216 B，Arrow pool 峰值均为 163,840 B。目录/索引内存仍随 Row Group 数增长 | 数据路径没有整文件驻留；严格元数据上界与 cold-cache 未完成 |

固定场景复测命令：

```sh
./build-release/sniffer_core_performance_benchmark \
  '--benchmark_filter=^Performance/(Sniffer|Parquet|Parquet_ZSTD)/100000/4096/manual_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
./build-release/sniffer_core_compression_benchmark \
  '--benchmark_filter=^Compression/(narrow_int64|high_cardinality_string)_(Sniffer|Parquet|Parquet_ZSTD)/manual_time$' \
  --benchmark_repetitions=7 --benchmark_min_time=0.05s \
  --benchmark_report_aggregates_only=true --benchmark_format=json
```

旧版与当前同机回归、完整 warm-cache 矩阵、逐进程内存定义和逐 case 汇总指标见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md) 及
[`bench/V0.2_MATRIX_2026-09-28.tsv`](../bench/V0.2_MATRIX_2026-09-28.tsv)。
SortKey 范围整组证明与 1/2/4/8 worker 分片可行性实验见
[`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)；默认单线程、文件格式和
公共 API 保持不变。下一步优先补超页缓存/cold-cache 实验，并评估索引/目录
的元数据预算；正式单请求内部并行仍需有界任务预算、`limit`/取消语义、
非排序谓词及 Parquet 同条件 A/B，不能靠勾选 TODO 代替实测。
Reader-only 基准只增加 benchmark 可执行文件，不改变 Segment 格式、
生产 Reader 或公共 API；与 `DESIGN.md` 无偏离。

### 2026-09-17：CRC32C slicing-by-8 与 ARM hardware path

- codec microbenchmark 新增固定 1 MiB CRC32C case，独立于文件 I/O、编码和索引。生产实现从逐字节
  单表更新改为可移植 slicing-by-8 fallback，每轮显式 little-endian 读取 8 字节。
- AArch64 且编译器定义 `__ARM_FEATURE_CRC32` 时使用 `__crc32cd`/`__crc32cb`；8 字节读取通过
  `memcpy` 避免未对齐访问，big-endian 构建显式 byteswap。其他 ARM 目标和 x86 继续使用
  slicing-by-8，不提高最低 CPU 要求。
- 新增独立逐 bit Castagnoli reference，对 0/1/边界/尾部/8,193 字节长度验证 one-shot 与增量
  checksum。Header、Footer、ColumnChunk、index block 和全文件 checksum 的既有损坏测试继续通过，
  算法和持久化字节未改变。

同机 Release before/after（microbenchmark 与完整场景各 11 次，均取 P50）：

| 指标 | 单表 baseline | slicing-by-8 | ARM CRC | ARM 相对 slicing |
|---|---:|---:|---:|---:|
| CRC32C 1 MiB | 545.6 MiB/s | 2.335 GiB/s | 10.528 GiB/s | 约 4.51x |
| writer checksum 阶段 | 2.4242 ms | 0.5637 ms | 0.1240 ms | -78.0% |
| reader chunk checksum | 0.3103 ms | 0.0729 ms | 0.0159 ms | -78.2% |
| reader index checksum | 0.3514 ms | 0.0857 ms | 0.0192 ms | -77.6% |
| 完整写入 | 9.1581 ms | 7.4458 ms | 7.0489 ms | -5.3% |
| 端到端吞吐 | 9.054 M rows/s | 11.319 M rows/s | 12.032 M rows/s | +6.3% |

文件仍为 663,679 bytes，Arrow allocation、剪枝数、ColumnChunk 读取数和读取字节均未变化。
Debug、Release、ASan+UBSan 三套构建均为 50/50 通过，`git diff --check` 通过。

### 2026-09-17：typed statistics min/max

- 通用 statistics 原来每行分别通过 `CompareArrayRows()` 比较 min 和 max，每次比较都重新执行类型
  switch。现在按列类型一次绑定 bool、整数、浮点、timestamp、string/binary 的 typed 循环，循环内
  直接读取 Arrow value/view；只在最终 min/max 行构造 Scalar。
- NaN 仍使该 Row Group 的 min/max 缺失，null count、首个等价值胜出、string/binary 无符号字节序、
  `-0/+0` bit pattern 均保持原语义。新增全类型 scalar-reference index block 逐字节测试，并单独覆盖
  NaN、null 和 signed zero。

同机 Release、10 万行、Row Group 4,096、11 次重复的 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer index 阶段 | 1.8001 ms | 1.5590 ms | -13.4% |
| 完整写入 | 9.4931 ms | 9.1581 ms | -3.5% |
| 端到端 | 11.3627 ms / 8.801 M rows/s | 11.0443 ms / 9.054 M rows/s | 吞吐 +2.9% |

文件仍为 663,679 bytes，Arrow allocation、剪枝和读取量均未变化。Debug、Release、ASan+UBSan
三套构建均为 49/49 通过，`git diff --check` 通过。

### 2026-09-17：复用已验证主排序键的 statistics

- writer 在切分 Row Group 前已经验证整批 sort-key 顺序。主排序键同时配置 statistics 时，整数、
  bool、timestamp、string/binary 现在直接以 Row Group 首尾值生成 min/max，不再逐行重复比较；
  `BuildRowGroupIndex()` 通过显式 `sort_order_validated` 参数启用该路径，未验证的调用仍走通用实现。
- float/double 刻意保留通用路径：排序比较认为 `-0` 和 `+0` 相等，首尾替换可能改变持久化标量的
  bit pattern。新增 reference test 将 fast path 与通用路径的完整 index block 逐字节比较，并覆盖
  signed-zero fallback。

同机 Release、10 万行、Row Group 4,096、11 次重复的 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer index 阶段 | 2.2134 ms | 1.8142 ms | -18.0% |
| 完整写入 | 9.9440 ms | 9.5825 ms | -3.6% |
| 端到端 | 11.8320 ms / 8.452 M rows/s | 11.4767 ms / 8.713 M rows/s | 吞吐 +3.1% |

文件仍为 663,679 bytes，Arrow allocation、剪枝数、ColumnChunk 读取数和读取字节均未变化。
Debug、Release、ASan+UBSan 三套构建均为 48/48 通过，`git diff --check` 通过。

### 2026-09-17：FOR + Bitpack 按 byte 打包

- profiler 将 `EncodeNonPlain` 定位为最大已解析 self hotspot。FOR encoder 原来对每个 delta 的每个
  bit 分支并写入目标 byte；现在保持相同 LSB-first 字节布局，按当前 byte、完整 byte 和尾部 byte
  分段写入。base/delta 计算、encoding ID、payload header 和 decoder 均未改变。
- 新增 0–64 全部 bit width 的逐字节 reference 对照与 round-trip，包含 null；既有整数宽度、
  timestamp、极值和随机 property tests 继续通过。第一版试图移除 delta buffer，但因额外 min/max
  比较造成回退，已在提交前弃用。

同机 Release before/after（10 万行，11 次重复，均取 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| FOR encode microbenchmark | 710.6 MiB/s | 1.107 GiB/s | 约 +59% |
| writer encoding 阶段 | 6.2222 ms | 2.3235 ms | -62.7% |
| 完整写入 | 13.4723 ms | 9.7804 ms | -27.4% |
| 端到端 | 15.3318 ms / 6.522 M rows/s | 11.6660 ms / 8.572 M rows/s | 吞吐 +31.4% |

文件仍为 663,679 bytes，仍剪枝 12/25 Row Group、读取 26 个 ColumnChunk 和 172,228 bytes；
Arrow allocation 指标仍为 114 次和 1,624,448 bytes/iteration。三个 FOR 相关压缩场景的文件大小
分别保持 153,882、91,594 和 54,094 bytes。Debug、Release、ASan+UBSan 最终均为 47/47 通过；
Debug JSON smoke 在三套构建并行时曾因 allocation 采样竞争失败一次，串行重跑通过。

### 2026-09-17：CPU 与 allocation profile

- Instruments Time Profiler 的完整工作负载窗口累计 4,582 ms 样本；最大已解析 self hotspot 为
  `EncodeNonPlain`（32.13%），其次是 `HashArrayValuePair`（4.76%）、`BuildRowGroupIndex`
  （4.71%）、FOR selected decode（3.10%）和 CRC32C（2.40%）。另有 25.38% 样本未解析符号，
  所以不把 inclusive time 当作可相加的精确阶段占比。
- Instruments Allocations 在本机无法稳定附加，改用 `MallocStackLogging`/`malloc_history` 获取 live
  allocation 快照；项目内最大归属同样是 `EncodeNonPlain`（57,344 bytes）和
  `BuildRowGroupIndex`（10,864 bytes）。mimalloc 的虚拟 arena 预留已排除，带 stack logging 的耗时
  不用于吞吐结论。
- 详细场景、命令、CPU/allocation 表和限制记录在
  [`bench/PROFILE_V0.2.md`](../bench/PROFILE_V0.2.md)。profile 支持下一项优先合并 writer 的选择器、
  索引和编码遍历；当前不优先做 SIMD 或 CRC32C 专项。

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

### 2026-09-17：FOR 复用 statistics minimum

- 在 `e0e3467` 后重新采样固定性能场景；`HashArrayValuePair`、`EncodeNonPlain`、FOR decode、
  `BuildRowGroupIndex` 和 `SelectEncoding` 仍位于 top-of-stack 前列，CRC 已降为次要热点。原始热点
  摘要更新于 [`bench/PROFILE_V0.2.md`](../bench/PROFILE_V0.2.md)。
- 当字段配置了 statistics 且最终选择 FOR + Bitpack 时，encoder 直接复用 statistics 的 minimum
  作为 base，不再重新遍历整列寻找最小值。没有 statistics、字段不匹配或统计不可用时继续走原
  泛化扫描；格式和错误语义不变。
- 新增逐字节参考测试，覆盖全部 FOR 支持的整数宽度、timestamp、null、极值和 all-null。测试首次
  暴露了窄有符号整数的符号扩展要求，最终实现严格沿用原编码位语义。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer encoding | 2.3872 ms | 1.7444 ms | -26.9% |
| writer 总耗时 | 6.9960 ms | 6.3436 ms | -9.3% |
| 端到端耗时 / 吞吐 | 8.2511 ms / 12.120 M rows/s | 7.6080 ms / 13.144 M rows/s | 吞吐 +8.5% |

文件保持 663,679 字节，Arrow 分配仍为 114 次 / 1,624,450 bytes；仍剪枝 12/25 Row Group，
读取 26 个 ColumnChunk 和 172,228 字节。下一小步继续处理 selector 与 index 的样本分析复用，
重点是当前 Bloom string 与整数 statistics 路径。

### 2026-09-17：整数 statistics 复用 selector sample

- `BuildRowGroupIndex()` 可选地产生不落盘的 Row Group 分析结果。非排序整数、bool 和 timestamp
  statistics 在既有 typed min/max 遍历的前 `encoding_sample_rows` 行同步统计 distinct、run 和
  extrema，`SelectEncoding()` 直接消费该摘要。
- 排序主键 statistics 仍使用 endpoint 快速路径，没有为了 selector 新增完整扫描；未配置
  statistics、string/binary 和不可复用的输入继续使用原 selector 路径。
- reference test 比较复用前后的编码 ID 和序列化 index bytes，覆盖所有整数宽度、bool、timestamp、
  null 和极值；分析结果不进入 Segment 格式。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| encoding selection | 1.8225 ms | 1.0945 ms | -39.9% |
| index | 1.6207 ms | 2.1369 ms | +31.8% |
| selection + index | 3.4432 ms | 3.2314 ms | -6.2% |
| writer 总耗时 | 6.3436 ms | 6.1797 ms | -2.6% |
| 端到端耗时 / 吞吐 | 7.6080 ms / 13.144 M rows/s | 7.4572 ms / 13.410 M rows/s | 吞吐 +2.0% |

单阶段时间发生了预期迁移，因此收益以 selection + index 合计和 writer 总耗时判断。文件仍为
663,679 字节，Arrow 分配、剪枝数量、ColumnChunk 读取数与读取字节不变。下一小步可把相同模型
扩展到 Bloom string 的 selector dictionary/run 样本，当前 `HashArrayValuePair` 仍是 profile 首位。

### 2026-09-18：绑定 Bloom typed hasher

- 先尝试在 Bloom string 遍历中同步生成 dictionary/run 样本。第一版使 writer 回退约 2.9%；复用
  Bloom 第一组哈希并拆分热循环后，阶段合计改善仍只有约 0.5%，端到端没有稳定收益，因此完整撤销，
  未为微弱结果保留额外状态与分支。
- 改为在每个 Bloom 列开始时绑定 `FieldSpec`、Arrow array 和 physical type。逐行仍执行原双种子
  FNV/Mix64 算法并保留 row/null 边界检查，但不再重复调用 Arrow type equality 与
  `PhysicalTypeFor()`。
- reference test 对全部首期平铺类型比较安全入口、绑定入口和 Scalar reference，并覆盖 null、负数、
  signed zero、越界 row 与类型不匹配；Bloom 字节和查询语义保持不变。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer index | 2.1369 ms | 1.9700 ms | -7.8% |
| writer 总耗时 | 6.1797 ms | 6.0562 ms | -2.0% |
| 端到端耗时 / 吞吐 | 7.4572 ms / 13.410 M rows/s | 7.3221 ms / 13.657 M rows/s | 吞吐 +1.8% |

文件仍为 663,679 字节，Arrow 分配、剪枝数量、ColumnChunk 读取数与读取字节不变。该收益大于本轮
P50 运行波动，且实现比字符串遍历融合更小；后续重新 profile 后再选择下一热点。

### 2026-09-18：FOR bytewise unpack

- 在绑定 Bloom typed hasher 的版本上重新采样后，Bloom 的剩余成本主要是实际双哈希计算，FOR typed
  decode 已成为第二大已解析 top-of-stack 热点。原 decoder 虽已移除 Scalar，但每个值仍逐 bit
  读取 payload。
- 新 decoder 按首个非对齐字节、连续完整字节和末尾残余 bit 三段读取 delta；不改变 base、validity、
  payload 边界验证或 Arrow builder 路径。全部 bit width 0–64 同时覆盖 full decode 和包含 null、
  非对齐 row 的 selected decode。

同机 Release 的 FOR codec decode microbenchmark（100,000 个 `uint64`、128 值范围、21 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| decode latency | 383 us | 314 us | -18.0% |
| logical throughput | 1.979 GiB/s | 2.412 GiB/s | +21.9% |

固定文件场景（100,000 行、Row Group 4,096、50% 选择率、11 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| scan | 1.2681 ms | 0.9628 ms | -24.1% |
| writer 总耗时 | 6.0562 ms | 5.9326 ms | -2.0% |
| 端到端耗时 / 吞吐 | 7.3221 ms / 13.657 M rows/s | 6.8913 ms / 14.511 M rows/s | 吞吐 +6.3% |

文件仍为 663,679 字节；Arrow 分配、12/25 Row Group 剪枝、26 个 ColumnChunk 和 172,228 个读取字节
均保持不变。microbenchmark CPU CV 为 3.51%，完整场景 CPU CV 为 1.04%。

### 2026-09-18：绑定 FOR delta typed loop

- 在 `6747ebf` 上重新采样后，`EncodeNonPlain` 和 `ArrayIntegralBits` 仍位于 writer 热点前列。
  FOR encoder 计算 delta 时原先每行进入 `ArrayIntegralBits()` 的 type switch；现在先按 Arrow type
  绑定模板循环，再直接读取 typed array。base、delta 的无符号位语义和 payload 字节不变。
- reference test 增加 Arrow sliced array，连同全部整数宽度、timestamp、null、极值和随机输入验证
  typed loop 与 Scalar reference 的逐字节一致性。

同机 Release 的 FOR codec encode microbenchmark（100,000 个 `uint64`、128 值范围、21 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| encode latency | 670 us | 575 us | -14.2% |
| logical throughput | 1.130 GiB/s | 1.318 GiB/s | +16.6% |

固定文件场景（100,000 行、Row Group 4,096、50% 选择率、11 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer encoding | 1.7211 ms | 1.5797 ms | -8.2% |
| writer 总耗时 | 5.9326 ms | 5.8406 ms | -1.6% |
| 端到端耗时 / 吞吐 | 6.8913 ms / 14.511 M rows/s | 6.7926 ms / 14.722 M rows/s | 吞吐 +1.5% |

文件、分配、剪枝和读取指标不变。microbenchmark CPU CV 为 0.88%，完整场景 CPU CV 为 0.20%。

### 2026-09-18：Bloom known-valid hash path

- `BuildRowGroupIndex()` 已在进入 Bloom 插入前检查 row 边界、null 和 NaN，但绑定 hasher 的安全入口
  又重复检查边界/null，并为每行构造 `Result<pair>`。新增带明确前置条件的 `HashKnownValid()`；只有
  完成上述检查的索引热循环使用它，其他调用者仍使用返回结构化错误的 `Hash()`。
- FNV 常量、physical-type 前缀、little-endian value bytes、signed-zero 归一化、两个 seed 与 Mix64
  均未改变。全部平铺类型逐值比较安全入口、known-valid 入口和 Scalar reference。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50；首次运行受文件写入离群值
影响，以下采用紧接着的确认运行（wall/CPU CV 0.46%/0.47%）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer index | 1.9879 ms | 1.7161 ms | -13.7% |
| writer 总耗时 | 5.8406 ms | 5.5042 ms | -5.8% |
| 端到端耗时 / 吞吐 | 6.7926 ms / 14.722 M rows/s | 6.4689 ms / 15.459 M rows/s | 吞吐 +5.0% |

文件仍为 663,679 字节；Arrow 分配、12/25 Row Group 剪枝、26 个 ColumnChunk 和 172,228 个读取字节
均保持不变。

### 2026-09-18：批量复制 Arrow validity bitmap

- `EncodeValidity()` 原先逐行调用 `IsValid()` 再组装 bitmap。Plain、Dictionary 和 FOR 现在直接用
  Arrow bitmap primitive 从 `array.offset()` 开始复制到零初始化的目标，保留尾部 padding bit 为零。
- 新增 nullable sliced binary 的 Plain reference 比较；既有 sliced FOR、全部 bit width、all-null、
  Dictionary 和确定性输出测试共同覆盖非 byte-aligned offset 与落盘字节一致性。

同机 Release 的 nullable FOR codec encode microbenchmark（100,000 行、21 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| encode latency | 575 us | 400 us | -30.4% |
| logical throughput | 1.318 GiB/s | 1.892 GiB/s | +43.6% |

固定文件场景（100,000 行、Row Group 4,096、50% 选择率、11 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer encoding | 1.5698 ms | 1.4255 ms | -9.2% |
| writer 总耗时 | 5.5224 ms | 5.3795 ms | -2.6% |
| 端到端耗时 / 吞吐 | 6.4561 ms / 15.489 M rows/s | 6.3531 ms / 15.740 M rows/s | 吞吐 +1.6% |

文件、Arrow 分配、剪枝和读取指标不变。microbenchmark CPU CV 为 0.77%，完整场景 CPU CV 为
1.03%。

### 2026-09-18：连续存储 statistics distinct sample

- 整数 statistics 与 encoding selector 共用的最多 4,096 行 sample 不再用 node-based
  `unordered_set<uint64_t>` 逐 distinct value 分配；改为预留连续 vector、收集有效 bits，再排序去重。
- 超过 4,096 行的用户配置继续使用原 hash-set 路径，避免把任意大 sample 强制变成 O(n log n)。
  1,024 行快路径和 5,000 行 fallback 都与直接 selector 的 encoding ID 一致；持久化 statistics bytes
  不变。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50；采用加入大 sample fallback
后的最终确认运行：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer index | 1.7176 ms | 1.2402 ms | -27.8% |
| writer 总耗时 | 5.3795 ms | 4.9502 ms | -8.0% |
| 端到端耗时 / 吞吐 | 6.3531 ms / 15.740 M rows/s | 5.9501 ms / 16.807 M rows/s | 吞吐 +6.8% |

前两轮 index P50 为 1.2281 ms 和 1.2274 ms，最终保守值仍明显高于运行波动；最终 wall/CPU CV 为
0.54%/0.55%。文件、Arrow 分配、剪枝和读取指标不变。

### 2026-09-18：批量写入 Dictionary index

- `d289ccb` 后的 profile 显示，Dictionary payload 仍逐个 index 调用 `ByteWriter::WriteU8()`；调用树中
  该路径占 234 个以上样本。现在按已确定的 1/2/4/8 字节宽度一次分配目标 buffer，并直接写入规范的
  little-endian 字节，避免每行函数调用与 vector 增长检查。
- reference test 在既有全部字段类型基础上增加 257 和 65,537 个 distinct value，覆盖 2 字节和
  4 字节宽度边界；既有低基数输入覆盖 1 字节宽度。8 字节宽度需要超过 `uint32_t` 上限的 distinct
  value，无法作为常规单元测试构造，但由同一通用写入循环处理。

同机 Release 的 Dictionary string encode microbenchmark（100,000 行、32 个 distinct value、21 次
P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| encode latency | 783 us | 639 us | -18.4% |
| logical throughput | 1.354 GiB/s | 1.656 GiB/s | +22.3% |

固定文件场景（100,000 行、Row Group 4,096、50% 选择率、11 次 P50）：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer encoding | 1.4614 ms | 1.2513 ms | -14.4% |
| writer 总耗时 | 4.9502 ms | 4.7825 ms | -3.4% |
| 端到端耗时 / 吞吐 | 5.9501 ms / 16.807 M rows/s | 5.7627 ms / 17.353 M rows/s | 吞吐 +3.3% |

确认运行的 wall/CPU CV 为 1.56%/1.64%，其中 writer encoding CPU CV 为 0.77%；microbenchmark CPU
CV 为 0.46%。Dictionary payload 仍为 113,058 字节，完整 Segment 仍为 663,679 字节；Arrow 分配、
12/25 Row Group 剪枝、26 个 ColumnChunk 和 172,228 个读取字节均保持不变。

### 2026-09-21：融合单列 sort-key 校验

- `dea1076` 后重新 profile，排除 benchmark 启动期符号后，项目内 top-of-stack 包括
  `BuildRowGroupIndex` 369、`EncodeNonPlain` 233、Bloom known-valid hash 205、FOR decode 143、
  `BuildStatistics` 118、Dictionary string map 107、`ValidateAndUpdateSortOrder` 104 和 CRC32C 100。
- 单列 sort-key 原先先遍历全部值检查 null/NaN，再次遍历相邻值并在每行进入 type switch。现在先
  按 Arrow type 绑定 typed loop，在一次遍历中完成两项检查；仍先报告 null/NaN，再执行跨 batch
  边界检查，最后报告局部逆序，因此错误类别和优先级不变。
- 新增全部首期平铺类型的有序/逆序测试，并显式覆盖重复值、浮点 signed zero、NaN、string、binary
  和 timestamp；既有测试继续覆盖跨 `Append()` 边界逆序。

同机 Release、100,000 行、Row Group 4,096、50% 选择率、11 次 P50：

| 指标 | Before | After | 变化 |
|---|---:|---:|---:|
| writer validation | 0.4059 ms | 0.3127 ms | -23.0% |
| writer 总耗时 | 4.7115 ms | 4.6200 ms | -1.9% |
| 端到端耗时 / 吞吐 | 5.6907 ms / 17.573 M rows/s | 5.5972 ms / 17.866 M rows/s | 吞吐 +1.7% |

确认运行 wall/CPU CV 为 0.62%/0.79%，validation CV 为 1.44%。Segment 仍为 663,679 字节，Arrow
分配、12/25 Row Group 剪枝、26 个 ColumnChunk 和 172,228 个读取字节均保持不变。

### 2026-09-23：文件格式对照切换为 Parquet

- 性能与压缩 benchmark 的当前对照改为未压缩 Parquet 和 Parquet + ZSTD；两个 Parquet case 均
  使用默认 dictionary 设置、单线程和与 Sniffer 相同的 Row Group 大小。
- 性能路径按 Footer 中 `id` 的 Row Group statistics 剪枝，并仅读取谓词与投影所需列；单谓词、
  三谓词、sort-key range 和 1%/10%/50%/100% 选择率矩阵均有对应 case。压缩路径逐轮全量
  回读并在计时外验证 Arrow batch 一致性。
- 已建立 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md) 初版。固定 warm-cache 场景下三者
  剪枝均为 12/25 Row Group，Sniffer、Parquet、Parquet + ZSTD 端到端 P50 分别为 5.431、
  8.528、11.459 ms，文件分别为 663,679、1,955,982、514,667 字节。完整矩阵、宽表、cold-cache
  和相对 v1 的同口径回归仍待补齐，故本专项完成定义暂不勾选。

### 2026-09-23：全非空变长列的 Plain 大小估算

- Dictionary 等非 Plain 编码需要记录等价 Plain 长度。全非空 string/binary 的长度可由已校验的
  Arrow offset 直接算出；新路径对 slice 和截短 sample 仍适用。含 null 列继续逐行累计有效值长度，
  保持 null 槽中潜在数据不计入文件大小的语义。未更改编码 ID、payload 或 checksum。
- 对所有起点和长度的 sliced string、nullable binary 比较 `PlainEncodedSize()` 与实际 Plain
  payload 大小；完整 round-trip、fuzz smoke 以及 Release/Debug/ASan+UBSan 各 55 项测试通过。
- 同机 Release、100,000 行、Row Group 4,096、低基数字符串压缩专项：改动前 7 次 P50
  写入 1.533 ms、端到端 2.930 ms（写入 CV 1.92%）；改动后 11 次 P50 写入 1.379 ms、
  端到端 2.847 ms（写入 CV 1.37%）。这是该场景的一轮短时测量，不能外推到其他分布。
  文件保持 117,044 字节。固定性能场景复测仍为 663,679 字节、12/25 Row Group 剪枝、
  26 个 ColumnChunk 和 172,228 个读取字节。
- CTest 中 benchmark smoke 使用同名临时文件；为避免并行运行相互覆盖，将这些 smoke case
  加入同一资源锁，不改变库或正式 benchmark 的行为。

### 2026-09-23：无谓词全量扫描的连续 selection

- 无 predicate、无 sort-key range 时，原路径对每一行调用空的 `RowMatches()` 和
  `SortKeyRowMatches()`。现在直接生成 `[0, min(row_group_rows, remaining_limit))`，其他
  filter/range 路径保持原样；未更改文件格式、剪枝或解码策略。
- 新增 full-scan 性能 case，Sniffer、Parquet 和 Parquet + ZSTD 使用相同输入、Row Group 和投影。
  额外测试覆盖 `limit=0/1/7/30`，检查逐行结果、批次顺序及 predicate/projection chunk 指标。
- Apple M4、Release、100,000 行、Row Group 4,096、无过滤、投影 `id,value`：改动前 7 次
  P50 扫描 1.277 ms（wall CV 1.06%），改动后 11 次 P50 扫描 1.128 ms（wall CV 0.64%），
  约下降 11.7%。完整 Segment 仍为 663,679 字节，输出 100,000 行，读取 50 个 ColumnChunk /
  339,076 字节。短时 warm-cache 合成数据，不代表所有查询收益。

### 2026-09-23：Bloom 非浮点列的 NaN 检查外提

- 之前 Bloom 每行都调用 `ArrayValueHasNaN()`，即使列类型不可能包含 NaN。现在每列只判断一次
  类型，float/double 仍逐值检查 NaN。全首期类型和含 NaN/null 的浮点列以 Scalar reference
  验证 Bloom bit pattern，未改变哈希、索引或持久化格式。
- Apple M4、Release、100,000 行、Row Group 4,096、50% 选择率、投影 `id,value`：同机
  11 次 P50 的 writer index 阶段从 1.210 ms 降至 1.156 ms；第二轮复测为 1.152 ms。
  完整端到端结果从 5.199 ms 到第一轮 5.239 ms、第二轮 5.171 ms，变化尚不足以断言
  全路径提速。文件仍为 663,679 字节，12/25 Row Group 剪枝、26 个 ColumnChunk 和
  172,228 个读取字节不变。

### 2026-09-23：16 列宽表基准

- 性能 benchmark 增加 16 列、100,000 行、Row Group 4,096 的宽表：原 `id/group/value`
  加 13 列确定性非空 int64；每列按行号线性生成。查询仍为 `id >= 50000`，投影 `id,value`，
  因而主要衡量宽表写入、文件体积和窄投影扫描，不代表宽投影或真实列间分布。
- Sniffer、未压缩 Parquet、Parquet + ZSTD 统一使用 25 个 Row Group；均输出 50,000 行，
  剪枝 12/25 个 Row Group，候选读取列块数为 26。新增 JSON smoke 校验 16 输入列。
- Apple M4、Release、单线程、warm-cache，11 次 P50：Sniffer 写入/扫描 22.297/1.305 ms，
  3,104,713 字节；Parquet 为 40.439/1.852 ms，14,374,323 字节；Parquet + ZSTD 为
  59.152/2.405 ms，3,869,950 字节。Sniffer 写入 CV 为 1.00%，Parquet + ZSTD 扫描
  CV 为 6.69%。超页缓存及 cold-cache 项仍未完成，不能以此判断生产环境表现。

### 2026-09-23：自适应编码实际大小保护

- 新增采样偏斜整数分布：每个 4,096 行 Row Group 的前 1,024 行为 0，其余值互异。
  旧选择器只看前 1,024 行，选 RLE 后完整编码的 payload 会比 Plain 大；先补该场景
  的失败测试，再在自适应路径中用已计算的 `PlainEncodedSize()` 与实际编码大小比较。
  若非 Plain payload 不小于 Plain，则回退 Plain；用户显式指定的 encoding 不改写。
- Apple M4、Release、100,000 行、Row Group 4,096，改动前 7 次 P50：Sniffer
  写入/读取 3.846/2.499 ms，文件 1,789,694 字节；改动后 11 次 P50：
  3.550/0.833 ms，文件 803,694 字节。压缩比由 0.447x 升至 0.995x。
  原有六种压缩场景的 Sniffer 文件字节数完全不变；Parquet 对照及限制见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
- 回退前已经完成一次非 Plain 编码，故这一步保障空间上界但未解决无效编码的 CPU
  成本；更完整的编码成本模型仍在 TODO。文件格式版本、编码 ID 和读取兼容性不变。

### 2026-09-23：编码选择与大小回退的诊断计数

- `WriterMetrics` 增加按已写入 ColumnChunk 计数的四类互斥结果：自适应直接 Plain、
  自适应保留非 Plain、自适应实际大小回退、显式强制编码。大小回退额外累计被弃编码
  payload 和最终 Plain payload 的字节数。指标只在显式传入 metrics 时记录，不落盘。
- 压缩 benchmark 为 Sniffer case 输出这些计数。采样偏斜 100,000 行 case 共 25 个
  Row Group，25 个均因大小回退；被弃 payload 共 1,786,600 字节，替换成 Plain
  payload 共 800,600 字节。长 RLE case 的 25 个 Row Group 均保留非 Plain，回退为零。
  两个 case 的文件大小分别仍为 803,694 和 7,166 字节。基准细节见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-23：选择器采样 null 统计去重

- `SelectEncoding()` 原先为 Plain 样本大小与候选估算分别创建 Arrow slice 并统计 null；
  改为一次按 bitmap offset 计数，或在全长样本复用 `array.null_count()`。空 bitmap
  直接判定无 null，含 null 的切片与独立构造的同值数组作选择器等价测试。
- Apple M4 / Release / 单线程、100,000 行、Row Group 4,096，11 次重复、
  `--benchmark_min_time=0.03s`：标准性能场景 writer 编码选择 P50 由 1.028 ms
  到 0.998 ms，写入 P50 由 4.283 ms 到 4.233 ms；两组 CV 分别为 1.62%/1.46%
  与 1.31%/1.25%。低基数字符串压缩写入 P50 由 1.402 ms 到 1.377 ms；
  高基数字符串写入波动较大，不能据此断言提速。三个场景的文件字节完全不变。
  性能增益很小，仍需在固定硬件负载下交错重测后才可宣称稳定提速。

### 2026-09-24：无谓词整组扫描免建 selection

- 对无谓词、无 sort-key range 且当前 Row Group 未被 `limit` 截断的扫描，直接使用
  既有全量 Plain/Dictionary/RLE/FOR 解码路径，不分配 `[0..N)` selection。部分
  Row Group 仍走原 selected-decode；列投影、输出 batch 大小、limit、checksum 与
  剪枝语义均不变。新增强制编码混合、多 Row Group、空投影、部分 limit 及损坏的
  超大 row count 测试。
- Apple M4 / Release / 单线程，100,000 行、Row Group 4,096、投影两列、
  warm-cache、11 次重复、`--benchmark_min_time=0.03s`：full-scan scan P50
  改前 1.116 ms，改后两轮 1.068/1.091 ms；文件均为 663,679 字节，读取均为
  50 个 ColumnChunk / 339,076 字节。改前/改后第一轮 CV 为 2.35%/1.22%，
  第二轮为 1.30%。差异较小，暂不据此断言稳定提速；过滤扫描路径未改。

### 2026-09-24：输出 batch 边界诊断与快路径否决

- 增加 Sniffer-only `PerformanceAligned` 诊断 case：同为 100,000 行、Row Group 4,096、
  无过滤及两列投影，但 `output_batch_rows=4096`；与原 full-scan case 的 2,049 行输出
  并列观察。它不是 Parquet 格式对照，专门用于定位输出 batch 边界成本。
- Apple M4 / Release / 单线程、warm-cache，独立运行 11 次、
  `--benchmark_min_time=0.03s`：对齐 case 的 scan P50 为 1.011 ms，
  batch materialization P50 为 0.0038 ms，Arrow allocation 为 100；跨组 case 分别
  为 1.052 ms、0.0449 ms 和 172。两者文件均为 663,679 字节；scan CV 分别为
  2.20%/2.12%。对齐输出降低了拼接和分配，但两种输出 batch 规格不同，不能把
  差值当作同一 API 工作量下的纯实现提速。
- 曾尝试在单组输出时直接复用完整 `RecordBatch`，跳过 `Slice()`；保持同样诊断
  计数的 A/B 中，对齐 case 基线 P50 1.041 ms，快路径两轮 P50 1.039/1.075 ms，
  没有稳定收益，因此撤回快路径及临时公共指标。当前单组输出仍走零拷贝 `Slice()`；
  真正跨组且调用者要求固定 batch 大小时仍需拼接，相关 TODO 保持未完成。

### 2026-09-25：谓词全命中 Row Group 免 identity selection

- 谓词/排序范围扫描先探测连续命中前缀；若整组都命中，直接复用已解码的谓词列，并对
  其余投影列调用原有全量解码路径。首个失配后切回原有稀疏行号遍历；`limit` 截断的
  前缀仍构造精确 selection。文件格式、行序、null、checksum、索引剪枝均未改。
- Apple M4 / Release / 单线程 / warm-cache，100,000 行、Row Group 8,192、两列投影，
  每个场景 11 次、`--benchmark_min_time=0.03s`。同一轮新增指标之后、优化之前的
  scan P50 在 1%/10%/50%/100% 选择率下分别为 0.244/0.424/0.940/1.652 ms；
  分离稀疏循环后的 P50 分别为 0.243/0.395/0.817/1.347 ms，CV 为
  1.64%/1.46%/1.73%/1.25%。显式 selection 行号逻辑字节分别从
  8,000/80,000/400,000/800,000 变为 8,000/896/58,752/0；文件均为
  675,943 字节。该数据分布是按升序 `id` 做阈值过滤，易产生整组全命中；对随机
  稀疏过滤的收益不能由此推断。
- 拼接指标显示同一矩阵下 1%/10%/50%/100% 的跨组拼接次数为 0/2/6/12；
  优化前拼接耗时 P50 分别为 0/0.0042/0.0148/0.0275 ms，低于相应扫描总耗时。
  当前保留固定 batch 大小契约，不为省去拼接改变输出形状；相关 TODO 未完成。

### 2026-09-25：重复 Arrow 深度校验去重

- `Scan()` 输出中的每个数组现于 Plain/非 Plain 解码或谓词列选择后执行
  `Array::ValidateFull()`；`RecordBatch` 仍执行 `Validate()` 以检查列数、长度与类型。
  原先部分数组已深度校验，随后 `RecordBatch::ValidateFull()` 对所有列再次逐值检查。
  此调整不跳过任何输出数组的完整校验，也不改变格式校验、CRC 或结构化错误路径。
- 同机 Release / 单线程 / warm-cache、100,000 行、Row Group 8,192、100% 选择率，
  11 次重复的三列投影（含 string）batch materialization P50 约 0.291 → 0.058 ms，
  scan P50 2.763 → 2.605 ms，第二轮 2.633 ms；文件均为 675,943 字节。
  两列数值投影在两轮复测约 1.409/1.399 ms，对照此前 1.342 ms，不能宣称该场景
  提速；仍需交错 A/B 才能判断是否回退。
- 候选 chunk 合并读取的同场景诊断：三列扫描的 chunk I/O P50 约 0.127 ms，
  decode P50 约 1.799 ms、scan P50 约 2.798 ms。warm-cache 下减少少量相邻
  读取调用不是当前最大热点；冷缓存结论尚缺，I/O TODO 保持未完成。

### 2026-09-25：selection 表示微基准

- 新增 `sniffer_core_selection_benchmark`：100,000 行，均匀 hash 与连续前缀两类
  确定性命中分布，比较 64-bit 行号、64-bit bitmap 和 `[begin,end)` ranges 的
  构建＋遍历耗时及容器容量；运行前逐 case 与独立 mask reference 核对命中数和
  求和。CTests 包含 Google Benchmark dry-run 与 JSON smoke。
- Apple M4 / Release / 单线程，11 次 P50：均匀散点 10% 时行号/bitmap/range
  约 47.9/36.5/52.6 µs；50% 时约 178.1/104.9/221.6 µs。当前扫描
  row-group-sized reserve 下行号占 800 KB，bitmap 占 12.5 KB；连续前缀
  range 仅 16 字节。该微基准不包含 ColumnChunk 解码、Arrow builder 和
  `limit`，不能据此为生产 scan 设置选择阈值；顶层 TODO 保持未完成。

### 2026-09-25：Plain bitmap selected-decode 候选

- 增加只在内存使用的 `BitmapSelection`，校验 word 数量和末尾越界 bit，按递增行号
  迭代；Plain selected-decode 共用原来的 payload、validity、offset 与 Arrow 输出
  校验逻辑，支持所有 Plain 类型。没有修改 Segment 格式或公开 `IOPlan`。
- Apple M4 / Release / 单线程、100,000 行 nullable int64、11 次 CPU P50：均匀
  散点 1% 行号/bitmap 为 22.51/23.86 µs，10% 为 45.70/43.35 µs，50% 为
  143.67/134.92 µs。完整矩阵及复现命令见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。这只测预建 selection 到
  Arrow array，不含 bitmap 构建、谓词、非 Plain codec 和端到端扫描；生产路径
  暂保留 index vector，尚无可信阈值。

### 2026-09-25：连续谓词投影的零拷贝 slice

- 对已解码谓词列与 projection 重合、命中行号连续的情况，`SelectArray()` 直接使用
  `Array::Slice()`。行号由扫描器保证严格递增；首末跨度即可判定连续，不增加逐行检查。
  非连续选择仍走原 typed builder；文件格式、过滤和 batch 大小未变。
- Apple M4 / Release / 单线程 / warm-cache、100,000 行、有序 `id` 单谓词、仅投影
  `id`、输出 batch 为半组加一、7 次 P50，Row Group 为 8,192 或 65,536。
  4 个场景 baseline → 改后两轮 scan 分别为 0.348 → 0.345/0.343 ms、
  0.604 → 0.583/0.577 ms、0.416 → 0.400/0.394 ms、
  0.809 → 0.792/0.795 ms（顺序为 8K/10%、8K/50%、64K/10%、64K/50%）。
  projection 阶段由 0.0007–0.034 ms 降至约 0.00007–0.00012 ms；
  文件字节与 selection 行数一致。完整维度及复现命令见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。这是一组有序连续命中
  场景，不能外推为随机过滤或全部扫描的加速。

### 2026-09-25：bitmap 完整扫描 A/B 与默认接入否决

- 新增 100,000 行、Row Group 8,192、nullable int64 的真实 Segment 扫描基准。
  过滤列是确定性 boolean mask，投影列强制 Plain；扫描每次只读谓词列及有命中
  的投影列，并用独立 mask 参考核对命中数、null 数、值求和和读取 chunk 数。
  另外增加 8,192 行两 Row Group 的逐值测试，覆盖 1%/10%/50%/100%、
  均匀/连续分布、`limit`、FOR 投影和谓词列复用。
- 临时接入的自适应 bitmap 在行号字节超过 bitmap 字节时切换，仅限 Plain 投影。
  Apple M4 / Release / 单线程 / warm-cache，交错 index/bitmap/index/bitmap，
  每轮 11 次 CPU P50：均匀 10% 时 index 为 0.965/0.990 ms、bitmap 为
  0.986/0.995 ms；均匀 50% 时 index 为 1.723/1.739 ms、bitmap 为
  1.762/1.795 ms。连续前缀 10% 时 bitmap 略快，50% 基本持平。
  文件与 ColumnChunk 读取数一致。尽管均匀 50% 的逻辑选择表示从约
  400 KB 降到约 19 KB，端到端耗时仍回退，说明先前微基准不能作为生产
  阈值依据。
- 已撤回临时生产接入和额外公开指标；默认仍是 index vector，完整匹配组仍
  是隐式范围。保留 `BitmapSelection` 的内部解码候选与可复跑的完整扫描基准。
  未来若要以降低峰值内存为目标重新评估，应另设明确内存预算并覆盖非 Plain
  codec，而非仅依据本次吞吐数据启用 bitmap。

### 2026-09-25：CompactPlain 32-bit offset

- 为 string/binary 新增 encoding ID 4，保留旧 Plain ID 0 及其字节语义；格式兼容规则见
  [`docs/decisions/0004-compact-plain-variable-offsets.md`](decisions/0004-compact-plain-variable-offsets.md)。
  自适应选择按现有确定性阈值比较样本大小；旧文件仍可读，新格式对旧 Reader 显式不兼容。
- 100,000 行、24 字节高基数字符串、Row Group 4,096 的完整文件由 3,203,894
  降到 2,803,815 字节，压缩比 0.874x → 0.999x，达到本专项 0.98x 目标。
  强制 CompactPlain 的写入两轮 P50 3.067/3.034 ms，对照强制 Plain
  3.679/3.490 ms；自适应写入 4.220/4.002 ms，不能把强制编码收益等同
  于默认写入收益。读取对照和 Parquet 数据见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。低基数字符串仍选择
  Dictionary，文件字节不变。
- 切片、nullable、空值、随机 binary、selected decode、错误 offset、截断、
  checksum、未知 descriptor/版本和旧 Plain 兼容均有测试。并发及 cold-cache
  等 v0.2 验收项仍未完成。

### 2026-09-25：变长列字典候选的安全提前终止

- 对 string/binary 样本，已见不同值的字节数、offset 数、validity 和最终样本
  索引向量提供单调不减的字典编码大小下界。每新增 16 个不同值检查一次；
  下界超过当前候选时才跳过剩余样本，低基数候选继续完整采样，编码决策
  与原规则一致。压缩 benchmark 增加 `encoding_selection_ms` 观测指标。
- Apple M4 / Release、100,000 行、Row Group 4,096、高基数 24 字节字符串、
  11 次 P50：编码选择 0.947 → 0.816/0.820 ms；完整写入 4.030 →
  3.847/3.955 ms，第二轮写入波动较大。低基数选择阶段 0.267 →
  0.274/0.284 ms，不能宣称该场景获益。文件字节和选择的 encoding ID
  都不变；明细见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-25：并发读取第一步——独立 Scan 与 `pread`

- 同一 Reader 的不同 Scan iterator 各有计划、缓冲、footer/index 副本和 metrics；
  单个 iterator 的 `Next()`、单个 Writer 的写入仍须由调用方串行化。
  macOS/Linux 改用同一只读 fd 上按 offset 的 `pread`，其他平台保留带互斥的
  流式回退；保持原有 bounds、chunk CRC 和 Reader/iterator 生命周期语义。
- 四线程不同 IOPlan、空结果、`limit`、跨组分批反复与串行参考逐 batch 比较；
  同时 `ReadAll()`/文件校验、Reader 销毁后 iterator 消费、一个查询剪枝损坏块
  另一个查询读出 checksum 错误均有测试。未开启内部线程池。
- Apple M4 / Release / warm-cache、500,000 行双列、Row Group 8,192、
  50% 选择率、双列投影，8 线程共享 Reader 的每查询 P50 从
  6.228/7.075 ms 降到 4.036/3.986 ms；`pread` 路径与每线程独立 Reader
  接近。完整 1/2/4/8 线程数据及复现命令见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。冷缓存、Parquet 并发
  对照、内存预算和受控内部并行仍未完成。
- Release 与 ASan/UBSan 全量 82/82 通过，ThreadSanitizer 单测全量 66/66 通过；
  尚未在当前报告中给出 Parquet 同条件线程扩展、长时压力或大文件冷缓存结果。

### 2026-09-27：同条件 Parquet 多查询并发基线

- 同一确定性 500,000 行双 int64、Row Group 8,192、50% 选择率、两列投影；
  Parquet 采用每线程独立 Reader、Row Group 统计剪枝和有序 `id` 边界 slice，
  不含内部线程池或 Reader 构造时间；两边计时前均逐值验证并热身。
- Apple M4 / Release / warm-cache、7 次 P50（ms），1/8 线程：Sniffer 独立
  Reader 2.634/4.049；Parquet 无 codec 压缩 1.104/2.797；Parquet ZSTD
  4.519/6.568。11 次复测维持同方向。文件分别为 1.958/9.654/2.721 MB。
  完整 1/2/4/8 线程数据与复现命令见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。
- 结论限定于有序双 int64 合成数据：Parquet 无 codec 压缩更快但文件更大；
  Sniffer 比 Parquet ZSTD 更小且更快。尚未覆盖冷缓存、RSS/分配量、不同
  选择率或单请求内部并行，不能泛化为整体格式胜负。

### 2026-09-27：FOR 解码热点定位与密集值快路径

- 对前述并发查询补 `ScanMetrics` 阶段耗时：改动前 1 线程 2.598 ms P50，
  解码 1.545 ms、谓词 0.744 ms、跨组拼接 0.101 ms，故优先处理 FOR
  decoder 而非拼接。
- FOR 在 `Reserve` 后使用 builder `UnsafeAppend`；无 null 时避免逐行 validity
  检查，null 路径保留，delta 域检查和 `ValidateFull()` 不变。1/8 线程查询
  P50 2.598/3.946 → 1.955/3.288 ms；单线程解码 1.545 → 0.932 ms。
  文件格式与输出语义不变。全部位宽的含 null、无 null、selected decode
  回归已补齐；原始方法与数据见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-27：Statistics 证明整组命中，跳过逐行谓词

- 对每个候选 Row Group，仅在全部谓词都能由统计信息证明为真且没有排序键范围时，
  跳过逐行谓词判断；缺统计、比较列含 null/NaN 或无法证明时保守回退，
  null 测试则使用 `null_count`。仍读取/校验
  谓词 ColumnChunk，并保持 projection、limit、输出 batch 和格式语义不变。
- Apple M4 / Release / warm-cache、11 次 P50，500,000 行双 int64、Row Group
  8,192、50% 选择率：共享 Reader 的 1/8 线程查询 1.942/3.294 →
  1.273/2.203 ms，谓词阶段 0.696 → 0.022 ms；解码阶段基本不变。
  无统计回退、`limit`、全 null 和 NaN 均有回归测试。数据与复现命令见
  [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-27：整组命中时不读未投影谓词列

- 全组匹配证明成立后，谓词列若未投影，就不读取该 ColumnChunk；若投影，
  仍通过投影路径读取与校验。边界组及无统计回退保持逐行过滤。
- 500,000 行双 int64、50% 选择率、仅投影 `value`，Apple M4 Release
  9 次 P50：共享 Reader 1/8 线程 1.21/2.04 → 0.681/1.20 ms；
  每查询读取块数 64 → 33、读取字节 987,416 → 586,492，文件字节不变。
  损坏未访问的谓词块不影响该投影查询，但 `ReadAll()` 仍检测 checksum 错误。
  详细限制及复现见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-28：Dense FOR 直接写 Arrow 值缓冲区

- 在无 null、无 selection 的 FOR 解码中，直接填充一次分配的 Arrow-owned
  buffer；原 builder 仍处理 nullable 与 selected decode，检查和格式不变。
  8 字节窗口 bit-unpack 候选令 microbenchmark 变慢，已撤回。
- Apple M4 / Release，100,000 行 23-bit dense FOR microbenchmark 11 次
  P50 196 → 160 µs；500,000 行、50% 命中、仅投影一列的完整扫描
  1/8 线程 P50 0.681/1.20 → 0.590/1.06 ms，文件大小与读取量不变。
  详细基准与限制见 [`bench/BENCHMARK_V2.md`](../bench/BENCHMARK_V2.md)。

### 2026-09-28：Arrow 25 位解包 SIMD 可复用性调查

- Arrow 25 增加兼容 CPU 上的 SVE bit-unpack 动态分派；实际入口为
  `arrow::internal::unpack`（`bpacking_internal.h`），不是稳定公共 API。
  当前构建仅有 Arrow 23.0.1，安装的头文件中没有该入口，因此无法在同机
  对 Arrow 25 kernel 做可信 A/B，也不把其内部 ABI 接入生产 Reader。
- Sniffer FOR 使用连续、低位优先的 0–64 bit delta 流；Arrow 的
  `UnpackOptions` 有 bit width、bit offset 和批量大小，具备做独立实验的
  形态，但还需逐位宽验证实际兼容性、末尾边界以及解包后的 base 加法、
  delta 域检查成本。稀疏 selection/nullable 路径不能只按 dense unpack
  microbenchmark 判定收益；输出临时缓冲也必须计入内存和计时。
- 后续若单独批准 Arrow 25 依赖升级，在隔离构建中先跑 0–64 bit、空列、
  null、极值、截断输入的正确性/ASan，再按相同输入比较 Sniffer 当前完整
  FOR decode 与 Arrow unpack + base 重建的 P50/P95 和完整 scan；只有整体
  稳定获益且依赖边界可接受，才考虑生产路径。

参考：[Arrow 25 发布说明](https://arrow.apache.org/blog/2026/07/10/25.0.0-release/)、
[Arrow bit-unpack 变更](https://github.com/apache/arrow/pull/49756)、
[Arrow 位解包接口源码](https://github.com/apache/arrow/blob/main/cpp/src/arrow/util/bpacking_internal.h)。
