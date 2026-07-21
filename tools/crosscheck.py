#!/usr/bin/env python3
"""
Cross-check rdf2parquet's long-format Parquet output against the Python
reference parser (example_scripts/rdf_parser.py). TEST-005: run manually
against both fixtures once the C++ tool is built; not part of CI, since it
needs a built `rdf2parquet` binary plus pyarrow, on top of what CI already
builds.

Usage:
    python tools/crosscheck.py <input.rdf> <long.parquet> [--reference PATH]
"""

from __future__ import annotations

import argparse
import importlib.util
import math
import sys
from pathlib import Path
from typing import Optional

import pyarrow.parquet as pq

REPO = Path(__file__).resolve().parent.parent
DEFAULT_REFERENCE = REPO / "example_scripts" / "rdf_parser.py"


def load_reference(path: Path):
    spec = importlib.util.spec_from_file_location("rdf_parser_reference", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def safe_float(token) -> Optional[float]:
    """REQ-001 per-value coercion, applied here to the *reference's* output
    so it is comparable to the C++ tool. The reference itself falls back to
    keeping a whole slot's values as raw strings if any single token fails to
    parse (ASSUMPTION-001); this reproduces the per-value C++ behavior
    instead, which is the documented, deliberate deviation being verified."""
    try:
        v = float(token)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def reference_rows(rdf: dict) -> set:
    """(trace, object, slot, timestep_or_None, units, value_or_None) rows,
    matching the long Parquet schema minus `scale` — scale is stored raw and
    never applied to `value` (REQ-001), so it does not affect row identity
    for this comparison."""
    rows = set()
    for run in rdf["runs"]:
        trace_raw = run["preamble"].get("trace")
        try:
            trace_id = int(trace_raw)
        except (TypeError, ValueError):
            trace_id = None  # ordinal fallback (REQ-002) not reproduced here; dropped below
        for slot in run["slots"].values():
            if slot.get("scalar") is None:
                continue  # excluded (warn+skip) slot, same as the C++ tool (REQ-006)
            object_name = slot.get("object_name", "")
            slot_name = slot.get("slot_name", "")
            units = slot.get("units", "")
            raw_values = slot.get("values", [])
            if slot["scalar"]:
                value = safe_float(raw_values[0]) if raw_values else None
                rows.add((trace_id, object_name, slot_name, None, units, value))
            else:
                for ts, raw in zip(run["times"], raw_values):
                    rows.add((trace_id, object_name, slot_name, ts, units, safe_float(raw)))
    return rows


def parquet_rows(path: Path) -> set:
    table = pq.read_table(path)
    cols = {name: table.column(name).to_pylist() for name in table.schema.names}
    rows = set()
    for i in range(table.num_rows):
        ts = cols["timestep"][i]
        rows.add((
            cols["trace"][i],
            cols["object"][i],
            cols["slot"][i],
            ts.isoformat() if ts is not None else None,
            cols["units"][i],
            cols["value"][i],
        ))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rdf_file", type=Path)
    parser.add_argument("parquet_file", type=Path)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    args = parser.parse_args()

    reference = load_reference(args.reference)
    rdf = reference.parse_rdf(args.rdf_file)

    expected = reference_rows(rdf)
    # Rows whose trace id the preamble didn't resolve can't be compared
    # without reimplementing the C++ tool's ordinal fallback here; drop them
    # from both sides rather than reporting a false mismatch.
    expected = {r for r in expected if r[0] is not None}

    actual = parquet_rows(args.parquet_file)

    missing = expected - actual
    extra = actual - expected

    if not missing and not extra:
        print(f"OK: {len(actual)} rows match between {args.rdf_file} and {args.parquet_file}")
        return 0

    print(f"MISMATCH: {len(missing)} missing, {len(extra)} extra "
          f"(of {len(expected)} expected rows)")
    for row in sorted(missing, key=repr)[:10]:
        print(f"  missing: {row}")
    for row in sorted(extra, key=repr)[:10]:
        print(f"  extra:   {row}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
