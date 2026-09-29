#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>

#if defined(__APPLE__)
#include <fcntl.h>
#endif

namespace sniffer_bench {

inline arrow::Status WriteParquet(const std::filesystem::path& path,
                                  const arrow::RecordBatch& batch, uint32_t row_group_rows,
                                  parquet::Compression::type compression) {
  ARROW_ASSIGN_OR_RAISE(auto output, arrow::io::FileOutputStream::Open(path.string()));
  parquet::WriterProperties::Builder properties_builder;
  properties_builder.compression(compression);
  properties_builder.max_row_group_length(row_group_rows);
  parquet::ArrowWriterProperties::Builder arrow_properties_builder;
  arrow_properties_builder.set_use_threads(false);
  arrow_properties_builder.store_schema();
  ARROW_ASSIGN_OR_RAISE(
      auto writer, parquet::arrow::FileWriter::Open(*batch.schema(), arrow::default_memory_pool(),
                                                    output, properties_builder.build(),
                                                    arrow_properties_builder.build()));
  for (int64_t offset = 0; offset < batch.num_rows(); offset += row_group_rows) {
    const int64_t length = std::min<int64_t>(row_group_rows, batch.num_rows() - offset);
    ARROW_RETURN_NOT_OK(writer->NewBufferedRowGroup());
    ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(*batch.Slice(offset, length)));
  }
  ARROW_RETURN_NOT_OK(writer->Close());
  return output->Close();
}

inline arrow::Result<std::unique_ptr<parquet::arrow::FileReader>> OpenParquet(
    const std::filesystem::path& path, int64_t batch_rows, bool bypass_os_cache = false) {
  parquet::arrow::FileReaderBuilder builder;
  if (bypass_os_cache) {
#if defined(__APPLE__)
    ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(path.string()));
    if (::fcntl(input->file_descriptor(), F_NOCACHE, 1) != 0) {
      return arrow::Status::IOError("[sniffer.bench.io] cannot disable Parquet file cache");
    }
    ARROW_RETURN_NOT_OK(builder.Open(input));
#else
    return arrow::Status::NotImplemented(
        "[sniffer.bench.io] OS file-cache bypass is unavailable on this platform");
#endif
  } else {
    ARROW_RETURN_NOT_OK(builder.OpenFile(path.string(), false));
  }
  parquet::ArrowReaderProperties properties(false);
  properties.set_batch_size(batch_rows);
  builder.properties(properties);
  return builder.Build();
}

}  // namespace sniffer_bench
