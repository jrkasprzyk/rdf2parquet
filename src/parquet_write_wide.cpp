// Wide-format writer (REQ-004): one `<object>.<slot>.parquet` file per series
// slot (timestep + trace_<id> columns) plus a single scalars.parquet. Unlike
// the long writer, this validates cross-run structural invariants the wide
// layout depends on (identical timesteps per slot, unique trace ids, no
// filename collisions) — real ensembles violating these need an explicit
// error rather than a silently misaligned table (RISK-003).

#include "rdf2parquet/parquet_write.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "parquet_write_common.hpp"

namespace rdf2parquet {

namespace {

using namespace detail;
namespace fs = std::filesystem;

std::string sanitizeFilenamePart(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    out += ok ? c : '_';
  }
  return out;
}

// All occurrences (across runs) of one series slot, keyed by trace id so
// output columns come out in ascending trace-id order regardless of file
// order.
struct SeriesGroup {
  std::string object_name;
  std::string slot_name;
  std::vector<std::string> referenceTimesteps;  // from the first run that has this slot
  std::map<int32_t, std::vector<std::optional<double>>> byTrace;
};

void writeSeriesSlotFile(const SeriesGroup& g, const std::string& outPath,
                          Compression compression) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.push_back(arrow::field("timestep", arrow::date32(), /*nullable=*/false));
  for (const auto& [traceId, vals] : g.byTrace) {
    (void)vals;
    fields.push_back(arrow::field("trace_" + std::to_string(traceId), arrow::float64(), true));
  }
  auto schema = arrow::schema(fields);

  arrow::Date32Builder tsBuilder;
  for (const auto& t : g.referenceTimesteps) {
    checkStatus(tsBuilder.Append(isoDateToDays(t)), "append timestep");
  }
  std::shared_ptr<arrow::Array> tsArr;
  checkStatus(tsBuilder.Finish(&tsArr), "finish timestep");

  std::vector<std::shared_ptr<arrow::Array>> columns = {tsArr};
  for (const auto& [traceId, vals] : g.byTrace) {
    (void)traceId;
    arrow::DoubleBuilder db;
    for (std::size_t i = 0; i < g.referenceTimesteps.size(); ++i) {
      if (i < vals.size() && vals[i].has_value()) {
        checkStatus(db.Append(*vals[i]), "append value");
      } else {
        checkStatus(db.AppendNull(), "append value null");
      }
    }
    std::shared_ptr<arrow::Array> arr;
    checkStatus(db.Finish(&arr), "finish value");
    columns.push_back(arr);
  }

  writeTableToFile(arrow::Table::Make(schema, columns), outPath, compression);
}

void writeScalarsFile(const std::vector<std::pair<const RdfRun*, const RdfSlot*>>& scalars,
                       const std::string& outPath, Compression compression) {
  ScalarRowBuilders builders;
  for (const auto& [run, slot] : scalars) {
    std::optional<double> v;
    if (!slot->values.empty()) v = slot->values[0];
    builders.appendRow(run->trace_id, *slot, v);
  }
  writeTableToFile(builders.finish(scalarSchema()), outPath, compression);
}

}  // namespace

void writeWideParquet(const RdfFile& file, const std::string& outDir, Compression compression) {
  // File-level (REQ-004): trace ids must be unique across all runs.
  {
    std::map<int32_t, int> counts;
    for (const auto& run : file.runs) counts[run.trace_id]++;
    std::string dupMsg;
    for (const auto& [id, count] : counts) {
      if (count > 1) {
        if (!dupMsg.empty()) dupMsg += ", ";
        dupMsg += std::to_string(id);
      }
    }
    if (!dupMsg.empty()) {
      throw RdfParseError("Duplicate trace id(s) within file: " + dupMsg, 0);
    }
  }

  std::vector<std::string> seriesOrder;
  std::map<std::string, SeriesGroup> series;
  std::vector<std::pair<const RdfRun*, const RdfSlot*>> scalars;

  for (const auto& run : file.runs) {
    for (const auto& slot : run.slots) {
      if (!slot.is_scalar.has_value()) continue;  // REQ-006: warned-and-skipped slot
      if (*slot.is_scalar) {
        scalars.emplace_back(&run, &slot);
        continue;
      }

      std::string key = slot.object_name + "." + slot.slot_name;
      auto it = series.find(key);
      if (it == series.end()) {
        SeriesGroup g;
        g.object_name = slot.object_name;
        g.slot_name = slot.slot_name;
        g.referenceTimesteps = run.timesteps;
        it = series.emplace(key, std::move(g)).first;
        seriesOrder.push_back(key);
      } else if (it->second.referenceTimesteps != run.timesteps) {
        throw RdfParseError("Slot '" + key +
                                 "': timesteps differ across runs (trace " +
                                 std::to_string(run.trace_id) +
                                 "); wide mode requires identical timesteps for every run.",
                             0);
      }
      // Guarded redundantly to the file-level check above, in case a future
      // caller feeds runs with pre-validated-elsewhere duplicate ids.
      if (it->second.byTrace.count(run.trace_id)) {
        throw RdfParseError(
            "Duplicate trace id " + std::to_string(run.trace_id) + " for slot '" + key + "'.", 0);
      }
      it->second.byTrace[run.trace_id] = slot.values;
    }
  }

  // Filenames + collision detection (REQ-004).
  std::map<std::string, std::string> keyToFilename;
  std::map<std::string, std::vector<std::string>> filenameToKeys;
  for (const auto& key : seriesOrder) {
    const auto& g = series.at(key);
    std::string filename =
        sanitizeFilenamePart(g.object_name) + "." + sanitizeFilenamePart(g.slot_name) + ".parquet";
    keyToFilename[key] = filename;
    filenameToKeys[filename].push_back(key);
  }
  for (const auto& [filename, keys] : filenameToKeys) {
    if (keys.size() > 1) {
      std::string msg = "Filename collision '" + filename + "' between slots: ";
      for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i) msg += ", ";
        msg += keys[i];
      }
      throw RdfParseError(msg, 0);
    }
  }

  std::error_code ec;
  fs::create_directories(outDir, ec);
  if (ec) {
    throw RdfParseError("Could not create output directory '" + outDir + "': " + ec.message(), 0);
  }

  std::set<std::string> produced;
  for (const auto& [key, filename] : keyToFilename) {
    (void)key;
    produced.insert(filename);
  }
  if (!scalars.empty()) produced.insert("scalars.parquet");

  // Stale-file warning (REQ-004): pre-existing .parquet files this run will
  // not (re)write are reported, never deleted.
  std::vector<std::string> stale;
  for (const auto& entry : fs::directory_iterator(outDir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".parquet") continue;
    std::string fname = entry.path().filename().string();
    if (produced.find(fname) == produced.end()) stale.push_back(fname);
  }
  if (!stale.empty()) {
    std::sort(stale.begin(), stale.end());
    std::cerr << "Warning: " << outDir
              << " contains pre-existing .parquet file(s) not produced by this run "
                 "(left untouched): ";
    for (std::size_t i = 0; i < stale.size(); ++i) {
      if (i) std::cerr << ", ";
      std::cerr << stale[i];
    }
    std::cerr << "\n";
  }

  for (const auto& key : seriesOrder) {
    writeSeriesSlotFile(series.at(key), (fs::path(outDir) / keyToFilename.at(key)).string(),
                         compression);
  }
  if (!scalars.empty()) {
    writeScalarsFile(scalars, (fs::path(outDir) / "scalars.parquet").string(), compression);
  }
}

}  // namespace rdf2parquet
