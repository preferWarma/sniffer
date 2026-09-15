# sniffer-core

`sniffer-core` 是一个通用、不可变、Arrow-native 的列式 Segment C++ 库。它将 Apache
Arrow `RecordBatch` 写成自描述文件，并通过索引剪枝、列投影和谓词下推，以
`RecordBatchIterator` 的形式流式读取查询结果。

项目当前处于 **v0.1 / experimental** 阶段，已完成格式、索引扫描和基础编码三阶段实现。

## 当前能力

### 数据模型

- 使用稳定的 `field_id` 标识落盘字段，字段名不是文件内唯一身份。
- 支持 `bool`、有符号/无符号 8/16/32/64 位整数、`float`、`double`、`timestamp`、
  `string` 和 `binary`。
- 保留 Arrow nullable、行序、timestamp 单位和时区等语义。
- 支持一个 Segment 内的多个 Row Group。

### 文件格式与编码

- little-endian 整数和 `uint64_t` offset/length。
- 自描述 Header、Footer、schema、Row Group 目录、encoding descriptor 和索引位置。
- Header、Footer、ColumnChunk 和全文件 checksum。
- 所有外部 offset、length、count 均经过边界和溢出检查。
- 支持以下单列编码：

| 编码 | 支持类型 | 典型数据 |
| --- | --- | --- |
| Plain | 所有 v0.1 类型 | 高基数或无明显模式 |
| Dictionary | 整数、string、binary | 低基数重复值 |
| RLE | 整数、bool | 长连续重复值 |
| FOR + Bitpack | 整数、timestamp | 窄值域或递增值 |

Writer 默认根据固定样本和固定阈值确定性选择编码，也可以通过 `LayoutPolicy` 对字段强制指定
编码。相同输入与配置会生成相同文件内容。

### 索引与查询

- `SortKeyIndex`
- `StatisticsIndex`
- `BloomFilter`
- projection 与过滤字段分离
- AND 谓词：`= != < <= > >= IS NULL IS NOT NULL`
- 单键或复合排序键范围
- `limit` 和可配置输出 batch 大小
- selection vector 执行：先解码谓词列，再读取并解码命中行需要的 projection 列
- 未配置统计或 Bloom 时安全降级为顺序扫描
- `ScanMetrics` 可观测 Row Group 剪枝、ColumnChunk 读取和解码字节数

## 依赖

- C++20 编译器
- CMake 3.20+
- `pkg-config`
- Apache Arrow C++

macOS/Homebrew 环境可以使用：

```bash
brew install apache-arrow cmake pkg-config
```

其他平台只需确保 `pkg-config --cflags --libs arrow` 能找到 Arrow C++。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

可用构建选项：

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `SNIFFER_BUILD_TESTS` | `ON` | 构建核心测试 |
| `SNIFFER_BUILD_FUZZ_SMOKE` | `ON` | 构建并注册 standalone fuzz smoke test |
| `SNIFFER_BUILD_BENCHMARK` | `ON` | 构建 benchmark |
| `SNIFFER_BUILD_EXAMPLES` | `ON` | 构建示例程序 |

项目也可使用 AddressSanitizer 和 UndefinedBehaviorSanitizer：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## 运行示例

[`example/example.cpp`](example/example.cpp) 展示完整流程：

1. 构造 Arrow schema 和 `RecordBatch`；
2. 配置 Row Group、排序键、统计和 Bloom；
3. 写入并完成不可变 Segment；
4. 校验文件 checksum；
5. 使用 projection、AND predicate、sort-key range 和 limit 扫描；
6. 输出查询结果及剪枝指标。

```bash
cmake --build build -j
./build/sniffer_core_example ./example.seg
```

示例查询只选择 `category == "blue"` 且排序键位于 `[4, 10)` 的行，并将输出拆成最多两行的
Arrow batch。

## API 概览

写入：

```cpp
sniffer::LayoutPolicy layout;
layout.target_row_group_rows = 64 * 1024;
layout.sort_key_field_ids = {1};
layout.statistics_field_ids = {1, 3};
layout.bloom_field_ids = {2};

ARROW_ASSIGN_OR_RAISE(
    auto writer,
    sniffer::SegmentWriter::Open("data.seg", table_schema, layout));
ARROW_RETURN_NOT_OK(writer->Append(record_batch));
ARROW_RETURN_NOT_OK(writer->Finish());
```

扫描：

```cpp
sniffer::IOPlan plan;
plan.projection_field_ids = {1, 3};
plan.conjunctive_predicates = {
    {1, sniffer::Predicate::Op::kGe,
     std::make_shared<arrow::Int64Scalar>(100)}};
plan.output_batch_rows = 4096;

auto metrics = std::make_shared<sniffer::ScanMetrics>();
ARROW_ASSIGN_OR_RAISE(auto reader, sniffer::SegmentReader::Open("data.seg"));
ARROW_ASSIGN_OR_RAISE(auto batches, reader->Scan(std::move(plan), metrics));
```

公共头文件位于 [`include/sniffer`](include/sniffer)。完整、可运行代码以
[`example/example.cpp`](example/example.cpp) 为准。

## Benchmark 与 fuzz

运行可重复 benchmark：

```bash
./build/sniffer_core_benchmark \
  --rows=100000 \
  --iterations=5 \
  --row-group=4096
```

输出包含数据分布、Row Group 大小、选择率、构建模式、硬件线程数、文件大小、读取字节数、
剪枝数量和多次运行的中位耗时。

`tests/sniffer_core_fuzz.cc` 同时提供 `LLVMFuzzerTestOneInput` 入口和 CTest 使用的确定性
standalone smoke corpus，用于检查任意 Segment 输入的解析、checksum、完整读取和扫描路径。

## 设计与格式决策

- [总体设计](docs/DESIGN.md)
- [参考资料笔记](docs/REFERENCE_NOTES.md)
- [阶段一文件格式决策](docs/decisions/0001-phase-one-file-format.md)
- [阶段二索引与扫描格式决策](docs/decisions/0002-phase-two-index-and-scan-format.md)
- [阶段三编码与选择器决策](docs/decisions/0003-phase-three-encodings-and-selection.md)

## 当前边界

v0.1 暂不包含：

- nested Arrow 类型；
- schema migration 或完整 additive evolution；
- 多级编码链、通用压缩层、FSST、ALP 或 SIMD 专用解码；
- in-place update；
- SQL parser、执行引擎、RPC 或对象存储；
- WAL、LSM、MVCC、事务、compaction 或 RocksDB adapter。

这些能力应作为后续格式版本、独立 adapter 或上层模块实现，而不是引入当前 core API。
