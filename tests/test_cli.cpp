// TASK-015: end-to-end CLI tests via std::system on the built binary (path
// supplied by CMake as RDF2PARQUET_CLI_PATH). Exercises argv parsing and
// exit-code mapping (REQ-006), which unit tests on the library can't reach.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string cliPath() { return RDF2PARQUET_CLI_PATH; }
std::string fixturesDir() { return RDF2PARQUET_FIXTURES_DIR; }

fs::path freshTempDir(const std::string& name) {
  auto dir = fs::temp_directory_path() / "rdf2parquet_cli_tests" / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

int runCli(const std::vector<std::string>& args) {
  std::ostringstream cmd;
  cmd << "\"" << cliPath() << "\"";
  for (const auto& a : args) cmd << " \"" << a << "\"";

#ifdef _WIN32
  // std::system runs `cmd /c <string>`, and cmd strips the first and last
  // quote of that string before parsing it. With more than one quoted token
  // that unbalances the rest, so wrap the whole line in a sacrificial pair.
  const std::string line = "\"" + cmd.str() + "\"";
  return std::system(line.c_str());
#else
  int ret = std::system(cmd.str().c_str());
  return WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
#endif
}

}  // namespace

TEST_CASE("cli: convert -> info -> head -> to-csv round trip", "[cli]") {
  auto dir = freshTempDir("roundtrip");
  auto rdfPath = fixturesDir() + "/sample_traces.rdf";
  auto parquetPath = (dir / "out.parquet").string();
  auto csvPath = (dir / "out.csv").string();

  CHECK(runCli({"convert", rdfPath, parquetPath}) == 0);
  REQUIRE(fs::exists(parquetPath));

  CHECK(runCli({"info", parquetPath}) == 0);
  CHECK(runCli({"head", parquetPath, "-n", "5"}) == 0);
  CHECK(runCli({"to-csv", parquetPath, csvPath}) == 0);
  REQUIRE(fs::exists(csvPath));

  std::ifstream csv(csvPath);
  std::string firstLine;
  std::getline(csv, firstLine);
  CHECK(firstLine.find("trace") != std::string::npos);
}

TEST_CASE("cli: --wide convert produces a directory of files", "[cli]") {
  auto dir = freshTempDir("wide");
  auto rdfPath = fixturesDir() + "/sample_traces.rdf";
  auto outDir = (dir / "wideout").string();

  CHECK(runCli({"convert", rdfPath, outDir, "--wide"}) == 0);
  CHECK(fs::exists(fs::path(outDir) / "scalars.parquet"));
}

TEST_CASE("cli: missing input file exits 1", "[cli][errors]") {
  auto dir = freshTempDir("missing");
  auto outPath = (dir / "out.parquet").string();
  CHECK(runCli({"convert", (dir / "does_not_exist.rdf").string(), outPath}) == 1);
}

TEST_CASE("cli: wrong extension exits 1", "[cli][errors]") {
  auto dir = freshTempDir("badext");
  auto notRdf = dir / "data.csv";
  {
    std::ofstream f(notRdf);
    f << "not rdf";
  }
  auto outPath = (dir / "out.parquet").string();
  CHECK(runCli({"convert", notRdf.string(), outPath}) == 1);
}

TEST_CASE("cli: malformed RDF exits 2", "[cli][errors]") {
  auto dir = freshTempDir("malformed");
  auto badRdf = dir / "bad.rdf";
  {
    std::ofstream f(badRdf);
    f << "not_a_valid_rdf_file\n";
  }
  auto outPath = (dir / "out.parquet").string();
  CHECK(runCli({"convert", badRdf.string(), outPath}) == 2);
}

TEST_CASE("cli: bad --compression flag value exits 1", "[cli][errors]") {
  auto dir = freshTempDir("badcompression");
  auto rdfPath = fixturesDir() + "/sample_traces.rdf";
  auto outPath = (dir / "out.parquet").string();
  CHECK(runCli({"convert", rdfPath, outPath, "--compression", "bogus"}) == 1);
}

TEST_CASE("cli: no subcommand exits nonzero", "[cli][errors]") {
  CHECK(runCli({}) != 0);
}
