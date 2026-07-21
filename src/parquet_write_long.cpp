// Long-format writer (REQ-002/REQ-003): the default `convert` output. One row
// per (run, slot, timestep) using arrow builders directly rather than
// row-at-a-time RecordBatch construction, since the whole file already sits
// in memory as an RdfFile by the time we get here (Phase 2 parses eagerly).

#include "rdf2parquet/parquet_write.hpp"

#include "parquet_write_common.hpp"

namespace rdf2parquet {

void writeLongParquet(const RdfFile& file, const std::string& outPath, Compression compression) {
  using namespace detail;

  auto schema = longSchema();
  LongRowBuilders builders;

  for (const auto& run : file.runs) {
    for (const auto& slot : run.slots) {
      if (!slot.is_scalar.has_value()) continue;  // REQ-006: warned-and-skipped slot

      if (*slot.is_scalar) {
        std::optional<double> v;
        if (!slot.values.empty()) v = slot.values[0];
        builders.appendRow(run.trace_id, slot, std::nullopt, v);
      } else {
        for (std::size_t i = 0; i < run.timesteps.size(); ++i) {
          int32_t days = isoDateToDays(run.timesteps[i]);
          std::optional<double> v;
          if (i < slot.values.size()) v = slot.values[i];
          builders.appendRow(run.trace_id, slot, days, v);
        }
      }
    }
  }

  auto table = builders.finish(schema);

  std::vector<std::string> keys = {"rdf2parquet.version", "rdf.package_preamble",
                                    "rdf.run_preambles"};
  std::vector<std::map<std::string, std::string>> runPreambles;
  runPreambles.reserve(file.runs.size());
  for (const auto& run : file.runs) runPreambles.push_back(run.preamble);
  std::vector<std::string> values = {RDF2PARQUET_VERSION, jsonObject(file.package_preamble),
                                      jsonArrayOfObjects(runPreambles)};
  auto kvMeta = std::make_shared<arrow::KeyValueMetadata>(keys, values);
  table = table->ReplaceSchemaMetadata(kvMeta);

  writeTableToFile(table, outPath, compression);
}

}  // namespace rdf2parquet
