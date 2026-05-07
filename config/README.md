# Config Files

The runtime parser accepts strict JSON only. Do not add comments or trailing
commas to runnable `.json` files.

## Path Rules

- Absolute paths are used as-is.
- Relative paths in JSON are resolved from the project root.
- The only exception is `generate.output_dir`: when it is relative, it is
  resolved under `<project_root>/results_repo`.
- Relative `library_dir` values such as `results_repo/library_middle` are still
  treated as project-root-relative paths, so they resolve to
  `<project_root>/results_repo/library_middle`.
- Paths are not resolved relative to the current shell directory or the config
  file directory.

## Shipped Configs

| File | Workflow | Purpose |
| --- | --- | --- |
| `build_library.json` | `generate` | Primary K=4 exhaustive build using the default standard-cell set in `resources/standard_cells.csv`. Activity mode is `uniform`, so each library function gets the standard `(0.1,0.1,0.1,0.1)` through `(0.9,0.9,0.9,0.9)` sweep. Output goes to `results_repo/library_middle`. |
| `build_library_small.json` | `generate` | Faster build using the reduced gate set in `resources/standard_cells_small.csv`. Useful for quick bring-up or experiments. Output goes to `results_repo/library_small`. |
| `evaluate_default.json` | `evaluate` | Unified four-method physical evaluation: `Original / ABC Global / ABC Local / PONO`. By default, if `evaluate.abc_local_library_dir` is omitted, the app auto-builds `<library_dir>_abc_local` before evaluation. |

## Typical Usage

Build the main library:

```bash
./build/fes_app generate --config config/build_library.json
```

Build the smaller/faster library:

```bash
./build/fes_app generate --config config/build_library_small.json
```

Run the merged evaluation flow:

```bash
./build/fes_app evaluate --config config/evaluate_default.json
```

`evaluate_default.json` ships with `benchmark_dir: "benchmarks"` as a portable
project-root-relative placeholder. Replace it with your actual benchmark
directory before running a full evaluation job.

## Common Parameters

| Parameter | Applies To | Meaning |
| --- | --- | --- |
| `dependencies.abc_path` | all ABC-backed flows | ABC executable path. Relative values are project-root-relative. |
| `dependencies.python_script` | generation/evaluation | Physical-power helper script. Relative values are project-root-relative. |
| `dependencies.genlib_path` | generation | ABC genlib path. Relative values are project-root-relative. |
| `dependencies.liberty_path` | generation | Liberty file path. Relative values are project-root-relative. |
| `dependencies.standard_cell_csv` | generation | Standard-cell CSV used for gate metadata and exact-synthesis gate derivation. |
| `run.resume_policy` | generate/evaluate | `run_all`, `skip_completed`, `resume`, `rerun_failed`, or `rerun_timeout`. |
| `run.case_timeout_ms` | generate/evaluate | Shared per-case timeout. `0` disables timeout marking. |
| `run.threads` | generate/evaluate | Worker hint. Generation uses it directly; evaluation currently records it for diagnostics and memory-budget normalization. |
| `run.max_worker_memory_mb` | generate/evaluate | Best-effort per-worker memory budget. |
| `run.max_total_memory_mb` | generate/evaluate | Best-effort total memory budget. |
| `generate.output_dir` | generate | Relative values are rooted under `results_repo`. |
| `generate.activity.mode` | generate | `uniform`, `cartesian`, or `explicit`. |
| `generate.activity.levels` | generate | Activity levels for `uniform` or `cartesian`. Omit for the default uniform sweep. |
| `generate.activity.explicit` | generate | Explicit activity vectors. |
| `evaluate.library_dir` | evaluate | Main PONO library directory. Relative values are project-root-relative. |
| `evaluate.abc_local_library_dir` | evaluate | Optional ABC local-library directory. If omitted, the evaluator auto-builds `<library_dir>_abc_local`. |
| `evaluate.verify` | evaluate | Enable rewrite-time combinational equivalence checking. |

Legacy flat timeout keys and legacy `tools.*` dependency keys are still parsed
for compatibility, but new configs should prefer the grouped keys above.
