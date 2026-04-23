# Deployment

PONO/FlexibleExactSynthesis is built as a C++17 CLI executable,
`fes_app`. The build does not require editing `CMakeLists.txt`; dependency
roots are supplied through CMake cache variables.

## Build Dependencies

- Z3 is required.
- Kissat is optional at configure time. If `KISSAT_ROOT` is provided, it must
  point to a valid Kissat tree/build. If it is omitted and Kissat is not found
  in system paths, the project builds without Kissat-backed SAT support.

Example:

```bash
cmake -S . -B build \
  -DZ3_ROOT=/absolute/path/to/z3 \
  -DKISSAT_ROOT=/absolute/path/to/kissat
cmake --build build --target fes_app -- -j
```

`Z3_ROOT` and `KISSAT_ROOT` may also be supplied as environment variables, but
the CMake cache variables are the recommended reproducible path.

## Runtime Dependencies

ABC is a runtime dependency. Configure it in JSON with
`dependencies.abc_path`:

```json
{
  "dependencies": {
    "abc_path": "/absolute/path/to/abc"
  }
}
```

Absolute paths are used as-is. Relative dependency/resource paths in JSON are
resolved below the compiled project root, not below the current working
directory, build directory, or config file directory.

For quick local use, `ABC_PATH` is also accepted as an environment fallback.
Commands that need ABC validate the selected executable path before launching
generation or physical validation and fail with a clear error if it is missing.

The older `tools.abc_path` key is still accepted as a compatibility alias.
Prefer `dependencies.abc_path` for new configs.

## Example Config

See `config/deployment_example.json` for a small strict-JSON generate config
with explicit dependency paths. Copy it, replace `/absolute/path/to/abc`, and
adjust resource paths if your standard-cell assets are not under `resources/`.
See `config/examples/deployment_example.jsonc` for a commented version.

Example project-root-relative paths:

```json
{
  "dependencies": {
    "abc_path": "third_party/abc/abc",
    "python_script": "scripts/single_power_run.py",
    "genlib_path": "resources/nangate_45nm.genlib"
  }
}
```

For generation outputs, use a short `output_dir` such as
`deployment_smoke_uniform_k4`. Relative output directories are written under
`<project_root>/results_repo`.

## Typical Commands

```bash
./build/fes_app generate --config config/generate_k4_uniform.json
./build/fes_app benchmark --config config/benchmark_default.json
./build/fes_app evaluate --config config/evaluate_default.json
./build/fes_app optimize --config config/evaluate_default.json
```

For reproducible paper runs, prefer config files with stable `output_dir`
values and keep the generated manifests and summaries with the result bundle.
