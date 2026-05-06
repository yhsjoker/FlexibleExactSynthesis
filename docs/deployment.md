# Deployment

This guide is for getting `fes_app` running quickly.

Build-time dependencies:

- Z3, configured with `Z3_ROOT=/path/to/z3`
- Kissat, configured with `KISSAT_ROOT=/path/to/kissat`

Runtime dependency:

- ABC, configured in JSON with `dependencies.abc_path=/path/to/abc`

Important current path behavior:

- relative input/resource/dependency paths in JSON are resolved relative to the
  project root
- relative `output_dir` values are written under `<project_root>/results_repo`

## 1. Build Z3

Go to the Z3 root:

```bash
cd /path/to/z3
python scripts/mk_make.py
cd build
make -j"$(nproc)"
```

Use the Z3 source/build root in this project:

```bash
Z3_ROOT=/path/to/z3
```

Do not point this project at a `build/z3` executable path.

## 2. Build Kissat

Go to the Kissat root:

```bash
cd /path/to/kissat
./configure
make -j"$(nproc)"
```

Use the Kissat root in this project:

```bash
KISSAT_ROOT=/path/to/kissat
```

Do not point this project at a `build/kissat` executable path.

## 3. Build ABC

Go to the ABC root:

```bash
cd /path/to/abc
make -j"$(nproc)"
```

At runtime, configure the executable path in JSON:

```json
{
  "dependencies": {
    "abc_path": "/path/to/abc/abc"
  }
}
```

## 4. Fix Execute Permission If Needed

If an uploaded or extracted tool/script is not executable, a quick fix is:

```bash
chmod -R +x <dir>
chmod +x <file>
```

## 5. Build This Project

From the project root:

```bash
cmake -S . -B build \
  -DZ3_ROOT=/root/autodl-tmp/softwares/z3 \
  -DKISSAT_ROOT=/root/autodl-tmp/softwares/kissat

cmake --build build --target fes_app -- -j"$(nproc)"
```

## 6. Configure Runtime Paths

Use a config like this:

```json
{
  "dependencies": {
    "abc_path": "/path/to/abc/abc",
    "python_script": "scripts/single_power_run.py",
    "genlib_path": "resources/nangate_45nm.genlib",
    "liberty_path": "resources/NangateOpenCellLibrary_typical.lib",
    "standard_cell_csv": "resources/standard_cells.csv"
  },
  "generate": {
    "output_dir": "library_k4_npn_cartesian"
  }
}
```

Notes:

- absolute paths are used as-is
- relative dependency/resource paths are project-root-relative
- `standard_cell_csv` is the single source of truth for both standard-cell
  metadata and the exact-synthesis gate library
- `output_dir: "library_k4_npn_cartesian"` writes to
  `<project_root>/results_repo/library_k4_npn_cartesian`

See `config/build_library.json` for the main runnable generation config.

## 7. Run The Project

From the project root:

```bash
./build/fes_app generate --config config/build_library.json
```

Users also commonly run from `build/`:

```bash
cd build
./fes_app generate --config ../config/build_library.json
```
