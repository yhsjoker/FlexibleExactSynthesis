# Reproducible Workflows

Use config files for paper and experiment runs. Config-driven commands keep
the command line short, make dependency paths explicit, and preserve output
locations for result bundles.

## Recommended Presets

- Primary full-library generation:
  `config/build_library.json`
- Uniform activity library generation:
  `config/generate_k4_uniform.json`
- Cartesian activity library generation:
  `config/generate_k4_cartesian.json`
- Explicit activity library generation:
  `config/generate_k4_explicit.json`
- Benchmark-driven cartesian library generation:
  `config/generate_benchmark_k4_cartesian.json`
- Standard physical validation:
  `config/evaluate_default.json`
- Mapped four-way physical validation:
  `config/evaluate_mapped_four_way.json`

For deployment-specific paths, copy `config/deployment_example.json` and set
`dependencies.abc_path`.

## Example Runs

```bash
./build/fes_app generate --config config/build_library.json
./build/fes_app generate --config config/generate_k4_uniform.json
./build/fes_app generate --config config/generate_k4_cartesian.json
./build/fes_app generate --config config/generate_k4_explicit.json
./build/fes_app benchmark --config config/benchmark_default.json
./build/fes_app evaluate --config config/evaluate_mapped_four_way.json
```

Before running benchmark/evaluate presets, set `benchmark_dir` to the intended
benchmark dataset. Relative benchmark paths are resolved below the project
root; absolute paths are used as-is.

Resume and rerun controls:

```bash
./build/fes_app generate --config config/generate_k4_cartesian.json --resume
./build/fes_app benchmark --config config/benchmark_default.json --rerun failed
./build/fes_app evaluate --config config/evaluate_default.json --rerun timeout
```

## Result Artifacts

Generation writes:

- `final_results.csv`
- `run_manifest.csv`
- `generation_summary.json`
- `detailed_infos/`
- `tmp_eval/`

Physical validation preserves legacy CSVs and also writes:

- `evaluation_manifest.csv`
- `evaluation_cases.csv`
- `evaluation_summary.json`

Keep the config file, generated manifests, summaries, and legacy CSV outputs
together when archiving a paper run.
