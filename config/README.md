# Config Files

The runtime parser accepts strict JSON only. Do not add comments or trailing
commas to runnable `.json` files.

## Path Rules

- Absolute paths are used as-is.
- Relative paths in JSON are resolved from the project root.
- The exceptions are:
  `generate.output_dir`, `evaluate.library_dir`,
  `evaluate.abc_local_library_dir`, `evaluate.output_dir`,
  `optimize_blif.library_dir`, `optimize_blif.abc_local_library_dir`,
  `optimize_blif.output_dir`, `evaluate_blif.library_dir`,
  `evaluate_blif.abc_local_library_dir`, and `evaluate_blif.output_dir`.
  When any of those are relative, they are resolved under
  `<project_root>/results_repo`.
- Paths are not resolved relative to the current shell directory or the config
  file directory.

## Shipped Configs

| File | Workflow | Purpose |
| --- | --- | --- |
| `build_library.json` | `generate` | Primary K=4 exhaustive build using the default standard-cell set in `resources/standard_cells.csv`. Activity mode is `uniform`, so each library function gets the standard `(0.1,0.1,0.1,0.1)` through `(0.9,0.9,0.9,0.9)` sweep. Output goes to `results_repo/library_middle`. |
| `build_library_small.json` | `generate` | Faster build using the reduced gate set in `resources/standard_cells_small.csv`. Useful for quick bring-up or experiments. Output goes to `results_repo/library_small`. |
| `evaluate_default.json` | `evaluate` | Unified four-method physical evaluation: `Original / ABC Global / ABC Local / PONO`. Results are written to a dedicated evaluation output directory. If `evaluate.abc_local_library_dir` is omitted, the app auto-builds `<library_dir>_abc_local` before evaluation. |
| `optimize_blif.json` | `optimize-blif` | Single-BLIF optimization example. Exports `result.json` plus `optimized.blif` under the configured output directory. |
| `evaluate_blif.json` | `evaluate-blif` | Single-BLIF evaluation example with explicit input probabilities. Useful as a backend-facing template. |

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

Optimize one BLIF:

```bash
./build/fes_app optimize-blif --config config/optimize_blif.json
```

Optimize one BLIF with machine-readable stdout:

```bash
./build/fes_app optimize-blif --config config/optimize_blif.json --json
```

Evaluate one BLIF:

```bash
./build/fes_app evaluate-blif --config config/evaluate_blif.json
```

Validate a config without running it:

```bash
./build/fes_app validate-config --config config/evaluate_default.json
```

Check environment dependencies:

```bash
./build/fes_app doctor
```

The shipped single-BLIF configs use placeholder `blif_path` values. Replace
them with your actual target netlist before execution.

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
| `evaluate.library_dir` | evaluate | Main PONO library directory. Relative values are rooted under `results_repo`. |
| `evaluate.abc_local_library_dir` | evaluate | Optional ABC local-library directory. If omitted, the evaluator auto-builds `<library_dir>_abc_local`. Relative values are rooted under `results_repo`. |
| `evaluate.output_dir` | evaluate | Output directory for `ppa_complete_validation.csv`, `evaluation_cases.csv`, and `evaluation_summary.json`. Relative values are rooted under `results_repo`. |
| `evaluate.verify` | evaluate | Enable rewrite-time combinational equivalence checking. |
| `optimize_blif.blif_path` / `evaluate_blif.blif_path` | single-BLIF flows | Input netlist to analyze. Relative values are project-root-relative. |
| `optimize_blif.input_probs` / `evaluate_blif.input_probs` | single-BLIF flows | Required input probabilities, ordered exactly as `.inputs` appears in the BLIF. |
| `optimize_blif.input_acts` / `evaluate_blif.input_acts` | single-BLIF flows | Optional input activities. If omitted on the CLI, the tool derives `2*p*(1-p)` for each input. |
| `optimize_blif.json_stdout` / `evaluate_blif.json_stdout` | single-BLIF flows | When `true`, print only the final result JSON to stdout and write captured execution logs to `<output_dir>/stdout.log` and `<output_dir>/stderr.log`. Passing CLI `--json` is the recommended backend-facing way to enable it. |
| `optimize_blif.library_dir` / `evaluate_blif.library_dir` | single-BLIF flows | Main PONO library directory. Relative values are rooted under `results_repo`. |
| `optimize_blif.abc_local_library_dir` / `evaluate_blif.abc_local_library_dir` | single-BLIF flows | Optional ABC local-library directory. Relative values are rooted under `results_repo`. |
| `optimize_blif.output_dir` / `evaluate_blif.output_dir` | single-BLIF flows | Output directory for `result.json` and any exported BLIF artifacts. Relative values are rooted under `results_repo`. |
| `optimize_blif.emit_blif_content` | `optimize-blif` | When `true`, embed the optimized BLIF text directly into `result.json`. |

Legacy flat timeout keys and legacy `tools.*` dependency keys are still parsed
for compatibility, but new configs should prefer the grouped keys above.
