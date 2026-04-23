# Activity Pattern Generation

Library generation uses `ActivityPatternSpec` and `generateActivityPatterns()`
from `include/fes/flow/ActivityPatternGenerator.h`.

Default CLI behavior is the legacy uniform sweep:

```bash
./build/fes_app generate --mode exhaustive --k 4 --num 1 \
  --out activity_uniform_k4_num1
```

Switch to a cartesian grid with `--activity-mode cartesian` and
comma-separated `--activity-levels`:

```bash
./build/fes_app generate --mode exhaustive --k 4 --num 1 \
  --activity-mode cartesian --activity-levels 0.2,0.4,0.6,0.8 \
  --out activity_cartesian_k4_num1
```

Use an explicit list with semicolon-separated patterns. Each pattern is a
comma-separated vector whose width must match the generated function input
count:

```bash
./build/fes_app generate --mode exhaustive --k 4 --num 1 \
  --activity-mode explicit \
  --activity-explicit "0.1,0.2,0.3,0.4;0.4,0.3,0.2,0.1" \
  --out activity_explicit_k4_num1
```

Relative `--out` paths are rooted under the project `results_repo`. Both
`--out activity_cartesian_k4_num1` and
`--out results_repo/activity_cartesian_k4_num1` write below that repository
output root instead of below the process working directory.

For C++ callers, `LibraryGenerationConfig::activityPatternSpec` still accepts
`ActivityPatternSpec::UniformSweep(...)`, `CartesianGrid(...)`, or
`ExplicitList(...)`. `generateActivityPatterns()` remains the customization
entry point for adding a new generation strategy without changing the synthesis
pipeline.

The same modes are available through strict JSON config files:

- `config/generate_k4_uniform.json`
- `config/generate_k4_cartesian.json`
- `config/generate_k4_explicit.json`

See `docs/configuration.md` and the commented examples in `config/examples/`.

Floating-point activity values are normalized to `[0, 1]`, quantized at `1e-9`,
and serialized with fixed 9-decimal formatting for deterministic duplicate
detection. Legacy integer-percent tags such as `_10_10_10_10` are preserved for
default sweep values; non-integer-percent values use stable decimal tag
components such as `_p0p125`.
