// CLI11 entry point (REQ-005/TASK-014). Thin by design: argument parsing and
// exit-code mapping only. All real behavior lives in rdf2parquet_lib so it
// stays testable without spawning a process (see tests/test_rdf_parser.cpp,
// test_write_long.cpp, test_write_wide.cpp); tests/test_cli.cpp covers this
// file's own responsibility — argv-to-exit-code plumbing — end to end.

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "rdf2parquet/parquet_read.hpp"
#include "rdf2parquet/parquet_write.hpp"
#include "rdf2parquet/rdf_parser.hpp"

using namespace rdf2parquet;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) throw std::runtime_error("Could not open input file: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool hasRdfExtension(const std::string& path) {
  if (path.size() < 4) return false;
  std::string ext = path.substr(path.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".rdf";
}

// REQ-006: 1 = usage/CLI error (bad extension, missing file, bad flag value);
// 2 = malformed RDF/Parquet content, raised as RdfParseError further down and
// caught once in main().
int cmdConvert(const std::string& inPath, const std::string& outPath, bool wide,
               const std::string& compressionStr) {
  if (!hasRdfExtension(inPath)) {
    std::cerr << "Error: input file must have a .rdf extension: " << inPath << "\n";
    return 1;
  }
  if (!std::filesystem::exists(inPath)) {
    std::cerr << "Error: input file does not exist: " << inPath << "\n";
    return 1;
  }

  Compression compression;
  if (compressionStr == "zstd") {
    compression = Compression::kZstd;
  } else if (compressionStr == "snappy") {
    compression = Compression::kSnappy;
  } else if (compressionStr == "none") {
    compression = Compression::kNone;
  } else {
    std::cerr << "Error: --compression must be one of zstd|snappy|none, got: " << compressionStr
               << "\n";
    return 1;
  }

  RdfFile rdf = parseRdf(readFile(inPath));

  if (wide) {
    writeWideParquet(rdf, outPath, compression);
  } else {
    writeLongParquet(rdf, outPath, compression);
  }
  return 0;
}

int cmdInfo(const std::string& path) {
  printParquetInfo(path, std::cout);
  return 0;
}

int cmdHead(const std::string& path, int64_t n) {
  printParquetHead(path, n, std::cout);
  return 0;
}

int cmdToCsv(const std::string& path, const std::string& outPath) {
  writeParquetToCsv(path, outPath);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"rdf2parquet - convert RiverWare RDF files to Apache Parquet, "
               "and inspect Parquet files"};
  app.set_version_flag("--version", std::string("rdf2parquet ") + RDF2PARQUET_VERSION);
  app.require_subcommand(1);

  auto* convert = app.add_subcommand("convert", "Convert an RDF file to Parquet");
  std::string convertIn, convertOut, compressionStr = "zstd";
  bool wide = false;
  convert->add_option("input", convertIn, "Input .rdf file")->required();
  convert->add_option("output", convertOut, "Output .parquet file, or directory with --wide")
      ->required();
  convert->add_flag("--wide", wide, "Write the wide (trace-columns) layout to a directory");
  convert->add_option("--compression", compressionStr, "zstd|snappy|none")->default_val("zstd");

  auto* info = app.add_subcommand("info", "Print schema, row count, and metadata for a Parquet file");
  std::string infoPath;
  info->add_option("file", infoPath, "Parquet file")->required();

  auto* head = app.add_subcommand("head", "Print the first N rows of a Parquet file (tab-separated)");
  std::string headPath;
  int64_t headN = 10;
  head->add_option("file", headPath, "Parquet file")->required();
  head->add_option("-n", headN, "Number of rows")->default_val(10);

  auto* toCsv = app.add_subcommand("to-csv", "Export a Parquet file to CSV");
  std::string toCsvIn, toCsvOut;
  toCsv->add_option("file", toCsvIn, "Parquet file")->required();
  toCsv->add_option("output", toCsvOut, "Output .csv file")->required();

  CLI11_PARSE(app, argc, argv);

  try {
    if (*convert) return cmdConvert(convertIn, convertOut, wide, compressionStr);
    if (*info) return cmdInfo(infoPath);
    if (*head) return cmdHead(headPath, headN);
    if (*toCsv) return cmdToCsv(toCsvIn, toCsvOut);
  } catch (const RdfParseError& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }

  return 1;
}
