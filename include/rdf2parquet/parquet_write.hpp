#pragma once

#include <string>

#include "rdf2parquet/rdf_model.hpp"

namespace rdf2parquet {

enum class Compression { kZstd, kSnappy, kNone };

// Writes the REQ-002/REQ-003 long-format table: one row per (run, slot,
// timestep) with columns trace/object/slot/timestep/units/scale/value, plus
// file-level metadata carrying the package and run preambles.
void writeLongParquet(const RdfFile& file, const std::string& outPath, Compression compression);

// Writes the REQ-004 wide-format directory layout: one
// `<object>.<slot>.parquet` file per series slot (columns: timestep,
// trace_<id>...), and one `scalars.parquet` holding every scalar slot in the
// long-format scalar row shape. Throws RdfParseError (see rdf_parser.hpp) on
// duplicate trace ids, filename collisions, or timestep mismatches across
// runs for the same slot.
void writeWideParquet(const RdfFile& file, const std::string& outDir, Compression compression);

}  // namespace rdf2parquet
