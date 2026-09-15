# Sniffer Core：通用 Arrow-native Immutable Columnar Segment

**状态：** v0.1 设计基线  
**目标：** 复刻 Sniffer 的公开设计思想，构建独立、通用、可嵌入的列式 Segment 组件；不追求与任何内部 Sniffer 文件字节兼容。

## 1. 组件边界

Sniffer Core 是一个不可变列式文件库。它接收 Apache Arrow `RecordBatch`，写出自描述的 Segment 文件；读取时根据 `IOPlan` 完成索引剪枝、列投影与谓词下推，并返回 Arrow `RecordBatch` 流。

```text
Arrow RecordBatch
       │
       ▼
SegmentWriter ──> immutable Sniffer Segment
                         │
                         ▼
                    SegmentReader
                         │
                       IOPlan
                         ▼
            Arrow RecordBatchIterator
```

### 1.1 目标

- 通用 schema：不硬编码 `uid`、`ts_ms`、`gid` 或任何业务字段。
- Arrow-native：以 Arrow C++ 的 `Schema`、`Array`、`RecordBatch` 作为公共数据接口。
- 自描述：文件包含 schema、布局、编码、索引、版本与校验信息。
- 面向扫描：支持投影、简单谓词、排序键范围与限制条数。
- 面向嵌入：提供 C++ 库，不绑定 RocksDB、DuckDB、对象存储或 RPC。

### 1.2 非目标

- 不实现 RocksDB `TableFactory`、WAL、MVCC、事务、删除或 Compaction。
- 不实现 SQL 解析、优化器或完整表达式执行器。
- 不实现分布式文件管理、缓存与复制。
- v0.1 不要求完整 Arrow nested 类型、通用 schema evolution 或全量 SIMD 优化。
- 不声称兼容 USDB/ByteHouse 的内部 Sniffer 二进制格式。

## 2. 通用数据模型

### 2.1 Schema

每个字段必须具有稳定的 `field_id`；名称只用于显示和用户侧计划构造。Segment 内以 `field_id` 绑定列，避免重命名造成歧义。

```cpp
struct FieldSpec {
  uint32_t field_id;
  std::string name;
  std::shared_ptr<arrow::DataType> type;
  bool nullable;
  std::optional<arrow::Scalar> default_value;
};

struct TableSchema {
  uint32_t schema_version;
  std::vector<FieldSpec> fields;
};
```

v0.1 支持：`bool`、有符号/无符号整数、`float/double`、`timestamp`、`string`、`binary` 及其 nullable 形式。`struct/list/map` 留到后续版本。

### 2.2 布局与索引策略

```cpp
struct LayoutPolicy {
  uint32_t target_row_group_rows;
  std::vector<uint32_t> sort_key_field_ids;  // 可为空
  std::vector<uint32_t> statistics_field_ids;
  std::vector<uint32_t> bloom_field_ids;
};
```

- **Row Group**：行的水平分区；是索引剪枝和扫描调度的最小单位。
- **Chunked Layout**：一个 Row Group 内某字段的一段压缩列块。
- **SortKeyIndex**：记录每个 Row Group 的排序键边界，用于范围剪枝。
- **StatisticsIndex**：每个受支持字段的 `min/max/null_count`，用于比较谓词剪枝。
- **BloomFilter**：可选的等值过滤加速结构。
- **LayoutIndex**：字段、Row Group 与物理列块偏移和长度的目录。

未配置 sort key 时，Segment 仍可扫描；只是不能进行排序键范围剪枝。Bloom 与统计信息均为按字段可选能力，而非业务主键约束。

### 2.3 IOPlan

v0.1 只定义可稳定下推的最小计划：

```cpp
struct Predicate {
  enum class Op { kEq, kNe, kLt, kLe, kGt, kGe, kIsNull, kIsNotNull };
  uint32_t field_id;
  Op op;
  std::optional<arrow::Scalar> value;
};

struct SortKeyRange {
  std::optional<std::vector<arrow::Scalar>> lower;
  std::optional<std::vector<arrow::Scalar>> upper;
  bool lower_inclusive = true;
  bool upper_inclusive = false;
};

struct IOPlan {
  std::vector<uint32_t> projection_field_ids;
  std::vector<Predicate> conjunctive_predicates;  // 仅 AND
  std::optional<SortKeyRange> sort_key_range;
  std::optional<uint64_t> limit;
  uint32_t output_batch_rows;
};
```

复杂布尔表达式、函数、join 和 aggregate 不属于 Sniffer Core；它们由上层执行引擎拆解后传入可下推的部分。

## 3. 文件格式

```text
Header
  magic | format_version | header_size | flags
Data Region
  RowGroup[0]
    ColumnChunk[field 0] ... ColumnChunk[field N]
  RowGroup[1] ...
Index Region
  LayoutIndex | SortKeyIndex | StatisticsIndex | BloomFilter
Footer
  TableSchema | encoding descriptors | index offsets | checksum
```

### 3.1 约束

- 所有整数、偏移和长度显式规定字节序；v0.1 统一使用 little-endian。
- 所有文件偏移采用 `uint64_t`，避免大文件溢出。
- 文件、footer 与每个 ColumnChunk 都需要校验和；读取端必须验证元数据边界。
- 每个 ColumnChunk 必须携带：`field_id`、物理类型、编码 ID、行数、null 数、压缩前/后字节数。
- writer 只能追加完整 Row Group；`Finish()` 后文件不可修改。
- 读取未知字段、未知编码或不支持的 format version 时必须返回明确错误，绝不猜测解码。

## 4. 扫描执行模型

对一个 `IOPlan`，Reader 必须按以下顺序工作：

1. 读取并校验 Header、Footer 和 Index Region。
2. 使用 SortKeyIndex、StatisticsIndex 与 BloomFilter 找出候选 Row Group。
3. 仅解码谓词涉及的列，计算 selection vector。
4. 对命中行再解码 projection 中尚未读取的列。
5. 按 `output_batch_rows` 生成 Arrow `RecordBatch`，并遵守 `limit`。

此顺序是组件的核心性能契约：不能先解压全部列、再在内存中过滤。

## 5. 编码策略

v0.1 每个 ColumnChunk 选择一种编码；编码选择可由采样得到，但必须可复现并写入元数据。

| 编码 | 首期适用类型 | 适用数据 |
| --- | --- | --- |
| Plain | 所有首期类型 | 高基数或无明显模式数据 |
| Dictionary | string、binary、整数 | 低基数重复值 |
| RLE | 整数、bool、dictionary index | 长连续重复值 |
| FOR + Bitpack | 整数、timestamp | 值域窄或递增/接近值 |

首期选择器使用固定、可解释的阈值和样本大小。多级编码链、FSST、ALP、SIMD 专用解码器属于第三阶段后的独立优化，不应阻塞格式正确性。

## 6. 三阶段交付

### 阶段一：格式骨架与 Arrow Round-trip

**范围**

- `TableSchema`、LayoutPolicy、Header/Footer、LayoutIndex。
- `SegmentWriter::Append/Finish` 与 `SegmentReader::Open`。
- 平铺基础类型、nullable、Plain 编码；允许 Dictionary 作为可选加分项。
- 完整 Arrow round-trip 与文件损坏检测。

**验收**

- 任意受支持的 Arrow `RecordBatch` 写入、读取后与参考 batch 逐字段相等。
- 多个 Row Group 的行序和 null 语义保持一致。
- 破坏 magic、offset、checksum、编码 ID 时读取稳定失败且无越界读取。
- `Sniffer Core` 不依赖 RocksDB、DuckDB 或业务 schema。

### 阶段二：索引与 IOPlan Scan

**范围**

- SortKeyIndex、StatisticsIndex、BloomFilter。
- `projection + AND predicates + sort-key range + limit`。
- selection vector 执行路径；过滤列与输出列解码分离。
- 输出 `RecordBatchIterator`，支持跨 Row Group 分批返回。

**验收**

- 每个 IOPlan 的结果与“全量解码后由 Arrow 参考实现过滤”的结果一致。
- 测试覆盖等值、范围、null、空结果、边界值、不同投影顺序和 limit。
- 统计信息或 Bloom 表明不命中时，测试可证明相关数据 ColumnChunk 未被读取。
- 无 sort key、无统计列、无 Bloom 的 Segment 能正确降级为顺序扫描。

### 阶段三：编码选择、性能与稳健性

**范围**

- Dictionary、RLE、FOR + Bitpack 完整实现与解码。
- 基于确定性采样的编码选择器。
- 基准、内存上限、fuzz/property testing 与 profiling。
- 为未来 nested 类型、schema additive evolution 与新编码预留版本/扩展位。

**验收**

- 所有编码在随机、偏斜、重复、递增和含 null 数据下通过 round-trip 与 IOPlan 语义测试。
- 对有选择性的查询，读取字节与解码列数明显低于全列全量扫描；报告方法和原始指标。
- Writer 对同一输入和配置产生确定性文件，或在文件内显式记录影响差异的随机种子。
- libFuzzer/AFL++ 或等价 fuzz 目标在格式解析、footer、各编码器上运行且无内存安全错误。

## 7. 正确性与性能基线

### 7.1 必须保持的语义

- 输入行序在没有排序重写的 v0.1 中保持不变。
- nullable 的 validity bitmap 语义与 Arrow 完全一致。
- 谓词只对非 null 值做普通比较；`IS NULL` 与 `IS NOT NULL` 单独定义。
- `projection` 可为空或与谓词列不重合；Reader 仍须正确执行过滤。
- `limit` 作用于过滤后的逻辑行序。

### 7.2 基准维度

- 数据：高基数、低基数、长 RLE、窄值域整数、变长字符串、含 null。
- Row Group：1K、8K、64K、256K 行。
- 查询：全列扫描、单列投影、低/中/高选择率谓词、排序键范围、Bloom 等值。
- 指标：文件大小、写入吞吐、解码吞吐、端到端 scan 吞吐、读取字节、峰值内存。

性能结果必须同时给出绝对值、机器配置、数据分布和 baseline；不得只报告压缩比或只报告单列解码速度。

## 8. 后续演进方向

- 完整 Arrow nested 类型与递归布局。
- schema additive evolution：新增字段可由默认值或 null 补齐。
- 更多编码器与可插拔 cost model。
- SIMD/vectorized predicate kernel。
- Page 级而非仅 Row Group 级的统计与跳过。
- 可选 RocksDB/对象存储/DuckDB adapter；这些属于独立模块，不能污染 `sniffer-core`。

## 9. 参考论文与文档

本设计复刻的是公开可见的思想与能力边界，而非任何内部实现或二进制协议。以下材料按其对设计决策的作用分组；实现前应优先阅读“必读”条目。

### 9.1 必读：Sniffer 目标与公共数据接口

1. **USDB: ByteDance's Large-Scale Storage Practice for Next-Generation Recommendations**，用户提供的论文，§4.6 “Sniffer: Hybrid Columnar Encoding”。  
   **对应本设计：** `TableScheme`、Data/Index Region、Layout/SortKey/Statistics/Bloom 索引，以及“同一格式同时服务点查和列扫描”的目标。该论文只作为设计灵感来源；它没有提供可用于字节兼容复刻的公开格式规范。

2. [Apache Arrow Columnar Format Specification](https://arrow.apache.org/docs/format/Columnar.html)。  
   **对应本设计：** `Schema`、nullable validity bitmap、primitive/variable binary/nested 类型的内存语义，以及 `RecordBatch` 作为公开输入输出。Sniffer Core 必须服从 Arrow 的逻辑值与 null 语义，而不是自行定义另一套列向量表示。

3. [Apache Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)。  
   **对应本设计：** 未来向 DuckDB、Velox 或其他原生执行引擎暴露零拷贝 C/C++ adapter 时的 ABI 边界。v0.1 可以仅依赖 Arrow C++，但不得设计与该接口冲突的所有权模型。

### 9.2 文件组织、元数据与跳过读取

4. [Apache Parquet File Format](https://parquet.apache.org/docs/file-format/)、[Concepts](https://parquet.apache.org/docs/concepts/)、[Metadata](https://parquet.apache.org/docs/file-format/metadata/)。  
   **对应本设计：** Row Group、Column Chunk、Footer 目录、显式 offset/size、文件版本与元数据校验。Sniffer Core 借鉴其组织原则，但不要求 Parquet 二进制兼容。

5. [Parquet Page Index](https://parquet.apache.org/docs/file-format/pageindex/) 与 [Bloom Filter](https://parquet.apache.org/docs/file-format/bloomfilter/)。  
   **对应本设计：** `StatisticsIndex`、`BloomFilter` 与“先判定候选块、后读数据列”的扫描顺序。Sniffer v0.1 的索引粒度为 Row Group；未来可演进到 Page 级。

### 9.3 编码与压缩策略

6. Maximilian Kuschewski, David Sauerwein, Adnan Alhomssi, and Viktor Leis. [BtrBlocks: Efficient Columnar Compression for Data Lakes](https://dl.acm.org/doi/10.1145/3589263). *Proceedings of the ACM on Management of Data*, 2023.  
   **对应本设计：** 针对列块数据特征选择轻量编码/编码链的思路，以及将压缩率、解码吞吐和选择开销一起评估的方法。v0.1 只实现可解释的单编码选择，不直接复刻其完整编码链。

7. [Vortex](https://github.com/vortex-data/vortex)。  
   **对应本设计：** 可扩展列压缩格式的工程化开源参考，尤其是 codec 注册、逻辑类型与物理 encoding 分离、以及 benchmark 组织方式。它是 Rust 项目，不是 `sniffer-core` 的直接依赖或格式兼容目标。

### 9.4 Sniffer 的公开延伸与未来集成边界

8. [ByteHouse: ByteDance's Cloud-Native Data Warehouse for Real-Time Multimodal Data Analytics](https://arxiv.org/abs/2602.08226)。  
   **对应本设计：** Sniffer 在更通用 schema、稳定列式段、索引与执行计划下推中的公开描述。它可帮助校验抽象是否足以承接 IOPlan，但不扩展 v0.1 的 core 范围。

9. [RocksDB External Table (Experimental)](https://github.com/facebook/rocksdb/wiki/External-Table-%28Experimental%29)。  
   **对应本设计：** 后续做 RocksDB adapter 时的接口与限制参考。该机制面向只读 ingest，且不支持 tombstone、merge、sequence number 或在线写；不得将这些限制反向引入纯 `sniffer-core`。

### 9.5 阅读顺序与兼容性原则

建议按 **1 → 2 → 4 → 5 → 6** 的顺序阅读；第 7、8、9 项用于实现和集成阶段的横向参考。

除非未来单独立项，Sniffer Core 的格式规范以本设计及其后续 decision records 为准。参考 Parquet、BtrBlocks、Vortex、USDB 或 ByteHouse 都不意味着读取端应接受它们的文件，也不意味着写入端产出与其字节兼容的文件。
