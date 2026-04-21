# Config-Driven Runs

PONO supports JSON config files for CLI-driven generation and physical
validation. Direct CLI flags still work and override config values where the
same setting is exposed.

## Generate

Uniform legacy sweep:

```bash
./build/fes_app generate --config config/generate_k4_uniform.json
```

Cartesian grid:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json
```

Benchmark-driven function selection with cartesian activity:

```bash
./build/fes_app generate --config config/generate_benchmark_k4_cartesian.json
```

Resume a partial generation run:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json --resume
```

Rerun only failed or timeout cases:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json --rerun failed
./build/fes_app generate --config config/generate_k4_cartesian.json --rerun timeout
```

Generation writes:

- `final_results.csv`
- `run_manifest.csv`
- `generation_summary.json`
- `detailed_infos/<HexFunc>/...`
- `tmp_eval/`

Relative `output_dir` and `--out` values are rooted under the project
`results_repo`. For example, `results_repo/activity_uniform_k4_num1` and
`activity_uniform_k4_num1` both resolve below the configured repository output
root.

## Evaluate / Benchmark

Standard physical validation:

```bash
./build/fes_app evaluate --config config/evaluate_default.json
./build/fes_app benchmark --config config/benchmark_default.json
```

Mapped four-way validation:

```bash
./build/fes_app evaluate --config config/evaluate_mapped_four_way.json
```

Evaluation writes the existing validation CSVs in the selected library
directory and adds `evaluation_summary.json`. Per-benchmark resume filtering is
not yet applied inside `InnovusBatchEvaluator`; generation has the robust
case-level manifest in this phase.

## Schema

Top-level keys:

- `command`: `generate`, `evaluate`, or `benchmark`
- `tools`: optional path overrides such as `abc_path`, `genlib_path`,
  `liberty_path`, `python_script`, and `standard_cell_csv`
- `run`: shared run controls
- `generate`: library generation settings
- `evaluate` or `benchmark`: physical validation settings

Generation activity settings:

```json
{
  "activity": {
    "mode": "cartesian",
    "levels": [0.2, 0.4, 0.6, 0.8]
  }
}
```

Explicit activity lists use one array per pattern:

```json
{
  "activity": {
    "mode": "explicit",
    "explicit": [
      [0.1, 0.2, 0.3, 0.4],
      [0.4, 0.3, 0.2, 0.1]
    ]
  }
}
```

Run controls:

```json
{
  "run": {
    "resume_policy": "resume",
    "case_timeout_ms": 600000,
    "threads": 0
  }
}
```

Supported resume policies are `run_all`, `skip_completed`, `resume`,
`rerun_failed`, and `rerun_timeout`.
