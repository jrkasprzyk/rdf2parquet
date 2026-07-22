# Sample RiverWare data

Input files used for manual testing and for the size measurements in
[`docs/FORMAT.md`](../../docs/FORMAT.md).

| File | Origin | Scale |
|---|---|---|
| `sample_traces.rdf` | synthetic, written for this project | 3 traces × 5 timesteps × 5 slots |
| `sample_subset.rdf` | synthetic, written for this project | 2 traces × 4 timesteps × 5 slots |
| `res.rdf` | real RiverWare output | 400 traces × 60 monthly timesteps × 105 series slots |

The two `sample_*.rdf` files are also the test-suite fixtures; the copies under
`tests/fixtures/` are what the Catch2 tests read. `sample_commands.bat` holds
example invocations.

`res.rdf` is 2,520,000 rows of genuine model output. Including this file is helpful
because the synthetic samples are too small to demonstrate the benefit of compression. 

Note: `rdf2parquet convert` copies the **entire** package preamble into the output's 
Parquet key-value metadata as `rdf.package_preamble`, and each run preamble into 
`rdf.run_preambles`.
