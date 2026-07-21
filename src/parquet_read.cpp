// Parquet read side (REQ-005): info/head/to-csv operate on ANY Parquet file,
// not only files this tool produced — unlike the writers, this code makes no
// assumption about which columns exist or what types they are, beyond
// rejecting nested (list/struct/map) columns for head/to-csv.

#include "rdf2parquet/parquet_read.hpp"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/types.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "rdf2parquet/rdf_parser.hpp"  // RdfParseError, reused for exit-2 read errors

namespace rdf2parquet {

namespace {

void checkStatus(const arrow::Status& status, const std::string& context) {
  if (!status.ok()) throw RdfParseError(context + ": " + status.ToString(), 0);
}

std::unique_ptr<parquet::arrow::FileReader> openReader(const std::string& path) {
  auto maybeInfile = arrow::io::ReadableFile::Open(path);
  checkStatus(maybeInfile.status(), "open " + path);

  auto maybeReader = parquet::arrow::OpenFile(*maybeInfile, arrow::default_memory_pool());
  checkStatus(maybeReader.status(), "open " + path + " as Parquet");
  return std::move(maybeReader).ValueOrDie();
}

bool isNested(const std::shared_ptr<arrow::DataType>& type) {
  switch (type->id()) {
    case arrow::Type::LIST:
    case arrow::Type::LARGE_LIST:
    case arrow::Type::FIXED_SIZE_LIST:
    case arrow::Type::STRUCT:
    case arrow::Type::MAP:
      return true;
    default:
      return false;
  }
}

// Throws RdfParseError naming every nested column, if any (REQ-005).
void requireFlatSchema(const arrow::Schema& schema) {
  std::string offending;
  for (const auto& field : schema.fields()) {
    if (isNested(field->type())) {
      if (!offending.empty()) offending += ", ";
      offending += field->name();
    }
  }
  if (!offending.empty()) {
    throw RdfParseError("Nested column(s) not supported by head/to-csv: " + offending, 0);
  }
}

std::string compressionToString(parquet::Compression::type c) {
  switch (c) {
    case parquet::Compression::UNCOMPRESSED: return "UNCOMPRESSED";
    case parquet::Compression::SNAPPY: return "SNAPPY";
    case parquet::Compression::GZIP: return "GZIP";
    case parquet::Compression::BROTLI: return "BROTLI";
    case parquet::Compression::ZSTD: return "ZSTD";
    case parquet::Compression::LZ4: return "LZ4";
    case parquet::Compression::LZO: return "LZO";
    case parquet::Compression::BZ2: return "BZ2";
    case parquet::Compression::LZ4_FRAME: return "LZ4_FRAME";
    default: return "UNKNOWN";
  }
}

// Decodes one cell to its display string. Nulls -> empty. Dictionary columns
// are decoded via their fixed int32 index (this tool's own writers always
// use StringDictionary32Builder); everything else falls back to Arrow's
// generic Scalar formatting so arbitrary flat Parquet files still work.
std::string cellToString(const std::shared_ptr<arrow::Array>& col, int64_t row) {
  if (col->IsNull(row)) return "";

  if (col->type_id() == arrow::Type::DICTIONARY) {
    auto dictArr = std::static_pointer_cast<arrow::DictionaryArray>(col);
    auto indices = dictArr->indices();
    if (indices->IsNull(row)) return "";
    auto dictionary = dictArr->dictionary();
    int64_t idx;
    switch (indices->type_id()) {
      case arrow::Type::INT8: idx = std::static_pointer_cast<arrow::Int8Array>(indices)->Value(row); break;
      case arrow::Type::INT16: idx = std::static_pointer_cast<arrow::Int16Array>(indices)->Value(row); break;
      case arrow::Type::INT32: idx = std::static_pointer_cast<arrow::Int32Array>(indices)->Value(row); break;
      case arrow::Type::INT64: idx = std::static_pointer_cast<arrow::Int64Array>(indices)->Value(row); break;
      default: return "";
    }
    if (dictionary->type_id() == arrow::Type::STRING || dictionary->type_id() == arrow::Type::LARGE_STRING) {
      return std::static_pointer_cast<arrow::StringArray>(dictionary)->GetString(idx);
    }
    auto maybeScalar = dictionary->GetScalar(idx);
    if (!maybeScalar.ok()) return "";
    return (*maybeScalar)->ToString();
  }

  auto maybeScalar = col->GetScalar(row);
  if (!maybeScalar.ok() || !(*maybeScalar)->is_valid) return "";
  return (*maybeScalar)->ToString();
}

std::shared_ptr<arrow::Table> readWholeTable(parquet::arrow::FileReader& reader) {
  auto maybeTable = reader.ReadTable();
  checkStatus(maybeTable.status(), "read table");
  auto combined = (*maybeTable)->CombineChunks();
  checkStatus(combined.status(), "combine chunks");
  return *combined;
}

std::string csvQuote(const std::string& field) {
  bool needsQuote =
      field.find_first_of(",\"\n\r") != std::string::npos;
  if (!needsQuote) return field;
  std::string out = "\"";
  for (char c : field) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
  return out;
}

}  // namespace

void printParquetInfo(const std::string& path, std::ostream& out) {
  auto reader = openReader(path);
  auto fileMeta = reader->parquet_reader()->metadata();

  std::error_code ec;
  auto fileSize = std::filesystem::file_size(path, ec);

  out << "file          : " << path << "\n";
  if (!ec) out << "size          : " << fileSize << " bytes\n";
  out << "rows          : " << fileMeta->num_rows() << "\n";
  out << "row groups    : " << fileMeta->num_row_groups() << "\n";

  std::shared_ptr<arrow::Schema> arrowSchema;
  checkStatus(reader->GetSchema(&arrowSchema), "read Arrow schema");

  auto schemaDescr = fileMeta->schema();
  out << "columns       :\n";
  for (int i = 0; i < schemaDescr->num_columns(); ++i) {
    const auto* col = schemaDescr->Column(i);
    out << "  " << col->name() << "  physical=" << parquet::TypeToString(col->physical_type())
        << "  logical=" << col->logical_type()->ToString();
    if (i < arrowSchema->num_fields()) {
      out << "  arrow=" << arrowSchema->field(i)->type()->ToString();
    }
    out << "\n";
  }

  out << "row group sizes (rows, compression per column):\n";
  for (int rg = 0; rg < fileMeta->num_row_groups(); ++rg) {
    auto rgMeta = fileMeta->RowGroup(rg);
    out << "  [" << rg << "] rows=" << rgMeta->num_rows()
        << " bytes=" << rgMeta->total_byte_size() << "  compression=";
    for (int c = 0; c < rgMeta->num_columns(); ++c) {
      if (c > 0) out << ",";
      out << compressionToString(rgMeta->ColumnChunk(c)->compression());
    }
    out << "\n";
  }

  auto kv = fileMeta->key_value_metadata();
  out << "metadata      :\n";
  if (kv) {
    for (int i = 0; i < kv->size(); ++i) {
      out << "  " << kv->key(i) << " = " << kv->value(i) << "\n";
    }
  }
}

void printParquetHead(const std::string& path, int64_t n, std::ostream& out) {
  auto reader = openReader(path);
  std::shared_ptr<arrow::Schema> schema;
  checkStatus(reader->GetSchema(&schema), "read Arrow schema");
  requireFlatSchema(*schema);

  auto table = readWholeTable(*reader);

  for (int i = 0; i < schema->num_fields(); ++i) {
    if (i > 0) out << "\t";
    out << schema->field(i)->name();
  }
  out << "\n";

  int64_t rows = std::min<int64_t>(n, table->num_rows());
  for (int64_t r = 0; r < rows; ++r) {
    for (int c = 0; c < table->num_columns(); ++c) {
      if (c > 0) out << "\t";
      out << cellToString(table->column(c)->chunk(0), r);
    }
    out << "\n";
  }
}

void writeParquetToCsv(const std::string& path, const std::string& outPath) {
  auto reader = openReader(path);
  std::shared_ptr<arrow::Schema> schema;
  checkStatus(reader->GetSchema(&schema), "read Arrow schema");
  requireFlatSchema(*schema);

  auto table = readWholeTable(*reader);

  std::ofstream out(outPath, std::ios::binary);
  if (!out.is_open()) {
    throw RdfParseError("Could not open output CSV file '" + outPath + "'", 0);
  }

  for (int i = 0; i < schema->num_fields(); ++i) {
    if (i > 0) out << ",";
    out << csvQuote(schema->field(i)->name());
  }
  out << "\r\n";

  for (int64_t r = 0; r < table->num_rows(); ++r) {
    for (int c = 0; c < table->num_columns(); ++c) {
      if (c > 0) out << ",";
      out << csvQuote(cellToString(table->column(c)->chunk(0), r));
    }
    out << "\r\n";
  }
}

}  // namespace rdf2parquet
