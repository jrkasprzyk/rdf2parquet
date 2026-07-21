"""
RiverWare RDF file parser.

RDF format (text, one token per line):
  Package preamble  key:value ... END_PACKAGE_PREAMBLE
  Per run:
    Run preamble    key:value ... END_RUN_PREAMBLE
    Timestep list   YYYY-M-D 24:00  (time_steps lines)
    Per slot:
      Slot preamble key:value ... END_SLOT_PREAMBLE
      units line
      scale line
      values        (time_steps lines for series; 1 line for scalar)
      END_COLUMN
      END_SLOT
    END_RUN
"""

from __future__ import annotations

import sys
from datetime import datetime, timedelta
from pathlib import Path


def _normalize_ts(raw: str) -> str:
    """Convert 'YYYY-M-D 24:00' → ISO date 'YYYY-MM-DD'."""
    date_part = raw.split()[0]
    dt = datetime.strptime(date_part, "%Y-%m-%d")
    # RiverWare 24:00 = end of that calendar day — keep as-is
    return dt.strftime("%Y-%m-%d")


def _parse_preamble(lines: list[str], pos: int, end_marker: str) -> tuple[dict, int]:
    """
    Read key:value lines from pos until end_marker (exclusive).
    Returns (dict, new_pos).  end_marker line is consumed.
    If end_marker == 1 (int), reads exactly one line.
    """
    result: dict = {}
    while pos < len(lines):
        line = lines[pos]
        pos += 1
        if line == end_marker:
            break
        parts = line.split(":", 1)
        key = parts[0].strip()
        value = parts[1].strip() if len(parts) > 1 else None
        result[key] = value
        if end_marker == 1:  # sentinel: read exactly one line
            break
    return result, pos


def _parse_slot(lines: list[str], pos: int, nts: int) -> tuple[dict, int]:
    """Parse one slot block starting at pos (first line = object_type or similar)."""
    slot, pos = _parse_preamble(lines, pos, "END_SLOT_PREAMBLE")

    # units and scale are bare single-value lines (no key prefix)
    units_line = lines[pos]; pos += 1
    scale_line = lines[pos]; pos += 1

    # Parse bare "key: value" or just "value"
    def _bare_value(line: str) -> str:
        return line.split(":", 1)[-1].strip() if ":" in line else line.strip()

    slot["units"] = _bare_value(units_line)
    slot["scale"] = _bare_value(scale_line)

    # Find next END_COLUMN to determine scalar vs series
    ec_idx = next(
        (i for i in range(pos, len(lines)) if lines[i] == "END_COLUMN"),
        None,
    )
    if ec_idx is None:
        raise ValueError(f"END_COLUMN not found after position {pos}")

    n_values = ec_idx - pos
    slot_type = (slot.get("slot_type") or "").strip().lower()
    is_series_slot = "series" in slot_type
    is_scalar_slot = "scalar" in slot_type

    if is_series_slot:
        slot["scalar"] = False
    elif is_scalar_slot:
        slot["scalar"] = True
    elif n_values == nts:
        slot["scalar"] = False
    elif n_values == 1:
        slot["scalar"] = True
    else:
        import warnings
        warnings.warn(
            f"Slot '{slot.get('object_name')}.{slot.get('slot_name')}': "
            f"expected {nts} or 1 values, found {n_values}. Skipping."
        )
        slot["values"] = []
        slot["scalar"] = None
        pos = ec_idx + 1  # skip to after END_COLUMN
        if pos < len(lines) and lines[pos] == "END_SLOT":
            pos += 1
        return slot, pos

    raw_values = lines[pos : pos + n_values]
    try:
        slot["values"] = [float(v) for v in raw_values]
    except ValueError:
        slot["values"] = raw_values  # keep as strings if not numeric

    pos = ec_idx + 1  # advance past END_COLUMN
    if pos < len(lines) and lines[pos] == "END_SLOT":
        pos += 1

    return slot, pos


def _parse_run(lines: list[str], pos: int) -> tuple[dict, int]:
    """Parse one run block.  pos points to first line after END_PACKAGE_PREAMBLE
    (or after previous END_RUN)."""
    run: dict = {}

    preamble, pos = _parse_preamble(lines, pos, "END_RUN_PREAMBLE")
    run["preamble"] = preamble

    nts = int(preamble.get("time_steps") or preamble.get("timesteps") or 0)

    # Read timestep list
    raw_times = lines[pos : pos + nts]
    run["times"] = [_normalize_ts(t) for t in raw_times]
    pos += nts

    # Read slots until END_RUN
    run["slots"] = {}
    while pos < len(lines):
        if lines[pos] == "END_RUN":
            pos += 1
            break

        slot, pos = _parse_slot(lines, pos, nts)
        key = f"{slot.get('object_name', '')}.{slot.get('slot_name', '')}"
        run["slots"][key] = slot

    return run, pos


def parse_rdf(path: str | Path) -> dict:
    """
    Parse a RiverWare RDF file.

    Returns:
        {
            "meta": {key: value, ...},
            "runs": [
                {
                    "preamble": {key: value, ...},
                    "times": ["YYYY-MM-DD", ...],
                    "slots": {
                        "ObjectName.SlotName": {
                            "object_type": ..., "object_name": ...,
                            "slot_type": ..., "slot_name": ...,
                            "units": ..., "scale": ...,
                            "scalar": bool,
                            "values": [float, ...]
                        },
                        ...
                    }
                },
                ...
            ]
        }
    """
    path = Path(path)
    if path.suffix.lower() != ".rdf":
        raise ValueError(f"{path} does not appear to be an rdf file.")
    if not path.exists():
        raise FileNotFoundError(f"{path} does not exist.")

    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    # Strip carriage returns from Windows line endings
    lines = [ln.rstrip("\r") for ln in lines]

    pos = 0
    meta, pos = _parse_preamble(lines, pos, "END_PACKAGE_PREAMBLE")

    expected_runs = int(meta.get("number_of_runs") or 0)
    runs: list[dict] = []

    while len(runs) < expected_runs and pos < len(lines):
        run, pos = _parse_run(lines, pos)
        runs.append(run)

    return {"meta": meta, "runs": runs}


def list_slots(rdf: dict) -> list[dict]:
    """Return slot metadata list from first run (all runs share the same slots)."""
    if not rdf["runs"]:
        return []
    return [
        {
            "key": key,
            "object_name": s.get("object_name", ""),
            "slot_name": s.get("slot_name", ""),
            "object_type": s.get("object_type", ""),
            "slot_type": s.get("slot_type", ""),
            "units": s.get("units", ""),
            "scale": s.get("scale", ""),
            "scalar": s.get("scalar"),
        }
        for key, s in rdf["runs"][0]["slots"].items()
    ]
