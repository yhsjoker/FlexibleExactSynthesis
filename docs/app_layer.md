# App Layer Structure

`src/main.cpp` is intentionally thin. It delegates process execution to
`fes::app::runApplication()` in `src/app/Application.cpp`.

The app layer owns command-facing orchestration:

- process command dispatch for `generate`, `optimize`, `evaluate`, and
  `benchmark`
- help/usage routing
- repository/resource context discovery from the CMake-provided project root
- config and CLI option merging through `AppConfig` and `GenerateCli`
- strict JSON config loading; commented config examples stay in
  `config/examples/*.jsonc`
- runtime dependency validation, including `dependencies.abc_path`
- command-specific wiring into `SynthesisFlow` and `InnovusBatchEvaluator`
- the legacy stdin API path (`fes_app -c ...`)

Algorithmic synthesis, activity-pattern generation, physical validation, and
manifest internals remain in their existing flow/evaluator modules. Keep future
CLI/product changes in the app layer unless the runtime behavior genuinely
belongs inside those lower-level modules.
