import { describe, it, expect } from 'vitest'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { parseRdf, listSlots, rdfToDataset, normalizeTimestamp, mergeRdfs } from './rdfParser.js'
import { parseCsvFile } from './parseCsv.js'

// Resolve the shared sample fixtures (also used by the Python suite, GUD-002).
const samplesDir = fileURLToPath(new URL('../../public/rw-sample-data/', import.meta.url))
const TRACES = readFileSync(samplesDir + 'sample_traces.rdf', 'utf-8')
const SUBSET = readFileSync(samplesDir + 'sample_subset.rdf', 'utf-8')

describe('parseRdf — sample_traces.rdf', () => {
  const rdf = parseRdf(TRACES)

  it('reads package meta and run count', () => {
    expect(rdf.meta.number_of_runs).toBe('3')
    expect(rdf.runs).toHaveLength(3)
  })

  it('reads 5 slots per run', () => {
    for (const run of rdf.runs) {
      expect(Object.keys(run.slots)).toHaveLength(5)
    }
  })

  it('exposes the expected slot keys', () => {
    expect(new Set(Object.keys(rdf.runs[0].slots))).toEqual(
      new Set([
        'Example Reservoir.Pool Elevation',
        'Example Reservoir.Outflow',
        'Example Reservoir.Turbine Release',
        'Run Management.Trace Historical Year',
        'Run Management.Historical Year Percent of Average',
      ])
    )
  })

  it('parses series slot values and units', () => {
    const slot = rdf.runs[0].slots['Example Reservoir.Pool Elevation']
    expect(slot.scalar).toBe(false)
    expect(slot.units).toBe('feet')
    expect(slot.values).toHaveLength(5)
    expect(slot.values[0]).toBeCloseTo(1100.0)
    expect(slot.values[4]).toBeCloseTo(1098.0)
  })

  it('flags scalar slots and reads one value', () => {
    const slot = rdf.runs[0].slots['Run Management.Trace Historical Year']
    expect(slot.scalar).toBe(true)
    expect(slot.values).toHaveLength(1)
    expect(slot.values[0]).toBeCloseTo(1988.0)
  })

  it('normalizes timestamps to ISO dates', () => {
    const times = rdf.runs[0].times
    expect(times[0]).toBe('2024-01-01')
    expect(times[times.length - 1]).toBe('2024-01-05')
    expect(times).toHaveLength(5)
  })

  it('reads trace ids from the run preamble', () => {
    expect(rdf.runs.map((r) => r.preamble.trace)).toEqual(['1', '2', '3'])
  })
})

describe('listSlots', () => {
  const rdf = parseRdf(TRACES)
  const slots = listSlots(rdf)

  it('returns metadata for every slot', () => {
    expect(slots).toHaveLength(5)
    const pool = slots.find((s) => s.key === 'Example Reservoir.Pool Elevation')
    expect(pool.units).toBe('feet')
    expect(pool.scalar).toBe(false)
    expect(pool.slot_type).toBe('SeriesSlot')
  })

  it('marks scalar slots', () => {
    const scalar = slots.find((s) => s.key === 'Run Management.Trace Historical Year')
    expect(scalar.scalar).toBe(true)
    expect(scalar.units).toBe('NONE')
  })
})

describe('normalizeTimestamp', () => {
  it('zero-pads single-digit month/day and drops the 24:00 clock', () => {
    expect(normalizeTimestamp('2025-10-2 24:00')).toBe('2025-10-02')
    expect(normalizeTimestamp('2026-1-15 24:00')).toBe('2026-01-15')
    expect(normalizeTimestamp('2026-10-1 24:00')).toBe('2026-10-01')
    // CRMMS-style annual / month-end stamp (Dec-31, NOT Jan-1).
    expect(normalizeTimestamp('2025-12-31 24:00')).toBe('2025-12-31')
  })

  it('throws a descriptive error on a malformed stamp', () => {
    expect(() => normalizeTimestamp('not-a-date')).toThrow(/Malformed RDF timestamp/)
  })
})

describe('rdfToDataset — shape parity with parseCsvFile', () => {
  const rdf = parseRdf(TRACES)
  const dataset = rdfToDataset(rdf, 'Example Reservoir.Pool Elevation')

  it('produces the parseCsvFile output shape', () => {
    expect(Object.keys(dataset).sort()).toEqual(
      ['columns', 'indexColumn', 'labelRowCount', 'labelsByColumn', 'rows'].sort()
    )
  })

  it('maps each run to a trace_<n> column (DR-01)', () => {
    expect(dataset.columns).toEqual(['trace_1', 'trace_2', 'trace_3'])
  })

  it('uses an ISO date index for daily data', () => {
    expect(dataset.indexColumn).toBe('date')
    expect(dataset.rows[0].date).toBe('2024-01-01')
    expect(dataset.rows).toHaveLength(5)
  })

  it('keys data rows by column with finite numbers', () => {
    expect(dataset.rows[0].trace_1).toBeCloseTo(1100.0)
    expect(dataset.rows[0].trace_2).toBeCloseTo(1101.0)
    expect(dataset.rows[0].trace_3).toBeCloseTo(1098.0)
  })

  it('matches the row-object structure parseCsvFile would produce', async () => {
    // Build the equivalent wide CSV and parse it — keys/index/columns should agree.
    const csv = 'date,trace_1,trace_2,trace_3\n2024-01-01,1100,1101,1098\n'
    const fromCsv = await parseCsvFile({ text: () => Promise.resolve(csv) })
    expect(Object.keys(dataset.rows[0]).sort()).toEqual(Object.keys(fromCsv.rows[0]).sort())
    expect(dataset.indexColumn).toBe(fromCsv.indexColumn)
  })

  it('injects every scalar slot as a label category keyed by Object.Slot (DR-02)', () => {
    const lbl = dataset.labelsByColumn.trace_1
    expect(lbl['Run Management.Trace Historical Year']).toBe('1988')
    expect(lbl['Run Management.Historical Year Percent of Average']).toBe('95')
    // Values vary per trace.
    expect(dataset.labelsByColumn.trace_2['Run Management.Trace Historical Year']).toBe('1995')
    expect(dataset.labelsByColumn.trace_3['Run Management.Trace Historical Year']).toBe('2003')
  })

  it('injects slot and units constant categories on every column (DR-02 / Phase 5)', () => {
    for (const col of dataset.columns) {
      expect(dataset.labelsByColumn[col].slot).toBe('Pool Elevation')
      expect(dataset.labelsByColumn[col].units).toBe('feet')
    }
  })

  it('reports labelRowCount equal to the scalar slot count', () => {
    expect(dataset.labelRowCount).toBe(2)
  })
})

describe('rdfToDataset — annual detection (DR-03)', () => {
  // Synthetic RDF with one Dec-31 stamp per distinct year (CRMMS-style).
  function annualRdf() {
    return [
      'name:Annual',
      'number_of_runs:1',
      'END_PACKAGE_PREAMBLE',
      'trace:1',
      'time_steps:3',
      'END_RUN_PREAMBLE',
      '2020-12-31 24:00',
      '2021-12-31 24:00',
      '2022-12-31 24:00',
      'object_type: Res',
      'object_name: R',
      'slot_type: SeriesSlot',
      'slot_name: Storage',
      'END_SLOT_PREAMBLE',
      'units: af',
      'scale: 1',
      '10',
      '20',
      '30',
      'END_COLUMN',
      'END_SLOT',
      'END_RUN',
      '',
    ].join('\n')
  }

  it('uses a 4-digit year-string index when one stamp per distinct year', () => {
    const rdf = parseRdf(annualRdf())
    const ds = rdfToDataset(rdf, 'R.Storage')
    expect(ds.indexColumn).toBe('year')
    expect(ds.rows.map((r) => r.year)).toEqual(['2020', '2021', '2022'])
    // String, not number — so detectIndexType treats it as numeric for int ticks.
    expect(typeof ds.rows[0].year).toBe('string')
  })
})

describe('rdfToDataset — error handling (SEC-001)', () => {
  it('throws for a non-existent slot', () => {
    const rdf = parseRdf(TRACES)
    expect(() => rdfToDataset(rdf, 'Nope.Missing')).toThrow(/not found/)
  })

  it('throws for a scalar slot chosen as the series', () => {
    const rdf = parseRdf(TRACES)
    expect(() => rdfToDataset(rdf, 'Run Management.Trace Historical Year')).toThrow(/scalar/)
  })

  it('throws on mismatched timesteps across runs', () => {
    // Craft a minimal 2-run file whose runs have differing step counts.
    const bad = [
      'name:x', 'number_of_runs:2', 'END_PACKAGE_PREAMBLE',
      'trace:1', 'time_steps:2', 'END_RUN_PREAMBLE',
      '2020-1-1 24:00', '2020-1-2 24:00',
      'object_type: R', 'object_name: R', 'slot_type: SeriesSlot', 'slot_name: S',
      'END_SLOT_PREAMBLE', 'units: af', 'scale: 1', '1', '2', 'END_COLUMN', 'END_SLOT', 'END_RUN',
      'trace:2', 'time_steps:3', 'END_RUN_PREAMBLE',
      '2020-1-1 24:00', '2020-1-2 24:00', '2020-1-3 24:00',
      'object_type: R', 'object_name: R', 'slot_type: SeriesSlot', 'slot_name: S',
      'END_SLOT_PREAMBLE', 'units: af', 'scale: 1', '1', '2', '3', 'END_COLUMN', 'END_SLOT', 'END_RUN',
      '',
    ].join('\n')
    const rdf = parseRdf(bad)
    expect(() => rdfToDataset(rdf, 'R.S')).toThrow(/timestep/i)
  })

  it('throws on a malformed slot skipped during parsing (empty values)', () => {
    // Slot with no slot_type and a value count that matches neither nts nor 1
    // hits parseSlot's warn path: values=[] and scalar=null. It slips past the
    // scalar guard (null is falsy) but must error on the length mismatch rather
    // than yield a silently-empty dataset.
    const bad = [
      'name:x', 'number_of_runs:1', 'END_PACKAGE_PREAMBLE',
      'trace:1', 'time_steps:3', 'END_RUN_PREAMBLE',
      '2020-1-1 24:00', '2020-1-2 24:00', '2020-1-3 24:00',
      'object_type: R', 'object_name: R', 'slot_name: S',
      'END_SLOT_PREAMBLE', 'units: af', 'scale: 1', '1', '2', 'END_COLUMN', 'END_SLOT', 'END_RUN',
      '',
    ].join('\n')
    const rdf = parseRdf(bad)
    expect(() => rdfToDataset(rdf, 'R.S')).toThrow(/malformed|skipped|values but/i)
  })

  it('throws when a later run has a malformed slot (value/timestep mismatch)', () => {
    const bad = [
      'name:x', 'number_of_runs:2', 'END_PACKAGE_PREAMBLE',
      'trace:1', 'time_steps:3', 'END_RUN_PREAMBLE',
      '2020-1-1 24:00', '2020-1-2 24:00', '2020-1-3 24:00',
      'object_type: R', 'object_name: R', 'slot_type: SeriesSlot', 'slot_name: S',
      'END_SLOT_PREAMBLE', 'units: af', 'scale: 1', '1', '2', '3', 'END_COLUMN', 'END_SLOT', 'END_RUN',
      'trace:2', 'time_steps:3', 'END_RUN_PREAMBLE',
      '2020-1-1 24:00', '2020-1-2 24:00', '2020-1-3 24:00',
      'object_type: R', 'object_name: R', 'slot_name: S',
      'END_SLOT_PREAMBLE', 'units: af', 'scale: 1', '1', '2', 'END_COLUMN', 'END_SLOT', 'END_RUN',
      '',
    ].join('\n')
    const rdf = parseRdf(bad)
    expect(() => rdfToDataset(rdf, 'R.S')).toThrow(/run 2.*values but|malformed|skipped/i)
  })

  it('throws a descriptive error on non-RDF text instead of crashing', () => {
    expect(() => parseRdf('just,a,csv\n1,2,3\n')).toThrow(/valid RDF file/)
  })

  it('throws when given a non-string input', () => {
    expect(() => parseRdf(null)).toThrow(/string/)
  })
})

describe('parseRdf — sample_subset.rdf', () => {
  const rdf = parseRdf(SUBSET)

  it('has 2 runs of 4 timesteps', () => {
    expect(rdf.runs).toHaveLength(2)
    for (const run of rdf.runs) {
      expect(run.times).toHaveLength(4)
    }
  })

  it('converts to a 4-row dataset', () => {
    const ds = rdfToDataset(rdf, 'Example Reservoir.Pool Elevation')
    expect(ds.columns).toEqual(['trace_1', 'trace_2'])
    expect(ds.rows).toHaveLength(4)
    expect(ds.indexColumn).toBe('date')
  })
})

describe('mergeRdfs', () => {
  function createMockRdfText({ slotName, values, times = ['2020-01-01 24:00', '2020-01-02 24:00'] }) {
    return [
      'name:x',
      'number_of_runs:1',
      'END_PACKAGE_PREAMBLE',
      'trace:1',
      `time_steps:${times.length}`,
      'END_RUN_PREAMBLE',
      ...times,
      'object_type:R',
      'object_name:Reservoir',
      'slot_type:SeriesSlot',
      `slot_name:${slotName}`,
      'END_SLOT_PREAMBLE',
      'units:cfs',
      'scale:1',
      ...values.map(String),
      'END_COLUMN',
      'END_SLOT',
      'END_RUN',
      '',
    ].join('\n')
  }

  it('merges slots from multiple RDF files and tracks slot sources', () => {
    const streamflow = parseRdf(createMockRdfText({ slotName: 'Streamflow', values: [10, 11] }))
    const releases = parseRdf(createMockRdfText({ slotName: 'Release', values: [20, 21] }))
    const merged = mergeRdfs([
      { name: 'streamflow.rdf', rdf: streamflow },
      { name: 'res.rdf', rdf: releases },
    ])
    expect(Object.keys(merged.rdf.runs[0].slots).sort()).toEqual(
      ['Reservoir.Release', 'Reservoir.Streamflow']
    )
    expect(merged.slotSources).toEqual({
      'Reservoir.Streamflow': 'streamflow.rdf',
      'Reservoir.Release': 'res.rdf',
    })
    expect(merged.duplicates).toEqual([])
  })

  it('ignores a duplicate slot whose units and values are identical', () => {
    const a = parseRdf(createMockRdfText({ slotName: 'Flow', values: [1, 2] }))
    const b = parseRdf(createMockRdfText({ slotName: 'Flow', values: [1, 2] }))
    const merged = mergeRdfs([
      { name: 'a.rdf', rdf: a },
      { name: 'b.rdf', rdf: b },
    ])
    expect(Object.keys(merged.rdf.runs[0].slots)).toEqual(['Reservoir.Flow'])
    expect(merged.slotSources['Reservoir.Flow']).toBe('a.rdf')
    expect(merged.duplicates).toEqual([
      { key: 'Reservoir.Flow', action: 'ignored-identical', files: ['a.rdf', 'b.rdf'] },
    ])
    expect(merged.rdf.warnings.some((w) => /identical/.test(w))).toBe(true)
  })

  it('keeps both versions of a conflicting duplicate slot under a suffixed key', () => {
    const a = parseRdf(createMockRdfText({ slotName: 'Flow', values: [1, 2] }))
    const b = parseRdf(createMockRdfText({ slotName: 'Flow', values: [3, 4] }))
    const merged = mergeRdfs([
      { name: 'a.rdf', rdf: a },
      { name: 'b.rdf', rdf: b },
    ])
    expect(Object.keys(merged.rdf.runs[0].slots).sort()).toEqual(
      ['Reservoir.Flow', 'Reservoir.Flow [b.rdf]']
    )
    // Original key keeps the first file's values; suffixed key holds the second's.
    expect(merged.rdf.runs[0].slots['Reservoir.Flow'].values).toEqual([1, 2])
    expect(merged.rdf.runs[0].slots['Reservoir.Flow [b.rdf]'].values).toEqual([3, 4])
    expect(merged.slotSources).toEqual({
      'Reservoir.Flow': 'a.rdf',
      'Reservoir.Flow [b.rdf]': 'b.rdf',
    })
    expect(merged.duplicates).toEqual([
      { key: 'Reservoir.Flow', action: 'kept-both', files: ['a.rdf', 'b.rdf'] },
    ])
    // Both versions remain individually plottable.
    const ds = rdfToDataset(merged.rdf, 'Reservoir.Flow [b.rdf]')
    expect(ds.rows.map((r) => r.trace_1)).toEqual([3, 4])
  })

  it('detects a duplicate of a slot that is missing from the first run', () => {
    // A slot present in only some runs of the base must still collide with an
    // incoming copy of the same key, instead of being overwritten as "new".
    const times = ['2020-01-01 24:00', '2020-01-02 24:00']
    const slot = (values) => ({ units: 'cfs', scale: 1, values, scalar: false })
    const a = {
      meta: {},
      runs: [
        { preamble: { trace: '1' }, times, slots: {} },
        { preamble: { trace: '2' }, times, slots: { 'Reservoir.Flow': slot([1, 2]) } },
      ],
      warnings: [],
    }
    const b = {
      meta: {},
      runs: [
        { preamble: { trace: '1' }, times, slots: { 'Reservoir.Flow': slot([5, 6]) } },
        { preamble: { trace: '2' }, times, slots: { 'Reservoir.Flow': slot([7, 8]) } },
      ],
      warnings: [],
    }
    const merged = mergeRdfs([
      { name: 'a.rdf', rdf: a },
      { name: 'b.rdf', rdf: b },
    ])
    // The base copy in run 2 survives under the original key.
    expect(merged.rdf.runs[1].slots['Reservoir.Flow'].values).toEqual([1, 2])
    // The incoming copy lands under the suffixed key in every run.
    expect(merged.rdf.runs[0].slots['Reservoir.Flow [b.rdf]'].values).toEqual([5, 6])
    expect(merged.rdf.runs[1].slots['Reservoir.Flow [b.rdf]'].values).toEqual([7, 8])
    expect(merged.duplicates).toEqual([
      { key: 'Reservoir.Flow', action: 'kept-both', files: ['a.rdf', 'b.rdf'] },
    ])
  })

  it('throws when file timesteps do not align', () => {
    const a = parseRdf(createMockRdfText({ slotName: 'Flow', values: [1, 2] }))
    const b = parseRdf(createMockRdfText({
      slotName: 'Release',
      values: [3, 4],
      times: ['2020-01-01 24:00', '2020-01-03 24:00'],
    }))
    expect(() => mergeRdfs([
      { name: 'a.rdf', rdf: a },
      { name: 'b.rdf', rdf: b },
    ])).toThrow(/incompatible/)
  })
})
