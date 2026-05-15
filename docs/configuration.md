# 配置系统与路径规则

本项目推荐通过 JSON 配置驱动所有可复现实验。命令行参数仍然可用，并且优先级高于配置文件中的同名字段。

## 严格 JSON

`fes_app` 只接受严格 JSON：

- 不允许注释。
- 不允许尾随逗号。
- 字符串必须使用双引号。

因此 `config/*.json` 应保持可直接运行，不要写说明性注释。说明文字统一写在 Markdown 文档中。

## 路径解析

路径解析遵循固定规则，避免因为从不同目录启动程序而产生歧义。

| 路径类型 | 解析方式 |
| --- | --- |
| 绝对路径 | 原样使用 |
| 普通相对路径 | 相对于项目根目录 |
| 建库输出目录 `generate.output_dir` | 相对于 `results_repo/` |
| 评测库目录 `evaluate.library_dir` | 相对于 `results_repo/` |
| 评测输出目录 `evaluate.output_dir` | 相对于 `results_repo/` |
| 单网表库目录和输出目录 | 相对于 `results_repo/` |

示例：

```json
{
  "evaluate": {
    "benchmark_dir": "/home/user/benchmarks",
    "library_dir": "library_middle",
    "output_dir": "evaluation_default"
  }
}
```

会解析为：

```text
benchmark_dir = /home/user/benchmarks
library_dir   = <project_root>/results_repo/library_middle
output_dir    = <project_root>/results_repo/evaluation_default
```

兼容写法 `results_repo/library_middle` 仍然可用，但新配置建议直接写 `library_middle`。

## 配置优先级

从低到高：

1. 程序内置默认值
2. JSON 配置文件
3. 命令行参数

例如：

```bash
./build/fes_app evaluate \
  --config config/evaluate_default.json \
  --lib library_s1 \
  --out evaluation_s1
```

即使 JSON 中写了 `library_dir` 和 `output_dir`，这里也会被 `--lib` 和 `--out` 覆盖。

## 环境检查

推荐在长任务前先运行：

```bash
./build/fes_app doctor --config config/evaluate_default.json
```

该命令会检查：

- ABC 路径
- Python 评测脚本
- benchmark 目录
- 主库目录
- `final_results.csv`
- `detailed_infos/`
- ABC local library 是否存在，或是否会自动构建

仅检查配置合法性和输入存在性：

```bash
./build/fes_app validate-config --config config/build_library.json
```

`validate-config` 不执行建库或评测，适合前后端系统在提交任务前调用。

## 建库配置要点

主配置：

```bash
./build/fes_app generate --config config/build_library.json
```

关键字段：

```json
{
  "generate": {
    "k": 4,
    "num_functions": 0,
    "function_source": "exhaustive",
    "output_dir": "library_middle",
    "activity": {
      "mode": "uniform"
    }
  }
}
```

- `k` 控制目标函数输入数量。
- `num_functions: 0` 表示不截断函数集合。
- `function_source: "exhaustive"` 表示使用穷举函数集合。
- `output_dir` 相对 `results_repo/`。
- `activity.mode: "uniform"` 使用默认同活动输入模式。

建库产物：

```text
final_results.csv
generation_summary.json
run_manifest.csv
detailed_infos/
```

## 批量评测配置要点

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

当前评测流程会生成并评估多类候选：

- `Original`
- `ABC_Global`
- `ABC_LowPower`
- `ABC_Local`
- `PONO_Aggressive`
- `PONO_Conservative`
- `PONO_LowPower`
- `PONO_Strict`
- `PONO_PositiveOnly`

最终选择真实 PPA 中功耗最低的候选。如果 `Original` 最低，则保留原始网表作为结果。

评测产物：

```text
ppa_complete_validation.csv
evaluation_cases.csv
evaluation_manifest.csv
evaluation_summary.json
```

## 单网表配置要点

优化单个 BLIF：

```json
{
  "command": "optimize-blif",
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

命令行推荐：

```bash
./build/fes_app optimize-blif \
  --config config/optimize_blif.json \
  --blif /path/to/design.blif \
  --input-probs 0.1,0.2,0.3,0.4 \
  --json
```

`--json` 模式适合前后端调用，因为 stdout 只包含最终 JSON，不混入普通日志。

## 恢复与重跑

支持的恢复策略：

```text
run_all
skip_completed
resume
rerun_failed
rerun_timeout
```

命令行示例：

```bash
./build/fes_app generate --config config/build_library.json --resume
./build/fes_app evaluate --config config/evaluate_default.json --rerun failed
./build/fes_app evaluate --config config/evaluate_default.json --rerun timeout
```

建库和评测都会写入 manifest 文件，用于记录每个 case 的运行状态。

## 前后端调用建议

后端服务应生成任务专属配置文件，并尽量使用绝对 WSL 路径。单网表任务统一加 `--json`：

```bash
./build/fes_app optimize-blif \
  --blif /abs/path/input.blif \
  --lib library_middle \
  --input-probs 0.1,0.2,0.3,0.4 \
  --out jobs/job_001 \
  --json
```

批量任务不需要解析 stdout，直接读取输出目录下的 JSON/CSV 产物即可。
