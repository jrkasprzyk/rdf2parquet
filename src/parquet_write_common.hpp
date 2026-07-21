#pragma once

// Internal helpers shared by parquet_write_long.cpp and parquet_write_wide.cpp
// (the wide writer's scalars.parquet reuses the long format's scalar row
// fields minus `timestep`, per REQ-004). Not installed; not part of the
// public API.

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rdf2parquet/parquet_write.hpp"
#include "rdf2parquet/rdf_model.hpp"
#include "rdf2parquet/rdf_parser.hpp"  // RdfParseError, reused for write-time structural failures

namespace rdf2parquet::detail {

// Howard Hinnant's days_from_civil algorithm: constant-time proleptic
// Gregorian -> days-since-1970-01-01, matching Arrow's date32 epoch. Avoids
// pulling in a <chrono> calendar dependency for this C++17 codebase.
inline int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

inline int32_t isoDateToDays(const std::string& iso) {
  int y = std::stoi(iso.substr(0, 4));
  unsigned m = static_cast<unsigned>(std::stoi(iso.substr(5, 2)));
  unsigned d = static_cast<unsigned>(std::stoi(iso.substr(8, 2)));
  return daysFromCivil(y, m, d);
}

// Independent from the parser's safeFloat (GUD-001: writers know nothing
// about RDF text parsing) — the writer derives the nullable `scale` column
// straight from the raw token stored on RdfSlot.
inline std::optional<double> parseFiniteDouble(const std::string& token) {
  try {
    std::size_t idx = 0;
    double v = std::stod(token, &idx);
    if (idx != token.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

inline std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

inline std::string jsonObject(const std::map<std::string, std::string>& m) {
  std::string out = "{";
  bool first = true;
  for (const auto& kv : m) {
    if (!first) out += ",";
    first = false;
    out += "\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\"";
  }
  return out + "}";
}

inline std::string jsonArrayOfObjects(const std::vector<std::map<std::string, std::string>>& arr) {
  std::string out = "[";
  bool first = true;
  for (const auto& m : arr) {
    if (!first) out += ",";
    first = false;
    out += jsonObject(m);
  }
  return out + "]";
}

inline arrow::Compression::type toArrowCompression(Compression c) {
  switch (c) {
    case Compression::kZstd:
      return arrow::Compression::ZSTD;
    case Compression::kSnappy:
      return arrow::Compression::SNAPPY;
    case Compression::kNone:
      return arrow::Compression::UNCOMPRESSED;
  }
  return arrow::Compression::UNCOMPRESSED;
}

inline void checkStatus(const arrow::Status& status, const std::string& context) {
  if (!status.ok()) {
    throw RdfParseError(context + ": " + status.ToString(), 0);
  }
}

// Exact REQ-002 long-format schema: also the row shape of scalars.parquet in
// the wide layout (REQ-004), minus the `timestep` column staying null there.
inline std::shared_ptr<arrow::Schema> longSchema() {
  return arrow::schema({
      arrow::field("trace", arrow::int32(), /*nullable=*/false),
      arrow::field("object", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("slot", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("timestep", arrow::date32(), true),
      arrow::field("units", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("scale", arrow::float64(), true),
      arrow::field("value", arrow::float64(), true),
  });
}

// Accumulates rows for the long schema. `appendRow` covers all three shapes
// it is used for: series rows (days set, value set or null when a value
// token failed to parse), and scalar rows (days = nullopt).
struct LongRowBuilders {
  arrow::Int32Builder trace;
  arrow::StringDictionary32Builder object;
  arrow::StringDictionary32Builder slot;
  arrow::Date32Builder timestep;
  arrow::StringDictionary32Builder units;
  arrow::DoubleBuilder scale;
  arrow::DoubleBuilder value;

  void appendRow(int32_t traceId, const RdfSlot& s, std::optional<int32_t> days,
                 std::optional<double> v) {
    checkStatus(trace.Append(traceId), "append trace");
    checkStatus(object.Append(s.object_name), "append object");
    checkStatus(slot.Append(s.slot_name), "append slot");
    if (days) {
      checkStatus(timestep.Append(*days), "append timestep");
    } else {
      checkStatus(timestep.AppendNull(), "append timestep null");
    }
    checkStatus(units.Append(s.units), "append units");
    auto sc = parseFiniteDouble(s.scale);
    if (sc) {
      checkStatus(scale.Append(*sc), "append scale");
    } else {
      checkStatus(scale.AppendNull(), "append scale null");
    }
    if (v) {
      checkStatus(value.Append(*v), "append value");
    } else {
      checkStatus(value.AppendNull(), "append value null");
    }
  }

  std::shared_ptr<arrow::Table> finish(const std::shared_ptr<arrow::Schema>& schema) {
    std::shared_ptr<arrow::Array> traceArr, objectArr, slotArr, timestepArr, unitsArr, scaleArr,
        valueArr;
    checkStatus(trace.Finish(&traceArr), "finish trace");
    checkStatus(object.Finish(&objectArr), "finish object");
    checkStatus(slot.Finish(&slotArr), "finish slot");
    checkStatus(timestep.Finish(&timestepArr), "finish timestep");
    checkStatus(units.Finish(&unitsArr), "finish units");
    checkStatus(scale.Finish(&scaleArr), "finish scale");
    checkStatus(value.Finish(&valueArr), "finish value");
    return arrow::Table::Make(
        schema, {traceArr, objectArr, slotArr, timestepArr, unitsArr, scaleArr, valueArr});
  }
};

// scalars.parquet in the wide layout (REQ-004): trace/object/slot/units/scale
// /value, deliberately without a `timestep` column (scalars have none).
inline std::shared_ptr<arrow::Schema> scalarSchema() {
  return arrow::schema({
      arrow::field("trace", arrow::int32(), /*nullable=*/false),
      arrow::field("object", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("slot", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("units", arrow::dictionary(arrow::int32(), arrow::utf8()), false),
      arrow::field("scale", arrow::float64(), true),
      arrow::field("value", arrow::float64(), true),
  });
}

struct ScalarRowBuilders {
  arrow::Int32Builder trace;
  arrow::StringDictionary32Builder object;
  arrow::StringDictionary32Builder slot;
  arrow::StringDictionary32Builder units;
  arrow::DoubleBuilder scale;
  arrow::DoubleBuilder value;

  void appendRow(int32_t traceId, const RdfSlot& s, std::optional<double> v) {
    checkStatus(trace.Append(traceId), "append trace");
    checkStatus(object.Append(s.object_name), "append object");
    checkStatus(slot.Append(s.slot_name), "append slot");
    checkStatus(units.Append(s.units), "append units");
    auto sc = parseFiniteDouble(s.scale);
    if (sc) {
      checkStatus(scale.Append(*sc), "append scale");
    } else {
      checkStatus(scale.AppendNull(), "append scale null");
    }
    if (v) {
      checkStatus(value.Append(*v), "append value");
    } else {
      checkStatus(value.AppendNull(), "append value null");
    }
  }

  std::shared_ptr<arrow::Table> finish(const std::shared_ptr<arrow::Schema>& schema) {
    std::shared_ptr<arrow::Array> traceArr, objectArr, slotArr, unitsArr, scaleArr, valueArr;
    checkStatus(trace.Finish(&traceArr), "finish trace");
    checkStatus(object.Finish(&objectArr), "finish object");
    checkStatus(slot.Finish(&slotArr), "finish slot");
    checkStatus(units.Finish(&unitsArr), "finish units");
    checkStatus(scale.Finish(&scaleArr), "finish scale");
    checkStatus(value.Finish(&valueArr), "finish value");
    return arrow::Table::Make(schema, {traceArr, objectArr, slotArr, unitsArr, scaleArr, valueArr});
  }
};

// Row group size fixed at 1,000,000 (TASK-009) — comfortably larger than any
// realistic single-file ensemble run, so files stay single-row-group and
// scan overhead is minimized without needing a size-driven policy.
inline constexpr int64_t kRowGroupSize = 1'000'000;

inline void writeTableToFile(const std::shared_ptr<arrow::Table>& table,
                              const std::string& outPath, Compression compression) {
  auto writerProps =
      parquet::WriterProperties::Builder().compression(toArrowCompression(compression))->build();
  auto arrowProps = parquet::ArrowWriterProperties::Builder().store_schema()->build();

  auto maybeOutfile = arrow::io::FileOutputStream::Open(outPath);
  checkStatus(maybeOutfile.status(), "open output file " + outPath);
  auto outfile = *maybeOutfile;

  auto status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile,
                                            kRowGroupSize, writerProps, arrowProps);
  checkStatus(status, "write parquet file " + outPath);
  checkStatus(outfile->Close(), "close output file " + outPath);
}

}  // namespace rdf2parquet::detail
