# AGENTS.md — Sniffer Core 实现规范

本文件约束所有参与 `sniffer-core` 的编码 Agent。开始工作前必须通读同目录的 `DESIGN.md`；当设计文档与临时任务描述冲突时，以用户的最新明确指令为准，并在变更说明中列出冲突与取舍。

## 1. 组件使命与边界

`sniffer-core` 是一个通用、不可变、Arrow-native 的列式 Segment 库。

必须做到：

- 接收 Arrow `RecordBatch` 并写出自描述 Segment。
- 接受 `IOPlan`，使用索引剪枝、列投影和谓词下推，返回 Arrow batch 流。
- 对字段、索引与编码保持通用和可配置。

绝对不得默认加入：

- RocksDB、WAL、LSM、Compaction、MVCC、事务、SQL parser、RPC、对象存储。
- 任何业务字段名或业务语义：禁止硬编码 `uid`、`ts_ms`、`gid`、user/item/session 等。
- 未经明确任务授权的完整 nested type、schema migration 框架或 SIMD 重写。

若上层需要这些能力，应通过独立 adapter 或上层模块实现，不得污染 core API。

## 2. 开始任务前的必做项

1. 阅读 `DESIGN.md` 中与当前阶段有关的范围和验收标准。
2. 检查现有目录、构建方式和测试，再提出最小改动方案。
3. 将任务映射到一个阶段：
   - 阶段一：格式骨架与 Arrow round-trip；
   - 阶段二：索引与 IOPlan scan；
   - 阶段三：编码选择、性能与稳健性。
4. 不得跳过前一阶段的验收门槛来实现后一阶段功能。
5. 发现二进制格式语义不完整时，先在 `docs/decisions/` 新增简短 decision record，或向用户提出一个明确问题；不得擅自编造兼容性语义。

## 3. API 与数据建模规则

- 所有落盘列以稳定 `field_id` 标识；名称不能是文件内部唯一身份。
- 公共输入输出优先使用 Arrow C++ 的 `Schema`、`Array` 与 `RecordBatch`。
- `IOPlan` v0.1 只允许：投影、AND 谓词、排序键范围、limit、batch size。
- filter 可以引用不在 projection 中的字段。
- 只要一个字段未配置统计或 Bloom，Reader 必须安全降级，不能拒绝查询。
- 不允许隐式类型转换；计划中的常量必须在执行前校验可与字段类型比较。
- 任何新增 encoding/index 都必须有显式 ID、版本兼容策略和“不支持时失败”的读路径。

## 4. 文件格式规则

- 所有整数和 offset 使用显式 little-endian 编解码，offset/size 采用 `uint64_t`。
- 所有 offset、length、count 在使用前进行边界检查和溢出检查。
- Header、Footer 与每个 ColumnChunk 都需要 checksum；任何校验失败必须返回结构化错误。
- `Finish()` 成功后 Segment 不可修改；不实现 in-place update。
- 元数据中必须持久化 schema、row-group 目录、编码描述符、索引偏移和 format version。
- 不得依赖 C++ struct 的内存布局直接落盘。
- 不得通过 reinterpret-cast 解析不可信输入；使用边界检查过的 reader/writer 工具函数。

## 5. 实现顺序

严格按以下顺序推进，每完成一项就补齐测试：

1. 基础类型、nullable、Header/Footer、LayoutIndex、Plain codec。
2. 多 Row Group writer/reader 与 Arrow round-trip。
3. StatisticsIndex、SortKeyIndex、简单 `IOPlan` 和 selection vector。
4. BloomFilter 与“只读取候选 ColumnChunk”的观测测试。
5. Dictionary、RLE、FOR + Bitpack。
6. 确定性编码选择器、benchmark、fuzz/property tests。

不要在 Plain codec 尚未通过 round-trip 时写自适应编码选择器；不要在过滤路径未正确时进行 SIMD 优化。

## 6. Reader 执行规则

对每个 `IOPlan` 固定遵循以下执行顺序：

1. 校验 Header、Footer、目录和索引。
2. 用 sort-key、统计和 Bloom 剪枝 Row Group。
3. 仅解码谓词列，形成 selection vector。
4. 仅为命中行解码 projection 中其余列。
5. 按目标 batch 大小输出 Arrow `RecordBatch`，再应用 limit。

禁止“为了实现简单”先解压整文件/全部列，再做过滤。若暂未实现某索引，允许顺序扫描，但必须在代码和测试中显式标记该降级路径。

## 7. 正确性与测试要求

每个功能改动必须同时提交或更新测试。最少包含：

- round-trip：值、行序、null bitmap、schema 与字段 ID 均一致；
- 多 Row Group；
- 空 batch、全 null、极值、重复值、随机字符串、binary；
- 每一种 encoding；
- 过滤和范围查询与“全量解码 + Arrow 参考过滤”结果逐字段一致；
- metadata 截断、错误 offset、未知 encoding、checksum 错误、版本不兼容；
- 已实现索引的剪枝观测：确认不命中的列块不被读取或解码。

优先使用 property-based test 或随机批次生成器。所有发现的 bug 都要先补最小复现测试，再修实现。

## 8. 性能与内存规则

- 不复制 Arrow buffer，除非编码/解码本身不可避免；任何复制要在代码注释中说明原因。
- Reader 的峰值内存应受 `output_batch_rows`、当前 Row Group 和投影列约束，不能随整个文件大小线性增长。
- 每个优化都需要 benchmark 对照；禁止基于单次 wall-clock 结果声称性能提升。
- benchmark 报告必须携带数据分布、row-group 大小、选择率、硬件、编译模式和原始指标。
- 性能优化不得改变文件确定性、谓词语义或错误处理。

## 9. 代码质量与交付规则

- C++ API 使用 RAII 和明确所有权；不引入裸拥有指针。
- 所有外部输入返回 `Status`/`Result` 类错误，不使用异常跨公共库边界。
- 保持 codec、format、index、scan、Arrow adapter 分目录，避免循环依赖。
- 每次提交前运行格式化、单元测试和与改动相关的 benchmark/fuzz smoke test。
- 完成任务时报告：改动范围、通过的测试、未实现或降级的路径、与 DESIGN 的任何偏离。
- 未经用户明确批准，不添加大型新依赖、网络服务、遥测、自动上传或破坏性命令。

## 10. 阶段验收门槛

### 阶段一完成条件

- 所有首期平铺类型和 nullable round-trip 正确。
- Header/Footer/offset/checksum 的损坏输入安全失败。
- 无 RocksDB、业务 schema 或执行引擎依赖。

### 阶段二完成条件

- projection、AND predicates、sort-key range、limit 与参考执行结果一致。
- Filter 与 projection 分离解码；索引可用时产生可观测剪枝。

### 阶段三完成条件

- Dictionary、RLE、FOR + Bitpack 的正确性和 fuzz 测试通过。
- 编码选择器可复现，benchmark 可重复运行并完整记录。
- 未为追求性能破坏格式版本、错误处理或 API 稳定性。
