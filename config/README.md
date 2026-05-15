# 配置文件说明

`config/` 目录保存可直接传给 `fes_app --config` 的严格 JSON 文件。运行时解析器不支持注释和尾随逗号，因此本目录中的 `.json` 文件必须保持合法 JSON。

## 路径规则

- 绝对路径原样使用。
- 普通相对路径按项目根目录解析。
- 以下字段的相对路径会解析到 `<project_root>/results_repo` 下：
  `generate.output_dir`、`evaluate.library_dir`、`evaluate.abc_local_library_dir`、`evaluate.output_dir`、`optimize_blif.library_dir`、`optimize_blif.abc_local_library_dir`、`optimize_blif.output_dir`、`evaluate_blif.library_dir`、`evaluate_blif.abc_local_library_dir`、`evaluate_blif.output_dir`。
- 路径不会按当前 shell 目录、`build/` 目录或配置文件所在目录解析。

示例：

```json
{
  "evaluate": {
    "library_dir": "library_middle",
    "output_dir": "evaluation_default"
  }
}
```

实际会解析为：

```text
<project_root>/results_repo/library_middle
<project_root>/results_repo/evaluation_default
```

## 当前配置文件

| 文件 | 命令 | 用途 |
| --- | --- | --- |
| `build_library.json` | `generate` | 主建库配置。默认 K=4，穷举函数，活动模式为 `uniform`，输出到 `results_repo/library_middle`。 |
| `build_library_small.json` | `generate` | 使用 `resources/standard_cells_small.csv` 的快速建库配置，适合调试流程。 |
| `evaluate_default.json` | `evaluate` | 批量评测配置。比较 `Original / ABC Global / ABC Local / PONO portfolio`，并选择真实功耗最低的候选。 |
| `optimize_blif.json` | `optimize-blif` | 单个 BLIF 优化模板。需要替换 `blif_path` 和输入概率。 |
| `evaluate_blif.json` | `evaluate-blif` | 单个 BLIF 评测模板。需要替换 `blif_path` 和输入概率。 |

## 常用命令

建库：

```bash
./build/fes_app generate --config config/build_library.json
```

快速建库：

```bash
./build/fes_app generate --config config/build_library_small.json
```

批量评测：

```bash
./build/fes_app evaluate --config config/evaluate_default.json
```

单网表优化：

```bash
./build/fes_app optimize-blif --config config/optimize_blif.json --json
```

单网表评测：

```bash
./build/fes_app evaluate-blif --config config/evaluate_blif.json --json
```

环境检查：

```bash
./build/fes_app doctor --config config/evaluate_default.json
```

配置预检查：

```bash
./build/fes_app validate-config --config config/build_library.json
```

## 顶层字段

| 字段 | 说明 |
| --- | --- |
| `command` | 要执行的命令：`generate`、`evaluate`、`optimize-blif`、`evaluate-blif`。 |
| `dependencies` | 工具和资源路径，例如 ABC、Python 脚本、genlib、Liberty、标准单元 CSV。 |
| `run` | 运行控制，例如恢复策略、线程数、超时和内存预算。 |
| `generate` | 建库参数。 |
| `evaluate` | 批量评测参数。 |
| `optimize_blif` | 单个 BLIF 优化参数。 |
| `evaluate_blif` | 单个 BLIF 评测参数。 |

## `dependencies`

```json
{
  "dependencies": {
    "abc_path": "/path/to/abc/abc",
    "python_script": "scripts/single_power_run.py",
    "genlib_path": "resources/nangate_45nm.genlib",
    "liberty_path": "resources/NangateOpenCellLibrary_typical.lib",
    "standard_cell_csv": "resources/standard_cells.csv"
  }
}
```

- `abc_path`：ABC 可执行文件路径。也可以用环境变量 `ABC_PATH` 作为后备。
- `python_script`：物理功耗评测入口脚本。
- `genlib_path`：建库时供 ABC 使用的 genlib。
- `liberty_path`：建库相关 Liberty 文件。
- `standard_cell_csv`：精确综合使用的标准单元描述，是建库门集的直接来源。

## `run`

```json
{
  "run": {
    "resume_policy": "resume",
    "case_timeout_ms": 0,
    "threads": 4,
    "max_worker_memory_mb": 10240,
    "max_total_memory_mb": 51200
  }
}
```

- `resume_policy`：支持 `run_all`、`skip_completed`、`resume`、`rerun_failed`、`rerun_timeout`。
- `case_timeout_ms`：单 case 超时标记，`0` 表示不启用。
- `threads`：建库线程数。评测阶段目前只作为诊断信息记录。
- `max_worker_memory_mb` 和 `max_total_memory_mb`：建库阶段的保守内存预算，用于限制实际并发数。

## `generate`

```json
{
  "generate": {
    "k": 4,
    "num_functions": 0,
    "function_source": "exhaustive",
    "output_dir": "library_middle",
    "verify": false,
    "timeouts_ms": {
      "sat": 20000,
      "optimization": 100000,
      "case": 0
    },
    "activity": {
      "mode": "uniform"
    }
  }
}
```

- `k`：目标函数输入数。
- `num_functions`：生成函数数量。`0` 表示不截断，使用完整集合。
- `function_source`：`exhaustive` 或 `benchmark`。
- `output_dir`：输出库目录，相对路径位于 `results_repo/` 下。
- `verify`：是否对生成子电路做等价性检查。
- `timeouts_ms.sat`：SAT 求解超时。
- `timeouts_ms.optimization`：单个精确综合优化超时。
- `timeouts_ms.case`：单 case 总超时标记。
- `activity.mode`：`uniform`、`cartesian` 或 `explicit`。

`uniform` 默认生成 `(0.1,0.1,0.1,0.1)` 到 `(0.9,0.9,0.9,0.9)` 的均匀活动模式。

## `evaluate`

```json
{
  "evaluate": {
    "benchmark_dir": "/path/to/benchmarks",
    "library_dir": "library_middle",
    "output_dir": "evaluation_default",
    "verify": true
  }
}
```

- `benchmark_dir`：benchmark 根目录，可以是绝对路径或项目根目录相对路径。
- `library_dir`：主 PONO 库目录，相对路径位于 `results_repo/` 下。
- `abc_local_library_dir`：可选 ABC 局部库目录。省略时会自动使用 `<library_dir>_abc_local`，若不存在则自动构建。
- `output_dir`：评测输出目录，相对路径位于 `results_repo/` 下。
- `verify`：是否对重写后的 BLIF 做 CEC。

批量评测产物：

```text
ppa_complete_validation.csv
evaluation_cases.csv
evaluation_manifest.csv
evaluation_summary.json
```

## `optimize_blif` 和 `evaluate_blif`

```json
{
  "optimize_blif": {
    "blif_path": "benchmarks/example.blif",
    "library_dir": "library_middle",
    "output_dir": "single_blif_opt_example",
    "input_probs": [0.5, 0.5, 0.5, 0.5],
    "verify": true,
    "emit_blif_content": false
  }
}
```

- `blif_path`：输入 BLIF 网表。示例配置中的路径是占位符，正式运行前必须替换。
- `library_dir`：主库目录，相对路径位于 `results_repo/` 下。
- `output_dir`：输出目录，相对路径位于 `results_repo/` 下。
- `input_probs`：输入静态概率，顺序必须和 `.inputs` 一致。
- `input_acts`：可选输入翻转率。省略时按 `2*p*(1-p)` 自动计算。
- `json_stdout`：为 `true` 时 stdout 只输出最终 JSON。
- `emit_blif_content`：为 `true` 时在 `result.json` 中嵌入优化后 BLIF 文本。

单网表 JSON 的重要字段：

```text
success
errorMessage
selectedStrategy
optimizedBlifPath
gainVsOrigPct
gainVsAbcPct
gainVsAbcLocalPct
orig
abc
abcLocal
pono
```

其中 `pono` 字段表示最终选择的最低功耗候选，不一定来自 PONO 重写；当 `Original` 或 ABC 候选功耗最低时，也会作为最终候选记录。

## 兼容字段

旧版 `tools.*` 和部分扁平 timeout 字段仍然可以解析，但新配置应优先使用 `dependencies`、`run` 和 `timeouts_ms` 的分组写法。
