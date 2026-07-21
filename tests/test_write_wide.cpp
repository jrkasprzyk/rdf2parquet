// TASK-012: per-slot file naming, column counts, long/wide pivot equivalence
// for two sampled slots, scalars.parquet contents, and the two REQ-004 error
// paths (stale-file warning, filename collision).

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <parquet/arrow/reader.h>
#include <sstream>

#include "rdf2parquet/parquet_write.hpp"
#include "rdf2parquet/rdf_parser.hpp"

using namespace rdf2parquet;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string fixturesDir() { return RDF2PARQUET_FIXTURES_DIR; }

fs::path freshTempDir(const std::string& name) {
  auto dir = fs::temp_directory_path() / "rdf2parquet_tests" / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

std::shared_ptr<arrow::Table> readTable(const std::string& path) {
  auto maybeInfile = arrow::io::ReadableFile::Open(path);
  REQUIRE(maybeInfile.ok());
  auto infile = *maybeInfile;

  std::unique_ptr<parquet::arrow::FileReader> reader;
  REQUIRE(parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader).ok());

  std::shared_ptr<arrow::Table> table;
  REQUIRE(reader->ReadTable(&table).ok());
  return table;
}

std::vector<double> doubleColumn(const std::shared_ptr<arrow::Table>& table, int col) {
  auto combined = table->CombineChunks();
  REQUIRE(combined.ok());
  auto arr = std::static_pointer_cast<arrow::DoubleArray>((*combined)->column(col)->chunk(0));
  std::vector<double> out;
  for (int64_t i = 0; i < arr->length(); ++i) out.push_back(arr->Value(i));
  return out;
}

}  // namespace

TEST_CASE("wide writer: per-slot file naming and column counts", "[writer][wide]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto dir = freshTempDir("wide_naming");

  writeWideParquet(rdf, dir.string(), Compression::kZstd);

  // Spaces in object/slot names sanitize to '_' (REQ-004).
  CHECK(fs::exists(dir / "Example_Reservoir.Pool_Elevation.parquet"));
  CHECK(fs::exists(dir / "Example_Reservoir.Outflow.parquet"));
  CHECK(fs::exists(dir / "Example_Reservoir.Turbine_Release.parquet"));
  CHECK(fs::exists(dir / "scalars.parquet"));

  auto table = readTable((dir / "Example_Reservoir.Pool_Elevation.parquet").string());
  REQUIRE(table->num_columns() == 4);  // timestep + 3 trace columns (3 runs)
  CHECK(table->num_rows() == 5);
  CHECK(table->schema()->field(0)->name() == "timestep");
  CHECK(table->schema()->field(1)->name() == "trace_1");
  CHECK(table->schema()->field(2)->name() == "trace_2");
  CHECK(table->schema()->field(3)->name() == "trace_3");
}

TEST_CASE("wide writer: values match the long-format pivot for two sampled slots",
          "[writer][wide]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto dir = freshTempDir("wide_pivot");
  writeWideParquet(rdf, dir.string(), Compression::kZstd);

  auto pool = readTable((dir / "Example_Reservoir.Pool_Elevation.parquet").string());
  CHECK(doubleColumn(pool, 1) == std::vector<double>{1100.0, 1099.5, 1099.0, 1098.5, 1098.0});
  CHECK(doubleColumn(pool, 2) == std::vector<double>{1101.0, 1100.5, 1100.0, 1099.5, 1099.0});
  CHECK(doubleColumn(pool, 3) == std::vector<double>{1098.0, 1097.5, 1097.0, 1096.5, 1096.0});

  auto outflow = readTable((dir / "Example_Reservoir.Outflow.parquet").string());
  CHECK(doubleColumn(outflow, 1) == std::vector<double>{500.0, 510.0, 520.0, 530.0, 540.0});
  CHECK(doubleColumn(outflow, 2) == std::vector<double>{490.0, 500.0, 510.0, 520.0, 530.0});
  CHECK(doubleColumn(outflow, 3) == std::vector<double>{520.0, 530.0, 540.0, 550.0, 560.0});
}

TEST_CASE("wide writer: scalars.parquet holds every scalar slot for every run",
          "[writer][wide]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto dir = freshTempDir("wide_scalars");
  writeWideParquet(rdf, dir.string(), Compression::kZstd);

  auto table = readTable((dir / "scalars.parquet").string());
  REQUIRE(table->num_columns() == 6);  // trace, object, slot, units, scale, value
  CHECK(table->schema()->field(0)->name() == "trace");
  CHECK(table->schema()->field(5)->name() == "value");
  CHECK(table->num_rows() == 3 * 2);  // 3 runs * 2 scalar slots

  auto values = doubleColumn(table, 5);
  std::vector<double> expected = {1988.0, 95.0, 1995.0, 102.0, 2003.0, 88.5};
  CHECK(values == expected);
}

TEST_CASE("wide writer: pre-existing unrelated .parquet file triggers a warning, not deletion",
          "[writer][wide][errors]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto dir = freshTempDir("wide_stale");

  auto stalePath = dir / "leftover_from_before.parquet";
  { std::ofstream f(stalePath, std::ios::binary); f << "not a real parquet file"; }

  std::ostringstream captured;
  std::streambuf* origCerr = std::cerr.rdbuf(captured.rdbuf());
  writeWideParquet(rdf, dir.string(), Compression::kZstd);
  std::cerr.rdbuf(origCerr);

  CHECK(fs::exists(stalePath));  // never deleted
  CHECK(captured.str().find("leftover_from_before.parquet") != std::string::npos);
}

TEST_CASE("wide writer: filename collision between two slots is an error", "[writer][wide][errors]") {
  RdfFile rdf;
  rdf.package_preamble["number_of_runs"] = "1";

  RdfSlot a;
  a.object_type = "DataObj";
  a.object_name = "Object A";
  a.slot_type = "SeriesSlot";
  a.slot_name = "X";
  a.units = "feet";
  a.scale = "1";
  a.values = {1.0, 2.0};
  a.is_scalar = false;

  RdfSlot b = a;
  b.object_name = "Object_A";  // sanitizes to the same filename as "Object A"

  RdfRun run;
  run.trace_id = 1;
  run.timesteps = {"2024-01-01", "2024-01-02"};
  run.slots = {a, b};
  rdf.runs = {run};

  auto dir = freshTempDir("wide_collision");
  CHECK_THROWS_AS(writeWideParquet(rdf, dir.string(), Compression::kNone), RdfParseError);
}

TEST_CASE("wide writer: duplicate trace ids within a file is an error", "[writer][wide][errors]") {
  RdfFile rdf;
  rdf.package_preamble["number_of_runs"] = "2";

  RdfSlot slot;
  slot.object_name = "Object A";
  slot.slot_name = "X";
  slot.units = "feet";
  slot.scale = "1";
  slot.values = {1.0};
  slot.is_scalar = false;

  RdfRun run1;
  run1.trace_id = 1;
  run1.timesteps = {"2024-01-01"};
  run1.slots = {slot};

  RdfRun run2 = run1;  // same trace_id = 1 as run1

  rdf.runs = {run1, run2};

  auto dir = freshTempDir("wide_dup_trace");
  CHECK_THROWS_AS(writeWideParquet(rdf, dir.string(), Compression::kNone), RdfParseError);
}
