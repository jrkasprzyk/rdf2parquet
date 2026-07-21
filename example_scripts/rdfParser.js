// RiverWare RDF parser — faithful JS port of scripts/rdf_parser.py.
//
// RDF format (text, one token per line):
//   Package preamble  key:value ... END_PACKAGE_PREAMBLE
//   Per run:
//     Run preamble    key:value ... END_RUN_PREAMBLE
//     Timestep list   YYYY-M-D 24:00  (time_steps lines)
//     Per slot:
//       Slot preamble key:value ... END_SLOT_PREAMBLE
//       units line
//       scale line
//       values        (time_steps lines for series; 1 line for scalar)
//       END_COLUMN
//       END_SLOT
//     END_RUN
//
// Pure string operation: no filesystem access. The browser reads the file via
// File.text() and hands the text to parseRdf(). Malformed input throws a
// descriptive Error rather than crashing (SEC-001).
//
// See also rdfToDataset() in this module and src/lib/csvExport.js.

/**
 * Convert 'YYYY-M-D 24:00' → ISO date 'YYYY-MM-DD'.
 * RiverWare 24:00 means end-of-day; we keep the calendar date as-is (matching
 * the Python reference _normalize_ts). Single-digit month/day are zero-padded.
 *
 * @param {string} raw
 * @returns {string}
 */
export function normalizeTimestamp(raw) {
  const datePart = String(raw).trim().split(/\s+/)[0]
  const m = /^(\d{4})-(\d{1,2})-(\d{1,2})$/.exec(datePart)
  if (!m) {
    throw new Error(`Malformed RDF timestamp: "${raw}"`)
  }
  const year = m[1]
  const month = m[2].padStart(2, '0')
  const day = m[3].padStart(2, '0')
  return `${year}-${month}-${day}`
}

/**
 * Read key:value lines from pos until endMarker (exclusive).
 * Returns { result, pos }; the endMarker line is consumed.
 */
function parsePreamble(lines, pos, endMarker) {
  const result = {}
  while (pos < lines.length) {
    const line = lines[pos]
    pos += 1
    if (line === endMarker) break
    const idx = line.indexOf(':')
    if (idx === -1) {
      result[line.trim()] = null
    } else {
      const key = line.slice(0, idx).trim()
      const value = line.slice(idx + 1).trim()
      result[key] = value
    }
  }
  return { result, pos }
}

// Parse a bare "key: value" or just "value" line — return the value portion.
function bareValue(line) {
  return line.includes(':') ? line.slice(line.indexOf(':') + 1).trim() : line.trim()
}

// Coerce a token to a finite number, or null if not finite (SEC-002: no
// NaN/Infinity leak into the dataset).
function safeFloat(token) {
  const n = Number(token)
  return Number.isFinite(n) ? n : null
}

/**
 * Parse one slot block starting at pos (first line = object_type or similar).
 */
function parseSlot(lines, pos, nts, warn) {
  const parsed = parsePreamble(lines, pos, 'END_SLOT_PREAMBLE')
  const slot = parsed.result
  pos = parsed.pos

  // units and scale are bare single-value lines (no key prefix).
  if (pos + 1 >= lines.length) {
    throw new Error('Unexpected end of RDF: missing units/scale lines for a slot.')
  }
  const unitsLine = lines[pos]; pos += 1
  const scaleLine = lines[pos]; pos += 1

  slot.units = bareValue(unitsLine)
  slot.scale = bareValue(scaleLine)

  // Find next END_COLUMN to determine scalar vs series.
  let ecIdx = -1
  for (let i = pos; i < lines.length; i++) {
    if (lines[i] === 'END_COLUMN') { ecIdx = i; break }
  }
  if (ecIdx === -1) {
    throw new Error(`END_COLUMN not found after position ${pos}.`)
  }

  const nValues = ecIdx - pos
  const slotType = (slot.slot_type || '').trim().toLowerCase()
  const isSeriesSlot = slotType.includes('series')
  const isScalarSlot = slotType.includes('scalar')

  if (isSeriesSlot) {
    slot.scalar = false
  } else if (isScalarSlot) {
    slot.scalar = true
  } else if (nValues === nts) {
    slot.scalar = false
  } else if (nValues === 1) {
    slot.scalar = true
  } else {
    warn(
      `Slot '${slot.object_name}.${slot.slot_name}': expected ${nts} or 1 ` +
      `values, found ${nValues}. Skipping.`
    )
    slot.values = []
    slot.scalar = null
    pos = ecIdx + 1
    if (pos < lines.length && lines[pos] === 'END_SLOT') pos += 1
    return { slot, pos }
  }

  const rawValues = lines.slice(pos, pos + nValues)
  // Coerce to numbers; if any value is non-numeric, keep all as raw strings
  // (mirrors the Python try/except over the whole list).
  const nums = rawValues.map(safeFloat)
  slot.values = nums.every((n) => n !== null) ? nums : rawValues

  pos = ecIdx + 1
  if (pos < lines.length && lines[pos] === 'END_SLOT') pos += 1

  return { slot, pos }
}

/**
 * Parse one run block. pos points to the first line after END_PACKAGE_PREAMBLE
 * (or after the previous END_RUN).
 */
function parseRun(lines, pos, warn) {
  const run = {}

  const pre = parsePreamble(lines, pos, 'END_RUN_PREAMBLE')
  run.preamble = pre.result
  pos = pre.pos

  const nts = parseInt(run.preamble.time_steps ?? run.preamble.timesteps ?? '0', 10) || 0

  // Read timestep list.
  const rawTimes = lines.slice(pos, pos + nts)
  run.times = rawTimes.map(normalizeTimestamp)
  pos += nts

  // Read slots until END_RUN.
  run.slots = {}
  while (pos < lines.length) {
    if (lines[pos] === 'END_RUN') { pos += 1; break }
    const res = parseSlot(lines, pos, nts, warn)
    pos = res.pos
    const slot = res.slot
    const key = `${slot.object_name ?? ''}.${slot.slot_name ?? ''}`
    run.slots[key] = slot
  }

  return { run, pos }
}

/**
 * Parse a RiverWare RDF file from its text content.
 *
 * @param {string} text
 * @returns {{ meta: Record<string,string|null>, runs: object[], warnings: string[] }}
 * @throws {Error} on malformed input (SEC-001 — descriptive error, no crash).
 */
export function parseRdf(text) {
  if (typeof text !== 'string') {
    throw new Error('parseRdf expects RDF file text as a string.')
  }

  // Split on newlines and strip carriage returns from Windows line endings.
  const lines = text.split('\n').map((ln) => ln.replace(/\r$/, ''))

  const warnings = []
  const warn = (msg) => {
    warnings.push(msg)
    if (typeof console !== 'undefined') console.warn(msg)
  }

  let pos = 0
  const metaParsed = parsePreamble(lines, pos, 'END_PACKAGE_PREAMBLE')
  const meta = metaParsed.result
  pos = metaParsed.pos

  if (!('number_of_runs' in meta)) {
    throw new Error(
      'Not a valid RDF file: missing END_PACKAGE_PREAMBLE / number_of_runs. ' +
      'Make sure this is a RiverWare .rdf export.'
    )
  }

  const expectedRuns = parseInt(meta.number_of_runs ?? '0', 10) || 0
  const runs = []

  while (runs.length < expectedRuns && pos < lines.length) {
    const res = parseRun(lines, pos, warn)
    pos = res.pos
    runs.push(res.run)
  }

  if (runs.length === 0) {
    throw new Error('RDF file contains no runs.')
  }

  return { meta, runs, warnings }
}

/**
 * Return slot metadata list from the first run (all runs share the same slots).
 *
 * @param {{ runs: object[] }} rdf
 * @returns {Array<{key, object_name, slot_name, object_type, slot_type, units, scale, scalar}>}
 */
export function listSlots(rdf) {
  if (!rdf || !rdf.runs || !rdf.runs.length) return []
  const slots = rdf.runs[0].slots
  return Object.entries(slots).map(([key, s]) => ({
    key,
    object_name: s.object_name ?? '',
    slot_name: s.slot_name ?? '',
    object_type: s.object_type ?? '',
    slot_type: s.slot_type ?? '',
    units: s.units ?? '',
    scale: s.scale ?? '',
    scalar: s.scalar,
  }))
}

// Two copies of a slot are interchangeable when units and every value agree
// in a given run. Used to silently drop identical duplicates during merge.
function slotValuesEqual(a, b) {
  if (!a || !b) return false
  if (String(a.units ?? '') !== String(b.units ?? '')) return false
  const av = a.values || []
  const bv = b.values || []
  if (av.length !== bv.length) return false
  for (let i = 0; i < av.length; i++) {
    if (av[i] !== bv[i]) return false
  }
  return true
}

/**
 * Merge multiple parsed RDF payloads from the same model run into one logical
 * RDF object by unioning slots across files.
 *
 * Duplicate slot keys are not an error: an identical copy (same units and
 * values in every run) is dropped with a warning, while a conflicting copy is
 * kept under a source-suffixed key (`Object.Slot [file.rdf]`) so the user can
 * pick either version from the slot list.
 *
 * @param {Array<{name:string, rdf:{ meta:object, runs:object[], warnings?:string[] }}>} inputs
 * @returns {{ rdf:{ meta:object, runs:object[], warnings:string[] },
 *            slotSources:Record<string,string>,
 *            duplicates:Array<{key:string, action:'ignored-identical'|'kept-both', files:string[]}> }}
 */
export function mergeRdfs(inputs) {
  if (!Array.isArray(inputs) || inputs.length === 0) {
    throw new Error('At least one RDF input is required.')
  }

  const [{ name: firstName, rdf: firstRdf }] = inputs
  const base = {
    meta: { ...(firstRdf.meta || {}) },
    runs: firstRdf.runs.map((run) => ({
      ...run,
      preamble: { ...(run.preamble || {}) },
      times: [...(run.times || [])],
      slots: { ...(run.slots || {}) },
    })),
    warnings: Array.isArray(firstRdf.warnings) ? [...firstRdf.warnings] : [],
  }
  const slotSources = {}
  for (const slotKey of Object.keys(base.runs[0]?.slots || {})) {
    slotSources[slotKey] = firstName
  }
  const duplicates = []

  for (let fileIndex = 1; fileIndex < inputs.length; fileIndex++) {
    const { name, rdf } = inputs[fileIndex]
    if (!rdf?.runs?.length) {
      throw new Error(`"${name}" contains no runs.`)
    }
    if (rdf.runs.length !== base.runs.length) {
      throw new Error(
        `RDF files are incompatible: "${firstName}" has ${base.runs.length} runs but ` +
        `"${name}" has ${rdf.runs.length}.`
      )
    }
    for (let r = 0; r < base.runs.length; r++) {
      const expectedRun = base.runs[r]
      const incomingRun = rdf.runs[r]
      const expectedTrace = String(expectedRun.preamble?.trace ?? r + 1)
      const incomingTrace = String(incomingRun.preamble?.trace ?? r + 1)
      if (expectedTrace !== incomingTrace) {
        throw new Error(
          `RDF files are incompatible: run ${r + 1} trace id ${incomingTrace} in "${name}" ` +
          `does not match ${expectedTrace}.`
        )
      }
      if (incomingRun.times.length !== expectedRun.times.length) {
        throw new Error(
          `RDF files are incompatible: run ${r + 1} in "${name}" has ${incomingRun.times.length} ` +
          `timesteps, expected ${expectedRun.times.length}.`
        )
      }
      for (let t = 0; t < expectedRun.times.length; t++) {
        if (incomingRun.times[t] !== expectedRun.times[t]) {
          throw new Error(
            `RDF files are incompatible: run ${r + 1} timestep ${t + 1} in "${name}" ` +
            `(${incomingRun.times[t]}) does not match ${expectedRun.times[t]}.`
          )
        }
      }
    }

    // Resolve duplicate slot keys before merging. The decision must be made
    // once per key (not per run) so the merged slots stay uniform across runs:
    // identical everywhere → drop; any difference → keep both, with the
    // incoming copy under a source-suffixed key.
    const incomingKeys = new Set()
    for (const run of rdf.runs) {
      for (const slotKey of Object.keys(run.slots || {})) incomingKeys.add(slotKey)
    }
    // Collision checks must consult every run's slot catalog, not just run 0:
    // a slot can be present in only some runs of a file, and treating such a
    // key as "new" would silently overwrite the existing copy in those runs.
    const baseKeys = new Set()
    for (const run of base.runs) {
      for (const slotKey of Object.keys(run.slots || {})) baseKeys.add(slotKey)
    }
    const keyMap = {}
    for (const slotKey of incomingKeys) {
      if (!baseKeys.has(slotKey)) {
        keyMap[slotKey] = slotKey
        continue
      }
      const prior = slotSources[slotKey] || firstName
      const identical = base.runs.every((run, r) =>
        slotValuesEqual(run.slots[slotKey], rdf.runs[r].slots?.[slotKey])
      )
      if (identical) {
        keyMap[slotKey] = null
        duplicates.push({ key: slotKey, action: 'ignored-identical', files: [prior, name] })
        base.warnings.push(
          `Slot "${slotKey}" in "${name}" is identical to the copy already loaded ` +
          `from "${prior}"; the duplicate was ignored.`
        )
      } else {
        let renamed = `${slotKey} [${name}]`
        let n = 2
        while (baseKeys.has(renamed)) {
          renamed = `${slotKey} [${name}] (${n})`
          n += 1
        }
        keyMap[slotKey] = renamed
        duplicates.push({ key: slotKey, action: 'kept-both', files: [prior, name] })
        base.warnings.push(
          `Slot "${slotKey}" appears in both "${prior}" and "${name}" with different ` +
          `values; both were kept — the copy from "${name}" is listed as "${renamed}".`
        )
      }
    }

    for (let r = 0; r < base.runs.length; r++) {
      const expectedRun = base.runs[r]
      const incomingRun = rdf.runs[r]
      for (const [slotKey, slot] of Object.entries(incomingRun.slots || {})) {
        const mapped = keyMap[slotKey]
        if (mapped === null) continue
        expectedRun.slots[mapped] = slot
        if (!(mapped in slotSources)) slotSources[mapped] = name
      }
    }
    if (Array.isArray(rdf.warnings)) {
      base.warnings.push(...rdf.warnings)
    }
  }

  return { rdf: base, slotSources, duplicates }
}

// ---------------------------------------------------------------------------
// rdfToDataset — TASK-017 / DR-01 / DR-02 / DR-03
// ---------------------------------------------------------------------------

// Detect annual cadence: one timestamp per distinct year, all sharing the same
// month+day (e.g. CRMMS annual stamps are Dec-31, NOT Jan-1). Returns true when
// every ISO date has the same MM-DD suffix and the years are strictly the
// distinct set in order (one stamp per year).
function isAnnual(times) {
  if (!times.length) return false
  const monthDay = times[0].slice(5) // 'MM-DD'
  const years = []
  for (const t of times) {
    if (t.slice(5) !== monthDay) return false
    years.push(t.slice(0, 4))
  }
  // One timestamp per distinct year — no repeats.
  const distinct = new Set(years)
  return distinct.size === years.length && years.length > 1
}

/**
 * Build a viewer dataset from a parsed RDF + a chosen series slot.
 * Output shape matches parseCsvFile: { columns, indexColumn, rows,
 * labelsByColumn, labelRowCount }.
 *
 * DR-01: each run → column `trace_<n>` (using the run's `trace` preamble id,
 *        falling back to the ordinal). Timesteps must be identical across runs.
 * DR-03: annual → indexColumn 'year' with a 4-digit year STRING; else 'date'
 *        with the ISO date string.
 * DR-02: every scalar slot → a label category keyed by its full Object.Slot
 *        name; plus injected `slot` and `units` categories on every column.
 *
 * @param {{ runs: object[] }} rdf
 * @param {string} slotKey
 * @returns {{ columns:string[], indexColumn:string, rows:object[],
 *            labelsByColumn:object, labelRowCount:number }}
 */
export function rdfToDataset(rdf, slotKey) {
  if (!rdf || !rdf.runs || !rdf.runs.length) {
    throw new Error('RDF has no runs to convert.')
  }
  const runs = rdf.runs

  // Validate slot exists.
  if (!(slotKey in runs[0].slots)) {
    const available = Object.keys(runs[0].slots).join(', ')
    throw new Error(`Slot "${slotKey}" not found. Available slots: ${available}`)
  }

  const chosen = runs[0].slots[slotKey]
  if (chosen.scalar) {
    throw new Error(
      `Slot "${slotKey}" is a scalar slot (one value per run, not a timeseries). ` +
      'Pick a series slot to plot.'
    )
  }

  // DR-01: assert identical timesteps across runs (length + first/last date).
  const refTimes = runs[0].times
  if (!refTimes.length) {
    throw new Error(`Slot "${slotKey}": run 1 has no timesteps.`)
  }
  // A slot that failed parseSlot's value-count check has scalar=null and
  // values=[] but still appears in run.slots. It slips past the scalar guard
  // above (null is falsy), so assert its length matches the timesteps to
  // surface a descriptive error instead of silently emitting empty rows.
  if (chosen.values.length !== refTimes.length) {
    throw new Error(
      `Slot "${slotKey}": run 1 has ${chosen.values.length} values but ` +
      `${refTimes.length} timesteps. The slot may be malformed or was skipped ` +
      'during parsing.'
    )
  }
  for (let i = 1; i < runs.length; i++) {
    const t = runs[i].times
    const trace = runs[i].preamble?.trace ?? i + 1
    if (t.length !== refTimes.length) {
      throw new Error(
        `Run ${i + 1} (trace=${trace}) has ${t.length} timesteps but run 1 has ` +
        `${refTimes.length}. Identical timesteps across runs are required.`
      )
    }
    if (t[0] !== refTimes[0] || t[t.length - 1] !== refTimes[refTimes.length - 1]) {
      throw new Error(
        `Run ${i + 1} (trace=${trace}) timestep range ${t[0]}..${t[t.length - 1]} ` +
        `differs from run 1 (${refTimes[0]}..${refTimes[refTimes.length - 1]}). ` +
        'Identical timesteps across runs are required.'
      )
    }
    if (!(slotKey in runs[i].slots)) {
      throw new Error(`Slot "${slotKey}" missing from run ${i + 1} (trace=${trace}).`)
    }
    const vals = runs[i].slots[slotKey].values
    if (vals.length !== refTimes.length) {
      throw new Error(
        `Slot "${slotKey}": run ${i + 1} (trace=${trace}) has ${vals.length} ` +
        `values but ${refTimes.length} timesteps. The slot may be malformed or ` +
        'was skipped during parsing.'
      )
    }
  }

  // Column id per run (DR-01).
  const traceId = (run, ordinal) => `trace_${run.preamble?.trace ?? ordinal}`
  const columns = runs.map((run, i) => traceId(run, i + 1))

  // Detect annual cadence and build the index values (DR-03).
  const annual = isAnnual(refTimes)
  const indexColumn = annual ? 'year' : 'date'
  const indexValues = annual ? refTimes.map((t) => t.slice(0, 4)) : refTimes

  // Build rows: one object per timestep keyed by column name.
  const nRows = chosen.values.length
  const rows = []
  for (let r = 0; r < nRows; r++) {
    const obj = { [indexColumn]: indexValues[r] ?? '' }
    for (let c = 0; c < runs.length; c++) {
      const vals = runs[c].slots[slotKey].values
      const v = r < vals.length ? vals[r] : NaN
      const num = typeof v === 'number' ? v : Number(v)
      obj[columns[c]] = Number.isFinite(num) ? num : NaN
    }
    rows.push(obj)
  }

  // DR-02: scalar label categories keyed by full Object.Slot name.
  const scalarKeys = Object.keys(runs[0].slots).filter((k) => runs[0].slots[k].scalar)
  const labelsByColumn = {}
  for (const col of columns) labelsByColumn[col] = {}

  for (const sk of scalarKeys) {
    for (let c = 0; c < runs.length; c++) {
      const slot = runs[c].slots[sk]
      const vals = slot ? slot.values : []
      labelsByColumn[columns[c]][sk] = vals.length ? String(vals[0]) : ''
    }
  }

  // DR-02: inject `slot` and `units` constant categories on every column for
  // Phase 5 y-axis label derivation.
  const slotName = chosen.slot_name ?? slotKey
  const units = chosen.units ?? ''
  for (const col of columns) {
    labelsByColumn[col].slot = String(slotName)
    labelsByColumn[col].units = String(units)
  }

  return {
    columns,
    indexColumn,
    rows,
    labelsByColumn,
    labelRowCount: scalarKeys.length,
  }
}
