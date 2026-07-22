# Test fixtures

`sample_traces.rdf` and `sample_subset.rdf` are synthetic RiverWare-style
sample data created for this project. They exercise the RDF text format
(package preamble, run preambles, timestep lists, series and scalar slot
blocks) without containing any real reservoir model data.

Copied from `public/rw-sample-data/` at the repository root, which also holds
`res.rdf` — real RiverWare output, far larger than these fixtures — and
`sample_commands.bat`, which runs every rdf2parquet subcommand against all
three for ad hoc manual testing.
