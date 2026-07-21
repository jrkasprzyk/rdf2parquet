---
goal: Build rdf2parquet — a standalone C++ CLI that converts RiverWare RDF files to Apache Parquet (long + optional wide layout) and reads/inspects Parquet files
version: 1.0
date_created: 2026-07-21
owner: Joseph Kasprzyk (jrkasprzyk)
status: 'Planned'
tags: [feature, architecture, cpp, parquet, riverware, cadswes, education]
---

# Introduction

![Status: Planned](https://img.shields.io/badge/status-Planned-blue)

Create a new repository `rdf2parquet` containing a C++17 command-line utility that (a) parses RiverWare RDF ensemble output files, (b) writes them as Apache Parquet in a long/tidy layout by default with an optional wide (trace-columns) layout, and (c) reads Parquet files back for inspection (`info`, `head`) and CSV export (`to-csv`). The tool uses the official Apache Arrow C++ library (which contains the canonical Parquet implementation) so the codebase doubles as an educational reference for the CADSWES team, whose RiverWare codebase is C++. Behavior is validated against the existing Python reference parser `example_scripts/rdf_parser.py` and shared fixtures `public/rw-sample-data/*.rdf`. 

## 1. Requirements & Constraints

- **REQ-001**: Due to RiverWare's formatting requirements, RDF parsing semantics MUST match `example_scripts/rdf_parser.py` exactly: timestamps `YYYY-M-D 24:00` normalize to ISO calendar date `YYYY-MM-DD` (24:00 = end of day, date kept as-is, zero-padded); `units` and `scale` are read as bare single-value lines per slot; values are stored raw (scale is NOT applied to values). **Deliberate deviation from the references' value coercion**: each value token is coerced independently — a non-finite/non-numeric token becomes null (`std::optional<double>` empty), rather than the references' whole-list fallback to raw strings. `tools/crosscheck.py` applies the same per-value coercion to the Python parser's output before comparing (DECISION 2026-07-21).
- **REQ-002**: Default (long) Parquet schema, exact columns in order: `trace` (int32 — the run preamble `trace` value when present and parseable, else the 1-based run ordinal within the file; matches `rdfParser.js` DR-01), `object` (string, dictionary-encoded), `slot` (string, dictionary-encoded), `timestep` (date32, nullable — null for scalar-slot rows), `units` (string, dictionary-encoded), `scale` (float64, nullable — null when the RDF scale token is not a finite number), `value` (float64, nullable).
- **REQ-003**: Long-format Parquet file-level key-value metadata MUST include: `rdf2parquet.version` (tool semver), `rdf.package_preamble` (JSON object of the package preamble key/values), `rdf.run_preambles` (JSON array, one object per run, in file order).
- **REQ-004**: Wide layout (`--wide`): output path is a directory (created if missing; files it writes are overwritten; pre-existing `.parquet` files not produced by this run trigger a stderr warning listing them, never deletion); one Parquet file per series slot named `<object>.<slot>.parquet` after sanitizing characters outside `[A-Za-z0-9._-]` to `_` (two slots sanitizing to the same filename → exit 2 naming both slot keys); columns: `timestep` (date32) then `trace_<id>` … (float64, nullable) where `<id>` is the same trace identifier as the long-format `trace` column (duplicate trace ids within one file → exit 2). Scalar slots are NOT discarded: they are written to `scalars.parquet` in the output directory using the long-format scalar row shape (trace, object, slot, units, scale, value — no timestep column).
- **REQ-005**: CLI surface (CLI11 subcommands): `rdf2parquet convert <in.rdf> <out> [--wide] [--compression zstd|snappy|none]` (default `zstd`); `rdf2parquet info <file.parquet>` (schema, row count, row groups, compression, key-value metadata); `rdf2parquet head <file.parquet> [-n N]` (default N=10, tab-separated to stdout); `rdf2parquet to-csv <file.parquet> <out.csv>`. Convert input must have a `.rdf` extension (case-insensitive; rejection = exit 1) AND pass content validation (missing `number_of_runs` before `END_PACKAGE_PREAMBLE`, or zero runs parsed → exit 2 "not a valid RDF"). `info` accepts any Parquet file; `head`/`to-csv` support flat schemas only — nested (list/struct/map) columns → exit 2 naming the offending column(s). Nulls render as empty fields in both TSV and CSV output.
- **REQ-006**: Exit codes: 0 success; 1 usage/CLI error (includes wrong input extension); 2 malformed input file (parse error with descriptive message naming the offending line number). Exit 2 is reserved for STRUCTURAL failures: truncated file, missing END markers, invalid preamble counts, multi-column slots (after `END_COLUMN` the next line must be `END_SLOT`, else exit 2 — the references silently mis-parse here), missing `number_of_runs`, zero runs. A slot whose value count is neither `time_steps` nor 1 is NOT structural: warn on stderr, exclude the slot from output, continue, exit 0 (matches both references' warn-and-skip).
- **REQ-007**: Educational quality: every public header documents WHY (format rationale, Arrow API choices) not just what; `docs/FORMAT.md` explains the RDF text format and its mapping to both Parquet layouts; `README.md` includes a quickstart plus pandas and DuckDB snippets that read the produced files.
- **SEC-001**: Parser MUST reject files that would over-allocate: cap `time_steps` and run/slot counts read from preambles against actual remaining line count before allocating vectors; malformed counts produce exit code 2, not OOM.
- **CON-001**: C++17 minimum (Arrow C++ requirement); CMake ≥ 3.25; dependencies via vcpkg manifest mode only (no system-installed Arrow assumed).
- **CON-002**: Must build and pass tests on Windows (MSVC) and Linux (gcc) — CADSWES ships RiverWare on Windows; CI covers both.
- **CON-003**: No dependency to any other repos at runtime; fixtures are copied (not submoduled) into `tests/fixtures/` with a provenance note.
- **GUD-001**: Single-purpose translation units: parser knows nothing about Arrow; writers/readers know nothing about RDF text. Interface between them is plain structs in `rdf_model.hpp`.
- **PAT-001**: Follow the parse structure of `example_scripts/rdfParser.js` (preamble reader consuming to end-marker, slot-block parser, run loop) so the three implementations stay reviewable side-by-side.

## 2. Implementation Steps

### Implementation Phase 1 — Repository scaffold and build infrastructure

- GOAL-001: A cloneable repo that configures, builds an empty CLI, and runs a trivial test on Windows + Linux CI.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-001 | Use THIS existing repo (`C:\Github\rdf2parquet`, created for this purpose — do not scaffold elsewhere). Rename branch `master`→`main` (`git branch -m master main`), add MIT `LICENSE`, push to github.com/jrkasprzyk/rdf2parquet (public). Keep existing folders (`plan/`, `example_scripts/`, `public/rw-sample-data/`). Add: `CMakeLists.txt`, `vcpkg.json`, `src/`, `include/rdf2parquet/`, `tests/`, `docs/`, `.github/workflows/`, `.gitignore` (CMake/vcpkg/build artifacts), `README.md` stub. | | |
| TASK-002 | `vcpkg.json` manifest with dependencies: `arrow` (feature `parquet`), `cli11`, `catch2`; pin a vcpkg baseline commit. `CMakeLists.txt`: project `rdf2parquet` VERSION 0.1.0, C++17, targets `rdf2parquet_lib` (static lib), `rdf2parquet` (CLI exe), `rdf2parquet_tests` (Catch2, registered via `catch_discover_tests`). | | |
| TASK-003 | GitHub Actions workflow `.github/workflows/ci.yml`: matrix `windows-latest` (MSVC) + `ubuntu-latest` (gcc); steps: checkout, vcpkg install with GitHub Actions binary cache (`vcpkg-action` or `run-vcpkg`), CMake configure/build Release, `ctest --output-on-failure`. | | |
| TASK-004 | Sample data exists in `public/rw-sample-data`. Copy to `tests/fixtures/`; add `tests/fixtures/README.md` describing them as synthetic RiverWare-style sample data created for this project, copied from `public/rw-sample-data/` (no external-repo references). | | |

### Implementation Phase 2 — RDF parser core (no Arrow)

- GOAL-002: `parseRdf(std::string_view) -> RdfFile` faithful to the Python reference, fully unit-tested.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-005 | `include/rdf2parquet/rdf_model.hpp`: structs `RdfSlot` {object, slot_name, units (string), scale (string), values (std::vector<std::optional<double>>), is_scalar}, `RdfRun` {preamble (string→string map), timesteps (std::vector<std::string> ISO dates), slots}, `RdfFile` {package_preamble, runs, warnings (std::vector<std::string>)}. | | |
| TASK-006 | `src/rdf_parser.cpp` + `include/rdf2parquet/rdf_parser.hpp`: port `normalizeTimestamp`, `parsePreamble`, `bareValue`, `safeFloat`, slot-block parser, run loop from `example_scripts/rdfParser.js` per PAT-001, EXCEPT: per-value null coercion (REQ-001 deviation), multi-column-slot detection (after `END_COLUMN` require `END_SLOT` else throw), wrong-value-count slot → warn+skip (REQ-006). Throw `RdfParseError` (carries line number) on structural errors; enforce SEC-001 allocation caps. Validate `number_of_runs` present and ≥1 run parsed. | | |
| TASK-007 | `tests/test_rdf_parser.cpp`: port every case from `example_scripts/test_rdf_parser.py` (timestamp normalization incl. single-digit month/day, preamble parsing, scalar vs series slots, malformed-input errors, non-finite value → null) plus fixture-driven assertions on `sample_traces.rdf` (run count, slot names, first/last values, units, scale). | | |

### Implementation Phase 3 — Long-format Parquet writer

- GOAL-003: `convert in.rdf out.parquet` produces the REQ-002/REQ-003 long table readable by pandas and DuckDB.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-008 | `src/parquet_write_long.cpp` + header: build `arrow::Table` from `RdfFile` using builders (`Int32Builder`, `StringDictionaryBuilder`, `Date32Builder`, `DoubleBuilder`); exact column order/types per REQ-002; ISO date string → date32 via days-since-epoch conversion; scalar slots emitted as single row with null timestep. | | |
| TASK-009 | Attach REQ-003 key-value metadata (serialize preambles to JSON with a minimal internal JSON writer — no extra dependency); write via `parquet::arrow::WriteTable` with compression from `--compression` flag (zstd default), row group size 1,000,000. | | |
| TASK-010 | `tests/test_write_long.cpp`: write fixture to temp dir, read back with `parquet::arrow::OpenFile`, assert schema (names, types, order), row count = Σ runs×timesteps + scalars, spot-check values against parser output, assert metadata keys present and JSON-parseable. | | |

### Implementation Phase 4 — Wide-format writer

- GOAL-004: `convert --wide in.rdf outdir/` produces per-slot trace-column files per REQ-004.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-011 | `src/parquet_write_wide.cpp` + header: group slots by (object, slot_name) across runs; validate all runs share identical timestep vectors for a slot (mismatch → exit 2 with message); validate trace ids unique (dup → exit 2); emit `timestep` + `trace_<id>` columns (same ids as long format); filename sanitization + collision check per REQ-004; dir create/overwrite/stale-warning semantics per REQ-004; write scalar slots to `scalars.parquet` in the output dir (long-format scalar shape, no timestep column). | | |
| TASK-012 | `tests/test_write_wide.cpp`: fixture → wide dir; assert file-per-slot naming, column count = runs+1, values match long-format pivot for two sampled slots; assert `scalars.parquet` present with expected scalar rows; assert stale-file warning and filename-collision error paths. | | |

### Implementation Phase 5 — Parquet read side (info, head, to-csv)

- GOAL-005: The tool reads arbitrary Parquet files (not only its own output) for inspection and CSV export.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-013 | `src/parquet_read.cpp` + header: `info` prints file path, size, row count, column schema (name, physical+logical type), row-group count/sizes, compression codec per column chunk, and all key-value metadata; `head -n N` prints first N rows tab-separated (dictionary columns decoded, nulls as empty string); `to-csv` streams full file to RFC-4180 CSV (quote fields containing comma/quote/newline); `head`/`to-csv` reject nested (list/struct/map) columns with exit 2 per REQ-005; nulls → empty fields. | | |
| TASK-014 | `src/main.cpp`: CLI11 app with the four subcommands per REQ-005, `--version`, exit-code mapping per REQ-006 (catch `RdfParseError` → 2, CLI11 parse error → 1). | | |
| TASK-015 | `tests/test_cli.cpp`: end-to-end via `std::system` on the built binary (path passed by CMake `target_compile_definitions`): convert→info→head→to-csv round trip on `sample_traces.rdf`; assert exit codes for missing file, malformed RDF, bad flag. | | |

### Implementation Phase 6 — Validation vs reference, docs, release

- GOAL-006: Cross-implementation parity proven; educational docs complete; v0.1.0 tagged.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-016 | `tools/crosscheck.py`: parses a given RDF with `example_scripts/rdf_parser.py` (default `--reference` path: `example_scripts/rdf_parser.py` in this repo), reads the tool's long Parquet with pyarrow, asserts row-for-row equality (trace, object, slot, timestep, units, value; float compare exact since both store raw tokens). Applies the REQ-001 per-value null coercion to the Python output before comparing (the reference falls back to whole-list strings on any non-numeric token — a documented deliberate deviation). Run manually against both fixtures; record result in README. | | |
| TASK-017 | `docs/FORMAT.md`: annotated RDF walkthrough (preambles, timestep list, slot blocks, END markers) with a real excerpt from `sample_subset.rdf`; mapping tables RDF→long schema and RDF→wide schema; explanation of dictionary encoding, row groups, and compression choices with observed size numbers (CSV vs zstd Parquet for the fixtures). | | |
| TASK-018 | `README.md`: build instructions (vcpkg bootstrap + CMake preset, Windows and Linux), CLI reference for all four subcommands, pandas snippet (`pd.read_parquet`), DuckDB snippet (`SELECT object, slot, avg(value) ... GROUP BY`), fixture provenance, link to FORMAT.md. Copy this plan file into `rdf2parquet/plan/` and mark completed tasks. | | |
| TASK-019 | Tag `v0.1.0`; attach CI-built Windows x64 binary zip to the GitHub release (add release job to ci.yml triggered on tag). | | |

## 3. Alternatives

- **ALT-001**: Extend `parasolpy` (owner's Python package) with pyarrow output — least code, easiest pip-install sharing, but user chose C++ specifically because CADSWES/RiverWare is a C++ shop and a C++ example is most transferable to them.
- **ALT-002**: Node CLI inside the `ensemble-viewer` repo reusing `src/lib/rdfParser.js` — maximal reuse, but npm tooling is a poor sharing story for the RiverWare team and couples the utility to the viewer repo.
- **ALT-003**: Static browser page (rdfParser.js + hyparquet-writer) — zero-install URL sharing, but no scripting/automation story and no C++ educational value.
- **ALT-004**: Lightweight single-header Parquet writer instead of Arrow C++ — much smaller build, but loses the read side, canonical-implementation credibility, and most educational value.

## 4. Dependencies

- **DEP-001**: Apache Arrow C++ with `parquet` feature, via vcpkg (provides `parquet::arrow::WriteTable` / `OpenFile`, compression codecs zstd/snappy).
- **DEP-002**: CLI11 (header-only argument parsing, subcommand support).
- **DEP-003**: Catch2 v3 (unit/integration tests, CTest integration).
- **DEP-004**: vcpkg (manifest mode, pinned baseline) + CMake ≥ 3.25 + MSVC 2022 / gcc ≥ 11.
- **DEP-005**: Reference assets from `example_scripts` in this repo: `rdf_parser.py`, `test_rdf_parser.py`, , `rdfParser.js` and sample data found here: `public/rw-sample-data/*.rdf` (structural reference).

## 5. Files

All paths relative to the new `rdf2parquet` repository root.

- **FILE-001**: `CMakeLists.txt`, `vcpkg.json`, `CMakePresets.json` — build system (Phase 1).
- **FILE-002**: `.github/workflows/ci.yml` — Windows+Linux CI, release job (Phases 1, 6).
- **FILE-003**: `include/rdf2parquet/rdf_model.hpp`, `include/rdf2parquet/rdf_parser.hpp`, `src/rdf_parser.cpp` — parser core (Phase 2).
- **FILE-004**: `include/rdf2parquet/parquet_write.hpp`, `src/parquet_write_long.cpp`, `src/parquet_write_wide.cpp` — writers (Phases 3–4).
- **FILE-005**: `include/rdf2parquet/parquet_read.hpp`, `src/parquet_read.cpp` — info/head/to-csv (Phase 5).
- **FILE-006**: `src/main.cpp` — CLI11 entry point (Phase 5).
- **FILE-007**: `tests/test_rdf_parser.cpp`, `tests/test_write_long.cpp`, `tests/test_write_wide.cpp`, `tests/test_cli.cpp`, `tests/fixtures/*` — tests + fixtures (Phases 1–5).
- **FILE-008**: `docs/FORMAT.md`, `README.md`, `tools/crosscheck.py`, `plan/feature-rdf2parquet-cpp-1.md` — docs, validation, plan copy (Phase 6).

## 6. Testing

- **TEST-001**: Parser unit tests porting every case in `scripts/test_rdf_parser.py` (timestamps, preambles, scalar/series slots, malformed input, non-finite values) — Phase 2.
- **TEST-002**: Long-writer round trip: schema/order/type assertions, row count formula, value spot checks, metadata JSON presence — Phase 3.
- **TEST-003**: Wide-writer: per-slot file naming, column counts, long↔wide pivot equivalence on sampled slots — Phase 4.
- **TEST-004**: CLI end-to-end: convert→info→head→to-csv on fixtures; exit codes 0/1/2 per REQ-006 — Phase 5.
- **TEST-005**: Cross-implementation parity via `tools/crosscheck.py` against the Python reference on both fixtures — Phase 6 (manual, result recorded in README).
- **TEST-006**: CI green on windows-latest and ubuntu-latest for every phase merge — continuous.

## 7. Risks & Assumptions

- **RISK-001**: Arrow C++ via vcpkg is a heavy first build (tens of minutes); mitigated by CI binary caching (TASK-003) — but first local build on a fresh machine will be slow; document expectation in README.
- **RISK-002**: Real CRMMS/CRSS RDFs may contain constructs absent from the two fixtures (multiple columns per slot, annual timesteps, flags); parser errors are descriptive (REQ-006) so gaps surface loudly; fixture set can grow. Multi-column slots specifically are DETECTED and rejected with exit 2 (the references silently consume to EOF on them); support deferred past v0.1.
- **RISK-003**: Wide mode assumes identical timesteps across runs for a slot (REQ-004/TASK-011); real files with per-run date ranges would fail — explicit error chosen over silent alignment.
- **ASSUMPTION-001**: `example_scripts/rdf_parser.py` is the agreed behavioral reference, with two documented deviations decided 2026-07-21: per-value null coercion (REQ-001) and multi-column-slot detection (REQ-006).
- **ASSUMPTION-002** (RESOLVED 2026-07-21): `trace` = run preamble `trace` value, falling back to 1-based ordinal — preserves trace identity in split ensemble files (e.g. one file holding traces 27–53).
- **ASSUMPTION-003**: MIT license acceptable for sharing with CADSWES team.

## 8. Related Specifications / Further Reading

- `example_scripts/rdf_parser.py` — Python reference parser (behavioral source of truth)
- `example_scripts/rdfParser.js` — JS port, structural pattern reference
- [Apache Arrow C++ Parquet documentation](https://arrow.apache.org/docs/cpp/parquet.html)
- [Parquet format specification](https://parquet.apache.org/docs/file-format/)
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/consume/manifest-mode)
- [CADSWES RiverWare](https://www.riverware.org/)
