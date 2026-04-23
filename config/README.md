# Config Files

The runtime parser accepts strict JSON only. Do not add `//` comments,
`/* */` comments, or trailing commas to runnable `.json` files. Commented
examples live in `config/examples/*.jsonc` and are for humans, not for
`fes_app --config`.

## Path Rules

- Absolute paths are used as-is.
- Relative input, resource, and dependency paths are resolved under the
  compiled project root.
- Relative `generate.output_dir` values and CLI `--out` values are resolved
  under `<project_root>/results_repo`.
- Paths are not resolved relative to the current working directory, build
  directory, or config file directory.

## Runnable Config Set

| File | Workflow | Purpose |
| --- | --- | --- |
| `build_library.json` | `generate` | Primary full-library build: all K=4 exhaustive NPN-canonical representatives, cartesian activity, verification enabled. |
| `generate_k4_uniform.json` | `generate` | Smoke/example config using one exhaustive K=4 function and the legacy uniform activity sweep. |
| `generate_k4_cartesian.json` | `generate` | Smoke/example config using one exhaustive K=4 function and cartesian activity levels. |
| `generate_k4_explicit.json` | `generate` | Smoke/example config using one exhaustive K=4 function and two explicit activity vectors. |
| `generate_benchmark_k4_cartesian.json` | `generate` | Research/example config for benchmark-driven function selection with cartesian activity. |
| `benchmark_default.json` | `benchmark` | Standard physical validation using the benchmark command. |
| `evaluate_default.json` | `evaluate` | Standard physical validation using the evaluate command. |
| `evaluate_mapped_four_way.json` | `evaluate` | Mapped-origin four-way physical validation. |
| `deployment_example.json` | `generate` | Deployment template showing runtime dependency/resource paths. |

The benchmark configs use `benchmark_dir: "benchmarks"` as a portable
project-root-relative placeholder. Replace it with an absolute benchmark
directory or with a project-root-relative dataset path before running a full
validation job.

## Primary Library Build

Use `config/build_library.json` for the main full-library generation path:

```bash
./build/fes_app generate --config config/build_library.json
```

Current program semantics for that config:

- `function_source: "exhaustive"` calls the exhaustive NPN path.
- The exhaustive path enumerates all truth tables for `generate.k`, computes
  NPN canonical representatives, aggregates representative frequency, and
  sorts representatives deterministically.
- `num_functions: 0` means no positive truncation. This is the current way to
  request the full selected exhaustive set.
- For `k: 4`, the raw truth-table universe has `2^(2^4) = 65536` functions
  before NPN canonicalization. The generated function set is the NPN-canonical
  representative set, not 65536 separate raw functions.
- With cartesian activity levels `[0.2, 0.4, 0.6, 0.8]` and `k: 4`, each
  selected representative gets `4^4 = 256` activity patterns.
- The output directory resolves to
  `<project_root>/results_repo/library_k4_npn_cartesian`.

## Common Parameters

| Parameter | Applies To | Meaning |
| --- | --- | --- |
| `command` | all | Expected CLI command: `generate`, `benchmark`, or `evaluate`. |
| `dependencies.abc_path` | all ABC-backed flows | Runtime ABC executable. Relative paths are project-root-relative. |
| `dependencies.python_script` | generation/evaluation | Physical-power helper script. Relative paths are project-root-relative. |
| `dependencies.genlib_path` | generation | ABC genlib path. Relative paths are project-root-relative. |
| `dependencies.liberty_path` | generation | Standard-cell Liberty path. Relative paths are project-root-relative. |
| `dependencies.standard_cell_csv` | generation | Cached parsed standard-cell CSV path. |
| `run.resume_policy` | generate/evaluate/benchmark | `run_all`, `skip_completed`, `resume`, `rerun_failed`, or `rerun_timeout`. |
| `run.case_timeout_ms` | generate/evaluate/benchmark | Shared case timeout; `0` disables timeout marking. Command-specific timeout keys can override it. |
| `run.threads` | generate/evaluate/benchmark | Parsed for all config-driven flows. `0` means auto/default. Generation uses this for `SynthesisFlow`; physical evaluation currently parses it for consistency but does not parallelize evaluator cases through this field. |
| `generate.k` | generate | LUT input count. Must fit the compiled LUT range. |
| `generate.num_functions` | generate | Maximum generated/selected functions; `0` means no positive count limit in the generation path. |
| `generate.function_source` | generate | `exhaustive` or `benchmark`. |
| `generate.benchmark_dir` | benchmark-driven generate | Benchmark source directory for function selection. Relative paths are project-root-relative. |
| `generate.output_dir` | generate | Output subdirectory under `<project_root>/results_repo` when relative. |
| `generate.verify` | generate | Enables Z3-backed equivalence checks for generated circuits. |
| `generate.timeouts_ms.sat` | generate | SAT solver timeout in milliseconds. |
| `generate.timeouts_ms.optimization` | generate | Optimization timeout in milliseconds. |
| `generate.timeouts_ms.case` | generate | Per-case wall-time threshold; `0` disables timeout marking. |
| `generate.activity.mode` | generate | `uniform`, `cartesian`, or `explicit`. |
| `generate.activity.levels` | generate | Numeric levels for uniform/cartesian modes. |
| `generate.activity.explicit` | generate | Array of explicit activity vectors; vector width must match the generated function input count. |
| `evaluate.benchmark_dir` / `benchmark.benchmark_dir` | evaluate/benchmark | Benchmark directory. Relative paths are project-root-relative. |
| `evaluate.library_dir` / `benchmark.library_dir` | evaluate/benchmark | Generated library directory. Empty string selects the newest generated library under `results_repo`. Relative non-empty paths are project-root-relative. |
| `evaluate.abc_local_library_dir` | mapped four-way evaluate | ABC local-library directory for mapped-origin comparison. |
| `evaluate.verify` / `benchmark.verify` | evaluate/benchmark | Enables rewrite-time equivalence checking. |
| `evaluate.mode` / `benchmark.mode` | evaluate/benchmark | `standard` or `mapped_four_way`. |
| `evaluate.timeouts_ms.case` / `benchmark.timeouts_ms.case` | evaluate/benchmark | Per-benchmark timeout threshold; `0` disables timeout marking. |

Legacy `tools.*` dependency keys and flat generation timeout keys
`sat_timeout_ms`, `opt_timeout_ms`, and `case_timeout_ms` are still accepted
for compatibility. Prefer the grouped keys shown here for new configs.
