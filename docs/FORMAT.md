# The RDF format, and how rdf2parquet maps it to Parquet

RiverWare's RDF ("RiverWare Data Format") is a line-oriented text format: one
token per line, no delimiters within a line beyond an optional `key:value`
colon. There is no line-length limit and no escaping — a value that itself
contained a newline could not be represented, which is why every RDF file is
just a flat list of lines consumed strictly in order. This document walks the
format section by section using a real excerpt from `tests/fixtures/sample_subset.rdf`
(synthetic RiverWare-style sample data created for this project), then shows
where each section lands in rdf2parquet's two output layouts.

## 1. Package preamble

```
name:Example Subset Ensemble Runs
owner:example_user
description:Synthetic subset sample for testing
create_date:2024-6-1 0:00
number_of_runs:2
END_PACKAGE_PREAMBLE
```

File-level metadata as `key:value` lines, terminated by the literal marker
line `END_PACKAGE_PREAMBLE`. `number_of_runs` is the one key rdf2parquet
actually reads structurally — it is how many run blocks to expect next; every
other key is opaque and gets carried through verbatim (see §5, metadata).
A file missing `number_of_runs`, or missing the `END_PACKAGE_PREAMBLE`
marker entirely (a truncated file), is rejected with exit code 2.

## 2. Run preamble + timestep list

```
trace:1
start:2024-6-1 24:00
end:2024-6-4 24:00
time_step_unit:day
unit_quantity:1
time_steps:4
consecutive:0
rule_set:(Example Rules)
idx_sequential:0
END_RUN_PREAMBLE
2024-6-1 24:00
2024-6-2 24:00
2024-6-3 24:00
2024-6-4 24:00
```

Same `key:value` shape as the package preamble, ended by `END_RUN_PREAMBLE`.
`time_steps` (falling back to `timesteps`) tells the parser exactly how many
of the following lines are the timestep list — RiverWare always stamps
`24:00` (end-of-day) rather than midnight, so `2024-6-1 24:00` normalizes to
the calendar date `2024-06-01`, not `2024-06-02`. Single-digit month/day
values are zero-padded during normalization.

`trace` identifies this run within the ensemble. rdf2parquet uses it as the
`trace` column value when it parses as an integer; a run whose preamble omits
`trace`, or has a non-integer value, falls back to its 1-based position among
the runs in this file. This fallback is what lets a file holding only traces
27–53 of a larger 100-trace ensemble still carry meaningful trace identity.

## 3. Slot blocks

Each run then holds one block per "slot" (RiverWare's term for a single
object+attribute timeseries or scalar):

```
object_type: LevelPowerReservoir
object_name: Example Reservoir
slot_type: SeriesSlot
slot_name: Pool Elevation
END_SLOT_PREAMBLE
units: feet
scale: 1
1095.0
1094.5
1094.0
1093.5
END_COLUMN
END_SLOT
```

- **Slot preamble** (`object_type`/`object_name`/`slot_type`/`slot_name`,
  more `key:value` lines) ended by `END_SLOT_PREAMBLE`.
- **Units and scale**: two bare lines (not `key:value` — just the value,
  optionally prefixed `label: value`). `scale` is stored as-is and is
  *never* applied to the values that follow (REQ-001) — RiverWare's own
  convention leaves that multiplication to the reader, and rdf2parquet
  preserves that rather than silently rescaling.
- **Values**: either `time_steps` lines (a *series* slot, one value per
  timestep) or exactly 1 line (a *scalar* slot, one value for the whole
  run) — whichever `slot_type` doesn't already say directly (`SeriesSlot` /
  `ScalarSlot`). A value count matching neither triggers a warning and the
  slot is excluded from output; the file itself is still valid (exit 0).
- **`END_COLUMN`** closes the value list. The line immediately after it
  **must** be `END_SLOT` — a slot with a second value column would put
  something else there instead, and rdf2parquet rejects that (exit 2) rather
  than silently misreading the extra column as the next slot's preamble.

Each value token is parsed independently: a token that isn't a finite number
becomes a null in that row's `value` cell, rather than falling back the
entire slot to text (a deliberate deviation from the Python/JS references,
which fall back per-slot — see `ASSUMPTION-001` in the project plan).

## 4. Run and file end

`END_RUN` closes a run (back to §2 for the next one); the file ends once
`number_of_runs` runs have been read. Anything past that point is ignored.

## 5. Mapping to the long layout (default)

The long layout is one row per (run, slot, timestep) — or one row per (run,
slot) for scalars, with `timestep` null:

| RDF source | Long column | Type | Notes |
|---|---|---|---|
| run `trace`, or 1-based run ordinal | `trace` | `int32` | REQ-002 fallback rule (§2) |
| slot `object_name` | `object` | `string`, dictionary-encoded | low cardinality — see §7 |
| slot `slot_name` | `slot` | `string`, dictionary-encoded | |
| normalized timestep | `timestep` | `date32`, nullable | null for scalar-slot rows |
| slot `units` line | `units` | `string`, dictionary-encoded | |
| slot `scale` line | `scale` | `float64`, nullable | raw token; null if not a finite number; never applied to `value` |
| one value token | `value` | `float64`, nullable | null if that token wasn't a finite number (REQ-001) |

File-level key-value metadata carries what didn't become a column:
`rdf2parquet.version` (this tool's semver), `rdf.package_preamble` (the
package preamble as a JSON object), and `rdf.run_preambles` (a JSON array,
one object per run, in file order) — so nothing from the original preambles
is lost even though most of it (e.g. `rule_set`, `consecutive`) has no
tabular meaning.

## 6. Mapping to the wide layout (`--wide`)

The wide layout mirrors how an analyst usually wants to *look* at ensemble
output: one file per slot, one column per trace.

- One `<object>.<slot>.parquet` per **series** slot (characters outside
  `[A-Za-z0-9._-]` become `_`, so `Example Reservoir.Pool Elevation` becomes
  `Example_Reservoir.Pool_Elevation.parquet`). Columns: `timestep` (`date32`)
  then one `trace_<id>` (`float64`, nullable) per run, in ascending trace-id
  order. All runs contributing to one slot file must share identical
  timesteps — a real ensemble with per-run date ranges is rejected (exit 2)
  rather than silently misaligned (see `RISK-003` in the project plan).
- One `scalars.parquet` for every **scalar** slot across every run, using the
  long format's scalar row shape minus `timestep` (which scalars don't
  have): `trace`, `object`, `slot`, `units`, `scale`, `value`.
- The output directory is created if missing and existing target files are
  overwritten; pre-existing `.parquet` files this run does *not* produce are
  reported on stderr and left untouched — never deleted, since a wide output
  directory is something a user might reasonably add other files to.

## 7. Why dictionary encoding, row groups, and compression

- **Dictionary encoding**: `object`, `slot`, and `units` are declared as
  Arrow `dictionary<int32, utf8>` in the schema, so the encoding is
  guaranteed rather than left to the writer's heuristics. An ensemble
  repeats the same handful of object/slot/units names across every timestep
  and every trace; storing each row's actual string would waste space and
  slow down `GROUP BY object, slot` queries in tools like DuckDB, which can
  operate directly on dictionary indices instead of re-hashing strings.
  `value` is declared plain `float64` — Parquet's writer may still
  dictionary-encode it where the data happens to repeat, but nothing here
  forces that either way.
- **Row groups**: fixed at 1,000,000 rows per group. Small files stay a
  single group, but real ensembles do split: 400 traces × 60 monthly
  timesteps × 105 slots is 2,520,000 rows, or three groups. Readers use
  group boundaries to skip and to parallelize, and at that row count the
  per-group footer overhead is noise.
- **Compression**: `zstd` by default (`--compression zstd|snappy|none`).
  zstd gives noticeably better ratios than `snappy` at a CPU cost that's
  irrelevant for files this size; `snappy` is offered for pipelines that
  value decode speed over ratio, `none` for debugging with a hex viewer.

### Observed sizes

All figures below are bytes, from long-format output on a Windows MSVC
Release build. "Encoded" is the row groups' `total_byte_size` summed — the
*uncompressed* size of the encoded columns, which is what `rdf2parquet info`
prints as `bytes=`. Compare it against the `none` column, not against zstd.

The two test fixtures are far too small to say anything about compression:

| Fixture | Runs × timesteps × slots | Rows | Raw `.rdf` | zstd | Encoded |
|---|---|---|---|---|---|
| `sample_traces.rdf` | 3 × 5 × 5 | 51 | 3,764 | 4,885 | 810 |
| `sample_subset.rdf` | 2 × 4 × 5 | 28 | 2,506 | 4,371 | 662 |

> Both come out *larger* as Parquet than as raw text. The encoded column shows
> why: there are only a few hundred bytes of actual data, and everything else
> is fixed overhead that does not shrink with row count — the file footer,
> per-column-chunk statistics, the preamble key-value metadata this tool
> attaches (`rdf.package_preamble`, `rdf.run_preambles`), and Arrow's own
> `ARROW:schema` blob, a base64-encoded flatbuffer of the full schema that
> alone accounts for well over a kilobyte.

At ensemble scale that overhead vanishes. A production file of 400 traces ×
60 monthly timesteps × 105 series slots — 2,520,000 rows, 3 row groups:

| | Bytes | vs. raw |
|---|---|---|
| Raw `.rdf` | 22,453,731 | — |
| `--compression zstd` (default) | 5,885,112 | 3.82× smaller |
| `--compression snappy` | 7,100,399 | 3.16× smaller |
| `--compression none` | 14,556,665 | 1.54× smaller |
| Encoded (uncompressed) | 14,288,117 | 5.67 bytes/row |

> Note that even the uncompressed file beats the text it came from. RDF spends
> ~9 bytes on every number as ASCII digits and repeats each object/slot/units
> name once per slot block; the columnar form stores dates as `date32`, traces
> as `int32`, and those names once per dictionary. Compression then works on
> already-compact columns of like-typed values, which is why zstd finds another
> 2.4× on top. Conversion takes under two seconds.
