// TASK-010: write the fixture, read it back through parquet::arrow, and
// assert schema/order/type, row count, spot-checked values, and metadata.

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

fs::path tempOutputPath(const std::string& name) {
  auto dir = fs::temp_directory_path() / "rdf2parquet_tests";
  fs::create_directories(dir);
  return dir / name;
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

}  // namespace

TEST_CASE("long writer: schema, row count, metadata round trip", "[writer][long]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto outPath = tempOutputPath("long_traces.parquet").string();

  writeLongParquet(rdf, outPath, Compression::kZstd);

  auto table = readTable(outPath);
  auto schema = table->schema();

  REQUIRE(schema->num_fields() == 7);
  CHECK(schema->field(0)->name() == "trace");
  CHECK(schema->field(1)->name() == "object");
  CHECK(schema->field(2)->name() == "slot");
  CHECK(schema->field(3)->name() == "timestep");
  CHECK(schema->field(4)->name() == "units");
  CHECK(schema->field(5)->name() == "scale");
  CHECK(schema->field(6)->name() == "value");

  // 3 runs * (3 series slots * 5 timesteps + 2 scalar slots) = 51 rows.
  CHECK(table->num_rows() == 3 * (3 * 5 + 2));

  auto metadata = schema->metadata();
  REQUIRE(metadata != nullptr);

  auto versionIdx = metadata->FindKey("rdf2parquet.version");
  REQUIRE(versionIdx >= 0);
  CHECK_FALSE(metadata->value(versionIdx).empty());

  auto packageIdx = metadata->FindKey("rdf.package_preamble");
  REQUIRE(packageIdx >= 0);
  std::string packageJson = metadata->value(packageIdx);
  REQUIRE_FALSE(packageJson.empty());
  CHECK(packageJson.front() == '{');
  CHECK(packageJson.back() == '}');
  CHECK(packageJson.find(R"("number_of_runs":"3")") != std::string::npos);

  auto runsIdx = metadata->FindKey("rdf.run_preambles");
  REQUIRE(runsIdx >= 0);
  std::string runsJson = metadata->value(runsIdx);
  REQUIRE_FALSE(runsJson.empty());
  CHECK(runsJson.front() == '[');
  CHECK(runsJson.back() == ']');
  CHECK(runsJson.find(R"("trace":"1")") != std::string::npos);
}

TEST_CASE("long writer: value spot check", "[writer][long]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_traces.rdf"));
  auto outPath = tempOutputPath("long_traces_values.parquet").string();
  writeLongParquet(rdf, outPath, Compression::kSnappy);

  auto table = readTable(outPath);
  auto combined = table->CombineChunks();
  REQUIRE(combined.ok());
  auto t = *combined;

  auto traceCol = std::static_pointer_cast<arrow::Int32Array>(t->column(0)->chunk(0));
  auto valueCol = std::static_pointer_cast<arrow::DoubleArray>(t->column(6)->chunk(0));

  // Row 0: run 1 (trace_id 1), first slot in file order (Pool Elevation),
  // first timestep -> 1100.0 per the fixture.
  CHECK(traceCol->Value(0) == 1);
  CHECK(valueCol->Value(0) == Approx(1100.0));

  // Row 15: run 1's first scalar slot (Trace Historical Year) -> 1988.0.
  CHECK(valueCol->Value(15) == Approx(1988.0));
}

TEST_CASE("long writer: scale column and compression=none both round-trip", "[writer][long]") {
  auto rdf = parseRdf(readFile(fixturesDir() + "/sample_subset.rdf"));
  auto outPath = tempOutputPath("long_subset_none.parquet").string();
  writeLongParquet(rdf, outPath, Compression::kNone);

  auto table = readTable(outPath);
  auto combined = table->CombineChunks();
  REQUIRE(combined.ok());
  auto scaleCol = std::static_pointer_cast<arrow::DoubleArray>((*combined)->column(5)->chunk(0));
  CHECK(scaleCol->Value(0) == Approx(1.0));
}
