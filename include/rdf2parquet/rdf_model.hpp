#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rdf2parquet {

// One column ("slot") of RiverWare output for a single run: either a series
// (one value per run timestep) or a scalar (one value for the whole run).
// `values` holds one entry per raw value token; a token that failed to parse
// as a finite number becomes an empty optional rather than dropping the row
// (REQ-001: per-value null coercion, a deliberate deviation from the Python/JS
// references, which fall back to keeping the whole list as raw strings).
struct RdfSlot {
  std::string object_type;
  std::string object_name;
  std::string slot_type;
  std::string slot_name;
  std::string units;
  std::string scale;  // raw RDF token, kept as text; NEVER applied to values (REQ-001)
  std::vector<std::optional<double>> values;
  // true = scalar slot (1 value), false = series slot (time_steps values),
  // nullopt = the value count matched neither and the slot was excluded from
  // output after a warning (REQ-006: warn-and-skip is not a structural error).
  std::optional<bool> is_scalar;
};

// One simulation run ("trace"): its preamble, ISO-date timesteps, and slots
// in file order.
struct RdfRun {
  std::map<std::string, std::string> preamble;
  // Preamble "trace" value when present and parseable as an integer,
  // otherwise the 1-based ordinal of this run within the file (REQ-002,
  // matches rdfParser.js DR-01 — preserves trace identity across files that
  // hold a subset of an ensemble, e.g. traces 27-53 of a larger run).
  int32_t trace_id = 0;
  std::vector<std::string> timesteps;  // ISO "YYYY-MM-DD"
  std::vector<RdfSlot> slots;
};

// A fully parsed RDF file: package-level preamble, one RdfRun per run, and
// any non-fatal warnings collected while parsing (e.g. REQ-006 skipped
// slots).
struct RdfFile {
  std::map<std::string, std::string> package_preamble;
  std::vector<RdfRun> runs;
  std::vector<std::string> warnings;
};

}  // namespace rdf2parquet
