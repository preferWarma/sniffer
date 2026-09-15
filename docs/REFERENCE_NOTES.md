# Sniffer Core 参考资料阅读笔记

**状态：** 实现参考，不是格式规范或 decision record  
**阅读日期：** 2026-09-15  
**设计基线：** [`DESIGN.md`](./DESIGN.md)

本文只提炼与 `sniffer-core` 当前三阶段交付直接相关的公开资料。若本文与
`DESIGN.md` 或 `docs/decisions/` 冲突，以后两者为准。

## 1. ByteHouse / Sniffer 的公开描述

来源：Yuxing Han et al., [ByteHouse: ByteDance's Cloud-Native Data Warehouse for
Real-Time Multimodal Data Analytics](https://arxiv.org/abs/2602.08226), SIGMOD
Companion 2026，重点阅读 §3.2 与 Figure 3。

论文公开描述的 Sniffer 文件由三个区域构成：

- Data Region：`RecordGroup -> ColumnPartition -> DataBlock`。`DataBlock` 是压缩和
  类型相关的最小物理单元。
- Descriptor Region：包含 Layout Index、Sort-Key Descriptor、Column Statistics、
  Bloom Filter 与 Schema Descriptor。
- Footer Region：保存 descriptor offset、文件版本和 CRC，并作为定位与完整性校验入口。

与当前设计一致的核心思想是：数据、索引和 schema 元数据共同构成自描述文件；Reader
先通过 footer/descriptor 定位候选块，再读取相关 DataBlock。论文还描述了按类型和分布
采样选择 FOR + Bitpack、RLE、Dictionary、FSST、ALP 等编码。

需要严格隔离的上层能力包括 stable/delta segment、MVCC、WAL、staging KV、compaction、
事务、缓存和对象存储。这些属于 ByteHouse 存储系统，而不是 `sniffer-core` 文件库。
论文中的向量 L&P 布局、FSST 和 ALP 也超出 v0.1 当前范围。

## 2. Apache Arrow：公共数据语义

来源：[Arrow Columnar Format](https://arrow.apache.org/docs/format/Columnar.html) 与
[Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)。

实现必须保留以下 Arrow 语义：

- validity bitmap 使用 LSB 编号；`1` 表示有效，`0` 表示 null。
- `null_count == 0` 时 validity buffer 可以不存在，Reader/Writer 都必须接受这种情况。
- null 槽位对应的 value bytes 未定义，正确性比较不能依赖被 validity mask 掉的内容。
- `string`/`binary` 的标准变长布局含 `length + 1` 个 offset；offset 必须单调不减，序列化时
  宜归一化为从 0 开始。null 槽位也可能对应非空 byte range。
- timestamp 是参数化类型，单位和时区都属于 schema 语义，不能只持久化一个 `int64` 物理类型。
- Arrow 推荐 8/64 字节对齐并优先采用 64 字节 padding；这是性能建议，不应被误当成
  Sniffer 文件格式语义，除非后续 decision record 明确规定。
- C Data Interface 的 buffer 生命周期由 producer 持有并通过 release callback 转移；若未来
  增加 C ABI adapter，应保持这一所有权模型，不能把存储 buffer 的裸指针暴露为拥有指针。

对阶段一的直接要求是：schema 参数、行序、null bitmap、空数组、变长 offset 和 field ID
都要做 round-trip；解析外部 offset 时必须在构造 Arrow Array 前完成边界和单调性校验。

## 3. Apache Parquet：文件组织、剪枝与兼容性

来源：

- [Parquet Concepts](https://parquet.apache.org/docs/concepts/)
- [Parquet File Format](https://parquet.apache.org/docs/file-format/)
- [Parquet Metadata](https://parquet.apache.org/docs/file-format/metadata/)
- [Parquet Page Index](https://parquet.apache.org/docs/file-format/pageindex/)
- [Parquet Bloom Filter](https://parquet.apache.org/docs/file-format/bloomfilter/)
- [Parquet format versions](https://parquet.apache.org/docs/file-format/versions/)
- [Parquet checksumming](https://parquet.apache.org/docs/file-format/data-pages/checksumming/)

可借鉴的组织原则：

- File 包含 Row Group；每个 Row Group 含每列一个连续 Column Chunk；Column Chunk 可再分 Page。
- footer 位于数据之后，支持单遍写入；Reader 先读 footer/metadata，再决定读取哪些列块。
- ColumnIndex 用值域定位候选页，OffsetIndex 用逻辑行位置定位其他列中的对应数据。索引放在
  row group 之外并由 offset/length 定位，可避免非选择性扫描承担额外 I/O。
- Bloom filter 只能给出“确定不包含”或“可能包含”，不能产生 false negative；适合统计 min/max
  无法有效剪枝的高基数等值查询。未配置 Bloom 的列必须退化为正常扫描。
- 局部 checksum 支持只读取少量块时独立验证。Sniffer 不需要复制 Parquet CRC32 的具体选择，
  但 checksum 算法、覆盖范围和校验顺序必须在自己的格式 decision 中固定。

兼容性方面，Parquet 区分“可忽略、只损失性能”的可选元数据与“缺少实现就无法解码”的新
encoding。Sniffer 应采用相同原则：未知可选索引可以安全忽略并降级；未知 encoding、物理类型
或不兼容 format version 必须明确失败。

## 4. BtrBlocks：编码选择与基准方法

来源：Maximilian Kuschewski et al., [BtrBlocks: Efficient Columnar Compression for
Data Lakes](https://doi.org/10.1145/3589263), PACMMOD/SIGMOD 2023。

BtrBlocks 的关键做法是先收集简单统计以排除明显不适用的编码，再对候选编码进行小样本实际
压缩并按结果选择；其级联压缩会递归地继续编码中间产物。论文默认对 64K-value block 取
10 段、每段 64 个相邻值，约 1% 样本，以兼顾全局分布和局部 run 特征。论文报告该设置约占
总压缩时间 1.2%，77% 的选择落在最优或距最优 2% 内，平均压缩结果距穷举最优约 3.3%。

这些数字只说明方法，不应直接成为 Sniffer v0.1 的格式或 API 常量。阶段三若采用采样选择器，
至少需要固定：

- block/sample 大小、样本位置生成方式和随机种子；
- 候选编码遍历顺序与平局规则；
- 估算目标是文件大小、解码成本还是二者的确定性加权；
- 不同类型和 null 分布下的可行性过滤条件；
- encoding ID、参数和失败兼容策略。

论文也说明单一 synthetic benchmark 容易错估真实压缩行为。阶段三 benchmark 应覆盖真实感
较强的偏斜、重复、结构化字符串、窄值域、浮点和 null 分布，并同时报告文件大小、压缩/解压
吞吐、端到端 scan、读取字节和成本；不能仅凭一次 wall-clock 或压缩比下结论。

## 5. Vortex：逻辑/物理分层与扩展性参考

来源：[Vortex concepts and file layout](https://docs.vortex.dev/concepts/file-format) 与
[Vortex file format specification](https://docs.vortex.dev/specs/file-format)。

Vortex 将 logical dtype、physical encoding、layout 和 file segment 分开，并用注册 ID 在读取时
解析组件。其默认布局按列、统计 zone、chunk、compressor 和 buffered segment 分层；尾部的
bounded postscript 保存 schema/layout/statistics/footer 的 locator，使 Reader 可从文件尾部开始
规划少量读取。

对 Sniffer 的启发是保持 schema、encoding、layout 和 scan 模块边界，并让所有落盘组件具有
稳定 ID。当前 v0.1 不需要复制 Vortex 的 layout tree、edition、WASM fallback 或复杂 registry；
显式 version、unknown-component failure 和预留扩展位已经足够。

## 6. RocksDB External Table：adapter 边界

来源：[RocksDB External Table (Experimental)](https://github.com/facebook/rocksdb/wiki/External-Table-(Experimental))。

External Table 是可配置的自定义 table format 接口，但受限于只读 ingest：仅 Put，不支持
tombstone、merge、range deletion 或 sequence number，也不允许正常 live write。它适合作为未来
独立 adapter 的实验接口，不应让 `sniffer-core` 获得 RocksDB key、sequence、compaction 或
ColumnFamily 语义。

## 7. 对三阶段实现的直接结论

### 阶段一

- 先固定可验证的二进制语义，再实现：magic/version、整数编码、offset/length、checksum
  算法与覆盖范围、schema/scalar 序列化、encoding ID。
- Plain round-trip 必须严格遵守 Arrow null、timestamp、string/binary offset 和类型参数语义。
- Footer/LayoutIndex 应允许 Reader 从少量尾部读取中定位所有必要区域，并对每个范围做
  overflow 与文件边界检查。

### 阶段二

- scan 先读 metadata/index，形成候选 Row Group；只读取谓词列并构造 selection vector，再读取
  projection 中其余列。
- SortKey/Statistics/Bloom 都是可选优化；缺失时保证结果相同，只允许性能退化。
- 测试除了比较结果，还要观测实际读取和解码的 ColumnChunk，证明剪枝不是“解码后丢弃”。

### 阶段三

- 编码选择必须确定性、可解释、可复现；采样参数和 tie-breaker 属于实现契约。
- 先实现单编码的正确性和兼容失败路径，再考虑级联、FSST、ALP 或 SIMD。
- benchmark 必须同时记录数据分布、row-group 大小、选择率、硬件、编译模式和原始指标。

## 8. 实现前仍需 decision record 的格式语义

现有公开材料不足以替 Sniffer Core 决定以下字节级语义；开始相应实现前应在
`docs/decisions/` 固定：

1. magic、Header/Footer 的精确字段、长度、对齐和 version 判定规则；
2. checksum 算法、端序、覆盖字节范围、嵌套校验顺序和错误分类；
3. schema、Arrow type 参数、field metadata 与 scalar 的序列化格式；
4. float 的 NaN、signed zero 与统计/谓词比较规则；string/binary 的排序规则；
5. composite sort key、null 排序和半开/闭区间的字节级比较语义；
6. all-null/empty Row Group 的 statistics 表示，以及 min/max 缺失的降级规则；
7. optional index 与 required encoding 的能力标记及 unknown-ID 行为；
8. 空 projection 的输出 schema 和 batch 行数表达方式。

## 9. 尚未取得的资料

`DESIGN.md` 所称用户提供的 **USDB: ByteDance's Large-Scale Storage Practice for
Next-Generation Recommendations** 未出现在当前仓库中，公开检索也未找到可核验全文。因此本文
没有把该论文的二手描述当作事实。当前 Sniffer 结论以可核验的 ByteHouse 2026 论文 §3.2 为准；
若后续提供 USDB PDF，应补读其 §4.6 并记录两篇公开描述之间的差异。
