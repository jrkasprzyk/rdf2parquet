// Ports every case from example_scripts/test_rdf_parser.py (TASK-007),
// plus fixture-driven assertions on sample_traces.rdf.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <set>
#include <sstream>

#include "rdf2parquet/rdf_parser.hpp"

using namespace rdf2parquet;
using Catch::Approx;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string fixturesDir() { return RDF2PARQUET_FIXTURES_DIR; }

const RdfSlot* findSlot(const RdfRun& run, const std::string& object, const std::string& slot) {
  for (const auto& s : run.slots) {
    if (s.object_name == object && s.slot_name == slot) return &s;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("sample_traces.rdf: meta and run count", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  CHECK(rdf.package_preamble.at("number_of_runs") == "3");
  CHECK(rdf.runs.size() == 3);
}

TEST_CASE("sample_traces.rdf: slot count per run", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  for (const auto& run : rdf.runs) {
    CHECK(run.slots.size() == 5);
  }
}

TEST_CASE("sample_traces.rdf: slot names", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  std::set<std::string> keys;
  for (const auto& s : rdf.runs[0].slots) keys.insert(s.object_name + "." + s.slot_name);

  std::set<std::string> expected = {
      "Example Reservoir.Pool Elevation",
      "Example Reservoir.Outflow",
      "Example Reservoir.Turbine Release",
      "Run Management.Trace Historical Year",
      "Run Management.Historical Year Percent of Average",
  };
  CHECK(keys == expected);
}

TEST_CASE("sample_traces.rdf: series slot value count and scalar flag", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  for (const auto& run : rdf.runs) {
    const RdfSlot* slot = findSlot(run, "Example Reservoir", "Pool Elevation");
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_scalar.has_value());
    CHECK_FALSE(*slot->is_scalar);
    CHECK(slot->values.size() == 5);
  }
}

TEST_CASE("sample_traces.rdf: series slot values", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  const RdfSlot* slot = findSlot(rdf.runs[0], "Example Reservoir", "Pool Elevation");
  REQUIRE(slot != nullptr);
  REQUIRE(slot->values.front().has_value());
  REQUIRE(slot->values.back().has_value());
  CHECK(*slot->values.front() == Approx(1100.0));
  CHECK(*slot->values.back() == Approx(1098.0));
}

TEST_CASE("sample_traces.rdf: scalar slot", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  const RdfSlot* slot = findSlot(rdf.runs[0], "Run Management", "Trace Historical Year");
  REQUIRE(slot != nullptr);
  REQUIRE(slot->is_scalar.has_value());
  CHECK(*slot->is_scalar);
  REQUIRE(slot->values.size() == 1);
  REQUIRE(slot->values[0].has_value());
  CHECK(*slot->values[0] == Approx(1988.0));
}

TEST_CASE("sample_traces.rdf: units", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  const RdfSlot* slot = findSlot(rdf.runs[0], "Example Reservoir", "Pool Elevation");
  REQUIRE(slot != nullptr);
  CHECK(slot->units == "feet");
}

TEST_CASE("sample_traces.rdf: timestamp normalization", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  const auto& times = rdf.runs[0].timesteps;
  REQUIRE(times.size() == 5);
  CHECK(times.front() == "2024-01-01");
  CHECK(times.back() == "2024-01-05");
}

TEST_CASE("sample_traces.rdf: trace ids", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  REQUIRE(rdf.runs.size() == 3);
  CHECK(rdf.runs[0].trace_id == 1);
  CHECK(rdf.runs[1].trace_id == 2);
  CHECK(rdf.runs[2].trace_id == 3);
}

TEST_CASE("sample_subset.rdf: meta and run count", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_subset.rdf"));
  CHECK(rdf.package_preamble.at("number_of_runs") == "2");
  CHECK(rdf.runs.size() == 2);
}

TEST_CASE("sample_subset.rdf: timestep count", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_subset.rdf"));
  for (const auto& run : rdf.runs) CHECK(run.timesteps.size() == 4);
}

TEST_CASE("sample_subset.rdf: series value count", "[parser][fixtures]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_subset.rdf"));
  for (const auto& run : rdf.runs) {
    for (const auto& slot : run.slots) {
      if (slot.is_scalar.has_value() && !*slot.is_scalar) {
        CHECK(slot.values.size() == 4);
      }
    }
  }
}

// --- timestamp normalization (unit-level, not fixture-driven) --------------
// normalizeTimestamp() itself is not exported (it is a parser implementation
// detail); exercised indirectly through a minimal single-run, single-slot RDF
// so single-digit month/day zero-padding is verified end to end.

namespace {

std::string minimalRdf(const std::string& timestampLine) {
  std::ostringstream rdf;
  rdf << "number_of_runs:1\n"
      << "END_PACKAGE_PREAMBLE\n"
      << "trace:1\n"
      << "time_steps:1\n"
      << "END_RUN_PREAMBLE\n"
      << timestampLine << "\n"
      << "object_type: DataObj\n"
      << "object_name: Obj\n"
      << "slot_type: SeriesSlot\n"
      << "slot_name: Slot\n"
      << "END_SLOT_PREAMBLE\n"
      << "units: feet\n"
      << "scale: 1\n"
      << "1.0\n"
      << "END_COLUMN\n"
      << "END_SLOT\n"
      << "END_RUN\n";
  return rdf.str();
}

}  // namespace

TEST_CASE("timestamp normalization: single-digit month/day", "[parser][timestamp]") {
  CHECK(parseRdf(minimalRdf("2025-10-2 24:00")).runs[0].timesteps[0] == "2025-10-02");
  CHECK(parseRdf(minimalRdf("2026-1-15 24:00")).runs[0].timesteps[0] == "2026-01-15");
  CHECK(parseRdf(minimalRdf("2026-10-1 24:00")).runs[0].timesteps[0] == "2026-10-01");
}

// --- structural (malformed input) errors -----------------------------------

TEST_CASE("parser errors: missing number_of_runs", "[parser][errors]") {
  std::string rdf = "name:x\nEND_PACKAGE_PREAMBLE\n";
  CHECK_THROWS_AS(parseRdf(rdf), RdfParseError);
}

TEST_CASE("parser errors: zero runs parsed", "[parser][errors]") {
  std::string rdf = "number_of_runs:0\nEND_PACKAGE_PREAMBLE\n";
  CHECK_THROWS_AS(parseRdf(rdf), RdfParseError);
}

TEST_CASE("parser errors: truncated file missing END_PACKAGE_PREAMBLE", "[parser][errors]") {
  std::string rdf = "name:x\nnumber_of_runs:1\n";
  CHECK_THROWS_AS(parseRdf(rdf), RdfParseError);
}

TEST_CASE("parser errors: truncated file missing END_RUN", "[parser][errors]") {
  std::string rdf =
      "number_of_runs:1\nEND_PACKAGE_PREAMBLE\ntrace:1\ntime_steps:0\nEND_RUN_PREAMBLE\n";
  CHECK_THROWS_AS(parseRdf(rdf), RdfParseError);
}

TEST_CASE("parser errors: multi-column slot (missing END_SLOT after END_COLUMN)",
          "[parser][errors]") {
  std::string rdf =
      "number_of_runs:1\n"
      "END_PACKAGE_PREAMBLE\n"
      "trace:1\n"
      "time_steps:1\n"
      "END_RUN_PREAMBLE\n"
      "2024-1-1 24:00\n"
      "object_type: DataObj\n"
      "object_name: Obj\n"
      "slot_type: SeriesSlot\n"
      "slot_name: Slot\n"
      "END_SLOT_PREAMBLE\n"
      "units: feet\n"
      "scale: 1\n"
      "1.0\n"
      "END_COLUMN\n"
      "2.0\n"  // second value column instead of END_SLOT
      "END_SLOT\n"
      "END_RUN\n";
  CHECK_THROWS_AS(parseRdf(rdf), RdfParseError);
}

TEST_CASE("parser errors: non-finite value becomes null, not a parse error", "[parser][errors]") {
  std::string rdf = minimalRdf("2024-1-1 24:00");
  // Replace the "1.0" value line with a non-numeric token.
  auto idx = rdf.find("1.0\nEND_COLUMN");
  REQUIRE(idx != std::string::npos);
  rdf.replace(idx, 3, "NaN");

  auto parsed = parseRdf(rdf);
  REQUIRE(parsed.runs[0].slots.size() == 1);
  const auto& values = parsed.runs[0].slots[0].values;
  REQUIRE(values.size() == 1);
  CHECK_FALSE(values[0].has_value());
}

TEST_CASE("parser: wrong value count warns and skips the slot, not a structural error",
          "[parser][errors]") {
  std::string rdf =
      "number_of_runs:1\n"
      "END_PACKAGE_PREAMBLE\n"
      "trace:1\n"
      "time_steps:2\n"
      "END_RUN_PREAMBLE\n"
      "2024-1-1 24:00\n"
      "2024-1-2 24:00\n"
      "object_type: DataObj\n"
      "object_name: Obj\n"
      "slot_type: \n"
      "slot_name: Slot\n"
      "END_SLOT_PREAMBLE\n"
      "units: feet\n"
      "scale: 1\n"
      "1.0\n"
      "2.0\n"
      "3.0\n"  // 3 values, but nts=2 and not 1 -> neither series nor scalar
      "END_COLUMN\n"
      "END_SLOT\n"
      "END_RUN\n";
  auto parsed = parseRdf(rdf);
  REQUIRE(parsed.runs[0].slots.size() == 1);
  CHECK_FALSE(parsed.runs[0].slots[0].is_scalar.has_value());
  CHECK(parsed.runs[0].slots[0].values.empty());
  CHECK(parsed.warnings.size() == 1);
}
