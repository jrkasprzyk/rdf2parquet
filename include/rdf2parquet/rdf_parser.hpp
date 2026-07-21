#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "rdf2parquet/rdf_model.hpp"

namespace rdf2parquet {

// Thrown on structural failures mapped to exit code 2 by the CLI (REQ-006):
// parse-time errors (truncated file, missing END markers, invalid preamble
// counts, multi-column slots, missing number_of_runs, zero runs parsed) as
// well as write-time structural failures from the REQ-004 wide layout
// (duplicate trace ids, filename collisions, mismatched timesteps across
// runs). Carries the 1-based input line number nearest the problem, when one
// applies, so error messages can name it (REQ-006); pass 0 for write-time
// errors that have no associated source line.
class RdfParseError : public std::runtime_error {
 public:
  RdfParseError(const std::string& message, std::size_t line);

  std::size_t line() const noexcept { return line_; }

 private:
  std::size_t line_;
};

// Parse RDF file text into a fully in-memory RdfFile. Faithful to
// example_scripts/rdf_parser.py (PAT-001), except for the documented REQ-001
// deviations: per-value null coercion instead of whole-slot string fallback,
// and strict multi-column-slot detection (END_COLUMN must be followed by
// END_SLOT, else this throws — the references silently mis-parse this case).
// Also enforces SEC-001: counts read from preambles are validated against
// the remaining line count before any vector sized by them is allocated.
RdfFile parseRdf(std::string_view text);

}  // namespace rdf2parquet
