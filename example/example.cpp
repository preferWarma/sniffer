#include <arrow/api.h>
#include <arrow/pretty_print.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sniffer/io_plan.h"
#include "sniffer/segment_reader.h"
#include "sniffer/segment_writer.h"

namespace {

arrow::Result<std::shared_ptr<arrow::RecordBatch>> MakeBatch(const sniffer::TableSchema& schema) {
  ARROW_ASSIGN_OR_RAISE(auto arrow_schema, schema.ToArrowSchema());

  arrow::Int64Builder id_builder;
  arrow::StringBuilder category_builder;
  arrow::DoubleBuilder score_builder;
  for (int64_t id = 0; id < 12; ++id) {
    ARROW_RETURN_NOT_OK(id_builder.Append(id));
    ARROW_RETURN_NOT_OK(category_builder.Append(id % 2 == 0 ? "blue" : "green"));
    if (id % 4 == 0) {
      ARROW_RETURN_NOT_OK(score_builder.AppendNull());
    } else {
      ARROW_RETURN_NOT_OK(score_builder.Append(static_cast<double>(id) * 1.5));
    }
  }

  std::vector<std::shared_ptr<arrow::Array>> columns(3);
  ARROW_RETURN_NOT_OK(id_builder.Finish(&columns[0]));
  ARROW_RETURN_NOT_OK(category_builder.Finish(&columns[1]));
  ARROW_RETURN_NOT_OK(score_builder.Finish(&columns[2]));
  auto batch = arrow::RecordBatch::Make(std::move(arrow_schema), 12, std::move(columns));
  ARROW_RETURN_NOT_OK(batch->ValidateFull());
  return batch;
}

arrow::Result<int> RunExample(const std::string& path) {
  sniffer::TableSchema schema{
      1,
      {{1, "id", arrow::int64(), false, nullptr},
       {2, "category", arrow::utf8(), false, nullptr},
       {3, "score", arrow::float64(), true, nullptr}},
  };
  ARROW_ASSIGN_OR_RAISE(auto batch, MakeBatch(schema));

  sniffer::LayoutPolicy layout;
  layout.target_row_group_rows = 4;
  layout.sort_key_field_ids = {1};
  layout.statistics_field_ids = {1, 3};
  layout.bloom_field_ids = {2};

  ARROW_ASSIGN_OR_RAISE(auto writer, sniffer::SegmentWriter::Open(path, schema, layout));
  ARROW_RETURN_NOT_OK(writer->Append(batch));
  ARROW_RETURN_NOT_OK(writer->Finish());

  ARROW_ASSIGN_OR_RAISE(auto reader, sniffer::SegmentReader::Open(path));
  ARROW_RETURN_NOT_OK(reader->VerifyFileChecksum());

  sniffer::IOPlan plan;
  plan.projection_field_ids = {1, 2, 3};
  plan.conjunctive_predicates = {
      {2, sniffer::Predicate::Op::kEq, std::make_shared<arrow::StringScalar>("blue")}};
  sniffer::SortKeyRange range;
  range.lower =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(4)};
  range.upper =
      std::vector<std::shared_ptr<arrow::Scalar>>{std::make_shared<arrow::Int64Scalar>(10)};
  plan.sort_key_range = std::move(range);
  plan.limit = 3;
  plan.output_batch_rows = 2;

  auto metrics = std::make_shared<sniffer::ScanMetrics>();
  ARROW_ASSIGN_OR_RAISE(auto batches, reader->Scan(std::move(plan), metrics));
  uint64_t output_rows = 0;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto output, batches.Next());
    if (!output) {
      break;
    }
    output_rows += static_cast<uint64_t>(output->num_rows());
    ARROW_RETURN_NOT_OK(arrow::PrettyPrint(*output, {}, &std::cout));
  }

  std::cout << "segment=" << path << " output_rows=" << output_rows
            << " row_groups=" << reader->num_row_groups()
            << " pruned=" << metrics->row_groups_pruned
            << " chunks_read=" << metrics->column_chunks_read
            << " bytes_read=" << metrics->chunk_bytes_read << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "sniffer-example.seg";
  auto result = RunExample(path);
  if (!result.ok()) {
    std::cerr << result.status().ToString() << '\n';
    return 1;
  }
  return *result;
}
