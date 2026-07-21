#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace rdf2parquet {

// Prints file path, size, row count, per-column schema (name, physical and
// logical type), row-group count/sizes, per-column-chunk compression codec,
// and all file-level key-value metadata. Accepts any Parquet file, not only
// this tool's own output (REQ-005).
void printParquetInfo(const std::string& path, std::ostream& out);

// Prints the first `n` rows, tab-separated, to `out`. Dictionary columns are
// decoded to their string values; nulls render as empty fields. Throws
// RdfParseError (mapped to exit 2) naming the offending column(s) if the
// file has a nested (list/struct/map) column (REQ-005).
void printParquetHead(const std::string& path, int64_t n, std::ostream& out);

// Streams the full file to RFC-4180 CSV at `outPath` (fields containing a
// comma, quote, or newline are quoted). Same nested-column and null-handling
// rules as printParquetHead.
void writeParquetToCsv(const std::string& path, const std::string& outPath);

}  // namespace rdf2parquet
