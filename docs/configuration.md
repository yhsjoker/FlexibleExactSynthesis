# Configuration

PONO supports strict JSON config files for generation, physical validation, and
benchmark-oriented workflows. Direct CLI flags still work and override config
values when the same setting is exposed.

The runtime parser does not support comments or trailing commas. Keep runnable
files in `config/*.json` as strict JSON. Human-readable commented examples are
kept separately in `config/examples/*.jsonc`, and the parameter reference lives
in `config/README.md`.

## Path Semantics

Path resolution is project-root based:

- absolute paths are used as-is
- relative input, resource, and dependency paths in JSON are resolved below the
  compiled project root
- relative `generate.output_dir` and CLI `--out` values are rooted under
  `<project_root>/results_repo`
- paths are not resolved relative to the current working directory, build
  directory, or config file directory

This remains true when running from `build/`, for example:

```bash
cd <project_root>/build
./fes_app generate --config ../config/build_library.json
```

Example:

```json
{
  "dependencies": {
    "abc_path": "third_party/abc/abc",
    "python_script": "scripts/single_power_run.py",
    "genlib_path": "resources/nangate_45nm.genlib"
  },
  "generate": {
    "output_dir": "activity_uniform_k4_num1"
  }
}
```

resolves to:

- `<project_root>/third_party/abc/abc`
- `<project_root>/scripts/single_power_run.py`
- `<project_root>/resources/nangate_45nm.genlib`
- `<project_root>/results_repo/activity_uniform_k4_num1`

`results_repo/<name>` is still accepted for compatibility and resolves to the
same output-root location, but new configs should prefer the shorter `<name>`
form for `output_dir`.

## Current Config Set

Runnable configs:

- `config/build_library.json`: primary full-library build
- `config/generate_k4_uniform.json`: legacy uniform sweep generation
- `config/generate_k4_cartesian.json`: cartesian activity generation
- `config/generate_k4_explicit.json`: explicit activity-list generation
- `config/generate_benchmark_k4_cartesian.json`: benchmark-driven generation
- `config/benchmark_default.json`: standard benchmark physical validation
- `config/evaluate_default.json`: standard evaluate physical validation
- `config/evaluate_mapped_four_way.json`: mapped four-way evaluation
- `config/deployment_example.json`: dependency-path deployment template

Commented `.jsonc` equivalents are available under `config/examples/`. They
are documentation only and should not be passed to `fes_app --config`.

The benchmark/evaluate configs use `benchmark_dir: "benchmarks"` as a portable
placeholder. Replace it with an absolute benchmark directory or with a
project-root-relative dataset directory before running full validation jobs.

## Primary Full-Library Build

Use this as the main generation entry point:

```bash
./build/fes_app generate --config config/build_library.json
```

The current full-library representation is exhaustive NPN-canonical generation:
`function_source: "exhaustive"` enumerates all raw truth tables for `k`, maps
them to NPN canonical representatives, aggregates representative frequency, and
sorts the representatives deterministically. `num_functions: 0` means no
positive truncation, so for exhaustive mode it selects the full NPN-canonical
representative set for the configured `k`.

For the shipped primary config, `k: 4` means the exhaustive raw universe is
`2^(2^4) = 65536` truth tables before NPN canonicalization. The config uses
cartesian activity levels `[0.3, 0.7]`, so each selected representative gets
`2^4 = 16` activity patterns. The output directory is
`<project_root>/results_repo/library_k4_npn_cartesian`.

The shipped config currently keeps `generate.verify: false`.

The config also includes a best-effort memory budget:

```json
{
  "run": {
    "threads": 1,
    "max_worker_memory_mb": 1024,
    "max_total_memory_mb": 4096
  }
}
```

The shipped config currently requests one generation worker. When both memory
fields are positive, generation caps effective workers to
`floor(max_total_memory_mb / max_worker_memory_mb)`, with a minimum of one
worker. This is not strict per-thread memory enforcement; it is a practical
anti-OOM scheduling guard.

## Generate

Uniform legacy sweep:

```bash
./build/fes_app generate --config config/generate_k4_uniform.json
```

Cartesian grid:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json
```

Explicit activity vectors:

```bash
./build/fes_app generate --config config/generate_k4_explicit.json
```

Benchmark-driven function selection with cartesian activity:

```bash
./build/fes_app generate --config config/generate_benchmark_k4_cartesian.json
```

Resume or rerun selected statuses:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json --resume
./build/fes_app generate --config config/generate_k4_cartesian.json --rerun failed
./build/fes_app generate --config config/generate_k4_cartesian.json --rerun timeout
```

Generation writes under the selected output directory:

- `final_results.csv`
- `run_manifest.csv`
- `generation_summary.json`
- `detailed_infos/<HexFunc>/...`
- `tmp_eval/`

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

Resume or rerun selected statuses:

```bash
./build/fes_app benchmark --config config/benchmark_default.json --resume
./build/fes_app benchmark --config config/benchmark_default.json --rerun failed
./build/fes_app evaluate --config config/evaluate_default.json --rerun timeout
```

Evaluation preserves the legacy validation CSVs in the selected library
directory and adds:

- `evaluation_manifest.csv`
- `evaluation_cases.csv`
- `evaluation_summary.json`

Per-benchmark resume/rerun filtering is applied inside
`InnovusBatchEvaluator`.

## Schema Summary

Top-level keys:

- `command`: `generate`, `evaluate`, or `benchmark`
- `dependencies`: runtime dependency/resource paths such as `abc_path`,
  `genlib_path`, `liberty_path`, `python_script`, and `standard_cell_csv`
- `tools`: compatibility alias for older dependency path overrides
- `run`: shared run controls
- `generate`: library generation settings
- `evaluate` or `benchmark`: physical validation settings

Runtime dependencies:

```json
{
  "dependencies": {
    "abc_path": "/absolute/path/to/abc",
    "python_script": "scripts/single_power_run.py",
    "genlib_path": "resources/nangate_45nm.genlib",
    "liberty_path": "resources/NangateOpenCellLibrary_typical.lib",
    "standard_cell_csv": "resources/standard_cells.csv"
  }
}
```

`dependencies.abc_path` is the preferred way to configure ABC. `ABC_PATH` is
accepted as a local environment fallback. The legacy `tools.abc_path` key still
works for existing configs.

`dependencies.standard_cell_csv` is the single source of truth for generation:
it is used both as the metadata cache for standard-cell properties and as the
source from which the exact-synthesis primitive set is derived.

Run controls:

```json
{
  "run": {
    "resume_policy": "resume",
    "case_timeout_ms": 600000,
    "threads": 0,
    "max_worker_memory_mb": 2048,
    "max_total_memory_mb": 8192
  }
}
```

Supported resume policies are `run_all`, `skip_completed`, `resume`,
`rerun_failed`, and `rerun_timeout`. `threads: 0` means auto/default.
Generation uses this field for `SynthesisFlow`; physical evaluation currently
parses it for consistency but does not use it to parallelize benchmark cases.
Memory-budget fields are optional; omitted or zero values preserve previous
worker-count behavior.

Generation timeout settings should use the grouped form:

```json
{
  "generate": {
    "timeouts_ms": {
      "sat": 10000,
      "optimization": 60000,
      "case": 0
    }
  }
}
```

Legacy flat keys `sat_timeout_ms`, `opt_timeout_ms`, and `case_timeout_ms` are
still accepted. For evaluation/benchmark configs, `evaluate.timeouts_ms.case`
or `benchmark.timeouts_ms.case` is accepted as an alias for `case_timeout_ms`.

Activity settings:

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
