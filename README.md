# rdf2parquet

A C++17 command-line tool that converts RiverWare RDF ensemble output files
to Apache Parquet, as well as reading Parquet files for inspection and CSV export.
The tool uses long/tidy layout by default, or an optional wide (trace-column) 
layout. Built on the official Apache Arrow C++ library (the canonical Parquet 
implementation).

See [`docs/FORMAT.md`](docs/FORMAT.md) for an annotated walkthrough of the
RDF text format and how it maps to each Parquet layout.

## Build

Requires C++17, CMake ≥ 3.25, and [vcpkg](https://vcpkg.io) in manifest mode
(no system-installed Arrow assumed — vcpkg builds it, along with CLI11 and
Catch2, the first time you configure). The first build compiles Arrow from
source and can take tens of minutes; subsequent builds are fast.

vcpkg is a general-purpose toolchain, not a per-project dependency — clone it
once, anywhere convenient *outside* this repo (e.g. next to your other
projects), and reuse it for everything. Set `VCPKG_ROOT` to wherever you put
it; add the `VCPKG_ROOT` line to your shell profile so it persists across
sessions instead of re-running it every time.

### Windows

**Prerequisites:** CMake, Ninja, and a C++17 compiler (MSVC via Visual Studio
Build Tools). If you don't already have them:

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

`CMakePresets.json` uses the Ninja generator, so it needs to find both `cmake`
and `ninja` on `PATH`, and MSVC's compiler needs a Developer environment. Run
everything below from a **Developer PowerShell for VS 2022** prompt — the
Build Tools installer registers one in your Start menu.

```powershell
# once, if you don't already have vcpkg:
git clone https://github.com/microsoft/vcpkg C:\GitHub\vcpkg
C:\GitHub\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\GitHub\vcpkg"
```

Then, from this repository:

```powershell
cmake --preset windows
cmake --build --preset windows
ctest --preset windows --output-on-failure
```

The Developer prompt is not optional. Configuring without it leaves `cl.exe`
unable to find the STL; CMake's compiler detection fails and caches
`CMAKE_CXX_FLAGS` *empty*, which silently drops `/EHsc` and `/O2`. Those cache
entries are only ever initialized once, so the build directory never recovers
on its own. `CMakeLists.txt` fails the configure when it sees this; recover by
deleting `build/windows/CMakeCache.txt` and `build/windows/CMakeFiles`, then
reconfiguring from a Developer prompt.

### Linux

**Prerequisites:** CMake, Ninja, and a C++17 compiler (gcc ≥ 11). On
Ubuntu/Debian, if you don't already have them:

```sh
sudo apt install build-essential cmake ninja-build
```

```sh
# once, if you don't already have vcpkg:
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

Then, from this repository:

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux --output-on-failure
```

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
part of CI, which has no environment with both a built binary and pyarrow. It
needs `pip install pyarrow`, and has not been run yet — the Catch2 suite
(`ctest`) is the automated gate.

## Status

v0.1.0 scope is complete: parser, both writers, the four subcommands, and 36
Catch2 tests, green on Windows (MSVC) and Linux (gcc) in GitHub Actions.
`plan/rdf2parquet-cpp-1.md` tracks task-by-task completion;
`.github/workflows/ci.yml` builds and tests both platforms on every push, and
tagging `v*` publishes a Windows x64 binary to a GitHub release.

Converting 2,520,000 rows of real ensemble output takes under two seconds and
produces a file 3.8× smaller than the source `.rdf` — see
[Observed sizes](docs/FORMAT.md#observed-sizes).

Not yet done: the pyarrow cross-check above, and no macOS preset or CI job.
