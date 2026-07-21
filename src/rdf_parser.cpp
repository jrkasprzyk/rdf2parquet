#include "rdf2parquet/rdf_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>

namespace rdf2parquet {

namespace {

// --- small string helpers ---------------------------------------------------

std::string_view trim(std::string_view s) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string toLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::vector<std::string> splitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t nl = text.find('\n', start);
    std::string_view line =
        (nl == std::string_view::npos) ? text.substr(start) : text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.emplace_back(line);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return lines;
}

// Bare "key: value" or plain "value" line -> the value portion, trimmed.
std::string bareValue(const std::string& line) {
  auto idx = line.find(':');
  if (idx == std::string::npos) return std::string(trim(line));
  return std::string(trim(std::string_view(line).substr(idx + 1)));
}

// Coerce a token to a finite double, or nullopt if not finite/not numeric
// (REQ-001: per-value coercion).
std::optional<double> safeFloat(const std::string& token) {
  std::string_view t = trim(token);
  if (t.empty()) return std::nullopt;
  try {
    std::size_t idx = 0;
    double v = std::stod(std::string(t), &idx);
    if (idx != t.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

// Parse a fully-numeric integer token, or nullopt (used for `trace` and
// count preamble fields; a partially-numeric token like "12a" is rejected
// rather than truncated).
std::optional<long> tryParseInt(const std::string& token) {
  std::string_view t = trim(token);
  if (t.empty()) return std::nullopt;
  try {
    std::size_t idx = 0;
    long v = std::stol(std::string(t), &idx);
    if (idx != t.size()) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

// Convert 'YYYY-M-D 24:00' -> ISO date 'YYYY-MM-DD'. RiverWare's 24:00 means
// end-of-day; the calendar date is kept as-is (matches the Python reference
// _normalize_ts). Single-digit month/day are zero-padded.
std::string normalizeTimestamp(const std::string& raw, std::size_t lineNo) {
  std::string_view trimmed = trim(raw);
  std::size_t spaceIdx = trimmed.find_first_of(" \t");
  std::string_view datePart =
      (spaceIdx == std::string_view::npos) ? trimmed : trimmed.substr(0, spaceIdx);

  // Expect \d{4}-\d{1,2}-\d{1,2}
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  std::size_t i = 0;
  auto readDigits = [&](std::size_t maxLen) -> std::string {
    std::size_t j = i;
    while (j < datePart.size() && j - i < maxLen && isDigit(datePart[j])) ++j;
    std::string out(datePart.substr(i, j - i));
    i = j;
    return out;
  };

  std::string year = readDigits(4);
  bool ok = year.size() == 4 && i < datePart.size() && datePart[i] == '-';
  std::string month, day;
  if (ok) {
    ++i;
    month = readDigits(2);
    ok = !month.empty() && month.size() <= 2 && i < datePart.size() && datePart[i] == '-';
  }
  if (ok) {
    ++i;
    day = readDigits(2);
    ok = !day.empty() && day.size() <= 2 && i == datePart.size();
  }
  if (!ok) {
    throw RdfParseError("Malformed RDF timestamp: \"" + raw + "\"", lineNo);
  }
  if (month.size() == 1) month = "0" + month;
  if (day.size() == 1) day = "0" + day;
  return year + "-" + month + "-" + day;
}

// Read "key:value" lines from pos until endMarker (exclusive); endMarker
// line is consumed. Throws if EOF is reached without finding endMarker
// (REQ-006: missing END markers are a structural, exit-2 error).
std::map<std::string, std::string> parsePreamble(const std::vector<std::string>& lines,
                                                   std::size_t& pos, const std::string& endMarker) {
  std::map<std::string, std::string> result;
  while (pos < lines.size()) {
    const std::string& line = lines[pos];
    ++pos;
    if (line == endMarker) return result;
    auto idx = line.find(':');
    if (idx == std::string::npos) {
      result[std::string(trim(line))] = "";
    } else {
      std::string key(trim(std::string_view(line).substr(0, idx)));
      std::string value(trim(std::string_view(line).substr(idx + 1)));
      result[key] = value;
    }
  }
  throw RdfParseError("Unexpected end of file: missing " + endMarker, pos);
}

struct SlotResult {
  RdfSlot slot;
  std::size_t pos;
};

SlotResult parseSlot(const std::vector<std::string>& lines, std::size_t pos, long nts,
                      std::vector<std::string>& warnings) {
  auto preamble = parsePreamble(lines, pos, "END_SLOT_PREAMBLE");

  if (pos + 1 >= lines.size()) {
    throw RdfParseError("Unexpected end of file: missing units/scale lines for a slot.", pos + 1);
  }
  const std::string& unitsLine = lines[pos]; ++pos;
  const std::string& scaleLine = lines[pos]; ++pos;

  RdfSlot slot;
  slot.object_type = preamble.count("object_type") ? preamble["object_type"] : "";
  slot.object_name = preamble.count("object_name") ? preamble["object_name"] : "";
  slot.slot_type = preamble.count("slot_type") ? preamble["slot_type"] : "";
  slot.slot_name = preamble.count("slot_name") ? preamble["slot_name"] : "";
  slot.units = bareValue(unitsLine);
  slot.scale = bareValue(scaleLine);

  std::size_t ecIdx = pos;
  while (ecIdx < lines.size() && lines[ecIdx] != "END_COLUMN") ++ecIdx;
  if (ecIdx >= lines.size()) {
    throw RdfParseError("END_COLUMN not found after position " + std::to_string(pos + 1),
                         pos + 1);
  }

  long nValues = static_cast<long>(ecIdx - pos);
  std::string slotType = toLower(slot.slot_type);
  bool isSeriesSlot = slotType.find("series") != std::string::npos;
  bool isScalarSlot = slotType.find("scalar") != std::string::npos;

  bool matched = true;
  if (isSeriesSlot) {
    slot.is_scalar = false;
  } else if (isScalarSlot) {
    slot.is_scalar = true;
  } else if (nValues == nts) {
    slot.is_scalar = false;
  } else if (nValues == 1) {
    slot.is_scalar = true;
  } else {
    matched = false;
  }

  if (!matched) {
    std::ostringstream msg;
    msg << "Slot '" << slot.object_name << "." << slot.slot_name << "': expected " << nts
        << " or 1 values, found " << nValues << ". Skipping.";
    warnings.push_back(msg.str());
    slot.is_scalar = std::nullopt;
  } else {
    for (std::size_t i = pos; i < pos + static_cast<std::size_t>(nValues); ++i) {
      slot.values.push_back(safeFloat(lines[i]));
    }
  }

  pos = ecIdx + 1;
  // REQ-006 deviation from the Python/JS references: the line after
  // END_COLUMN MUST be END_SLOT. A multi-column slot (more than one value
  // column) puts something else here; the references silently mis-parse
  // that as the start of the next slot's preamble, so we reject it instead.
  if (pos >= lines.size() || lines[pos] != "END_SLOT") {
    throw RdfParseError(
        "Expected END_SLOT after END_COLUMN for slot '" + slot.object_name + "." +
            slot.slot_name + "' (multi-column slots are not supported).",
        pos + 1);
  }
  ++pos;

  return SlotResult{std::move(slot), pos};
}

struct RunResult {
  RdfRun run;
  std::size_t pos;
};

RunResult parseRun(const std::vector<std::string>& lines, std::size_t pos, int ordinal,
                    std::vector<std::string>& warnings) {
  RdfRun run;
  run.preamble = parsePreamble(lines, pos, "END_RUN_PREAMBLE");

  auto timeStepsIt = run.preamble.find("time_steps");
  if (timeStepsIt == run.preamble.end()) timeStepsIt = run.preamble.find("timesteps");
  long nts = 0;
  if (timeStepsIt != run.preamble.end()) {
    auto parsed = tryParseInt(timeStepsIt->second);
    if (!parsed || *parsed < 0) {
      throw RdfParseError("Invalid time_steps value: \"" + timeStepsIt->second + "\"", pos);
    }
    nts = *parsed;
  }
  // SEC-001: reject counts that would over-allocate before touching them.
  if (static_cast<std::size_t>(nts) > lines.size() - pos) {
    throw RdfParseError(
        "time_steps (" + std::to_string(nts) + ") exceeds remaining lines in file.", pos);
  }

  auto traceIt = run.preamble.find("trace");
  if (traceIt != run.preamble.end()) {
    auto parsed = tryParseInt(traceIt->second);
    run.trace_id = parsed ? static_cast<int32_t>(*parsed) : ordinal;
  } else {
    run.trace_id = ordinal;
  }

  run.timesteps.reserve(static_cast<std::size_t>(nts));
  for (long i = 0; i < nts; ++i) {
    run.timesteps.push_back(normalizeTimestamp(lines[pos + static_cast<std::size_t>(i)], pos + 1));
  }
  pos += static_cast<std::size_t>(nts);

  bool sawEndRun = false;
  while (pos < lines.size()) {
    if (lines[pos] == "END_RUN") {
      ++pos;
      sawEndRun = true;
      break;
    }
    auto slotResult = parseSlot(lines, pos, nts, warnings);
    pos = slotResult.pos;
    run.slots.push_back(std::move(slotResult.slot));
  }
  if (!sawEndRun) {
    throw RdfParseError("Unexpected end of file: missing END_RUN.", pos);
  }

  return RunResult{std::move(run), pos};
}

}  // namespace

RdfParseError::RdfParseError(const std::string& message, std::size_t line)
    : std::runtime_error(line == 0 ? message : message + " (line " + std::to_string(line) + ")"),
      line_(line) {}

RdfFile parseRdf(std::string_view text) {
  std::vector<std::string> lines = splitLines(text);

  RdfFile file;
  std::size_t pos = 0;
  file.package_preamble = parsePreamble(lines, pos, "END_PACKAGE_PREAMBLE");

  auto runsIt = file.package_preamble.find("number_of_runs");
  if (runsIt == file.package_preamble.end()) {
    throw RdfParseError("Not a valid RDF file: missing number_of_runs.", pos);
  }
  auto expectedRuns = tryParseInt(runsIt->second);
  if (!expectedRuns || *expectedRuns < 0) {
    throw RdfParseError("Invalid number_of_runs value: \"" + runsIt->second + "\"", pos);
  }
  // SEC-001: a run needs at least an END_RUN_PREAMBLE and END_RUN line, so
  // this is a safe lower-bound sanity cap against a corrupt/huge count.
  if (static_cast<std::size_t>(*expectedRuns) > lines.size() - pos) {
    throw RdfParseError(
        "number_of_runs (" + std::to_string(*expectedRuns) + ") exceeds remaining lines in file.",
        pos);
  }

  while (file.runs.size() < static_cast<std::size_t>(*expectedRuns) && pos < lines.size()) {
    auto result = parseRun(lines, pos, static_cast<int>(file.runs.size()) + 1, file.warnings);
    pos = result.pos;
    file.runs.push_back(std::move(result.run));
  }

  if (file.runs.empty()) {
    throw RdfParseError("RDF file contains no runs.", pos);
  }

  return file;
}

}  // namespace rdf2parquet
