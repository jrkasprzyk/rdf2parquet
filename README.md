# rdf2parquet

A C++17 command-line tool that converts RiverWare RDF ensemble output files
to Apache Parquet — long/tidy layout by default, optional wide (trace-column)
layout — and reads Parquet files back for inspection and CSV export. Built on
the official Apache Arrow C++ library (the canonical Parquet implementation),
so this codebase doubles as a compact, real-world example of using Arrow C++
for the CADSWES/RiverWare team.

See [`docs/FORMAT.md`](docs/FORMAT.md) for an annotated walkthrough of the
RDF text format and exactly how it maps to each Parquet layout.

## Build

Requires C++17, CMake ≥ 3.25, and [vcpkg](https://vcpkg.io) in manifest mode
(no system-installed Arrow assumed — vcpkg builds it, along with CLI11 and
Catch2, the first time you configure). The first build compiles Arrow from
source and can take tens of minutes; subsequent builds are fast.

```sh
# once, if you don't already have vcpkg:
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh          # Linux/macOS
# .\vcpkg\bootstrap-vcpkg.bat       # Windows
export VCPKG_ROOT=$(pwd)/vcpkg      # Linux/macOS
# $env:VCPKG_ROOT = "$(pwd)\vcpkg"  # Windows PowerShell
```

Then, from this repository:

```sh
cmake --preset windows   # or: linux
cmake --build --preset windows
ctest --preset windows --output-on-failure
```

(`CMakePresets.json` picks the right one; `windows`/`linux` here are preset
names, not commands you need to branch on manually.)

## CLI reference

```
rdf2parquet convert <in.rdf> <out> [--wide] [--compression zstd|snappy|none]
rdf2parquet info <file.parquet>
rdf2parquet head <file.parquet> [-n N]
rdf2parquet to-csv <file.parquet> <out.csv>
rdf2parquet --version
```

- **`convert`** — `<in.rdf>` must have a `.rdf` extension and pass structural
  validation (see `docs/FORMAT.md`). By default `<out>` is a single long-format
  `.parquet` file; with `--wide`, `<out>` is a directory holding one file per
  series slot plus `scalars.parquet`. `--compression` defaults to `zstd`.
- **`info`** — schema (name, physical/logical/Arrow type per column), row
  count, row groups, per-column-chunk compression, and all key-value
  metadata. Works on any Parquet file, not just rdf2parquet's own output.
- **`head`** — first `N` rows (default 10), tab-separated, to stdout. Nulls
  print as empty fields. Rejects files with nested (list/struct/map) columns.
- **`to-csv`** — full file to RFC-4180 CSV. Same null and nested-column rules
  as `head`.

Exit codes: `0` success, `1` usage/CLI error (bad flag, wrong input
extension, missing file), `2` malformed input (RDF parse error or Parquet
structural problem — the message names the offending line or column).

## Reading the output

```python
import pandas as pd

df = pd.read_parquet("out.parquet")
df[df["slot"] == "Pool Elevation"].groupby("trace")["value"].mean()
```

```sql
-- DuckDB reads Parquet directly, no import step:
SELECT object, slot, avg(value) AS mean_value
FROM read_parquet('out.parquet')
GROUP BY object, slot;
```

## Fixtures

`tests/fixtures/sample_traces.rdf` and `tests/fixtures/sample_subset.rdf` are
synthetic RiverWare-style sample data created for this project (see
`tests/fixtures/README.md`), copied from `public/rw-sample-data/` at the
repository root.

## Cross-implementation validation

`tools/crosscheck.py` compares this tool's long-format Parquet output
row-for-row against `example_scripts/rdf_parser.py`, the Python reference
parser this tool's behavior is validated against (TEST-005):

```sh
rdf2parquet convert tests/fixtures/sample_traces.rdf /tmp/traces.parquet
python tools/crosscheck.py tests/fixtures/sample_traces.rdf /tmp/traces.parquet
```

This is a manual check, run against both fixtures after a build; it is not
part of CI (CI has no built binary + pyarrow environment together). Not yet
run against a local build in this environment — see `docs/FORMAT.md` for why.

## Status

Full source, tests, and CI are in place (`plan/rdf2parquet-cpp-1.md` tracks
task-by-task completion) but the tool has not yet been built or run in this
environment — no C++ toolchain, CMake, or vcpkg were available where this
code was written. The GitHub Actions workflow (`.github/workflows/ci.yml`) is
the first real build/test gate; treat the first CI run (or first local build)
as a validation step, not a formality.
