# 实验复现建议

本项目的实验结果由建库配置、benchmark 集合、输入活动参数、ABC/Yosys/Innovus 版本、工艺库和最终统计口径共同决定。公开结果或与论文数据比较时，应完整记录这些信息。

## 推荐复现流程

1. 固定代码版本。
2. 固定第三方工具版本。
3. 固定 `config/*.json`。
4. 运行 `doctor --config` 检查路径和依赖。
5. 建库。
6. 批量评测。
7. 保存配置、manifest、summary 和 CSV。

示例：

```bash
./build/fes_app doctor --config config/build_library.json
./build/fes_app generate --config config/build_library.json

./build/fes_app doctor --config config/evaluate_default.json
./build/fes_app evaluate --config config/evaluate_default.json
```

## 必须记录的信息

建议在实验报告中记录：

- Git commit hash
- `config/build_library.json`
- `config/evaluate_default.json`
- benchmark 来源和具体路径
- ABC 版本
- Yosys 版本
- Innovus 版本
- Z3/Kissat 版本
- 建库使用的 `genlib`、Liberty 和标准单元 CSV
- 物理评测使用的 Liberty/LEF
- 是否启用 CEC：`verify`
- 是否使用最终 portfolio 选择

如果建库和评测使用不同工艺库，例如建库使用 45nm、评测使用 15nm，应明确写出。

## 建库产物

建库输出目录通常位于：

```text
results_repo/<library_name>/
```

关键文件：

```text
final_results.csv
generation_summary.json
run_manifest.csv
detailed_infos/
```

归档实验时至少保留：

- 建库 JSON
- `final_results.csv`
- `generation_summary.json`
- `run_manifest.csv`
- 完整 `detailed_infos/`

`tmp_eval/` 是临时目录，不需要归档。

## 评测产物

评测输出目录通常位于：

```text
results_repo/<evaluation_name>/
```

关键文件：

```text
ppa_complete_validation.csv
evaluation_cases.csv
evaluation_manifest.csv
evaluation_summary.json
```

其中：

- `ppa_complete_validation.csv` 是面向论文表格的汇总结果。
- `evaluation_cases.csv` 包含每个 benchmark 的候选有效性、PPA 和选中策略。
- `evaluation_summary.json` 记录成功、失败、跳过和超时数量。
- `evaluation_manifest.csv` 支持恢复和重跑。

## 统计口径

常见功耗收益口径有两种。

逐 benchmark 平均：

```text
mean((P_base - P_opt) / P_base)
```

总功耗加权：

```text
(sum(P_base) - sum(P_opt)) / sum(P_base)
```

两者可能差异明显。小电路在逐 benchmark 平均中权重和大电路相同；总功耗加权会让大功耗 benchmark 占更高权重。

与论文比较时必须使用同一口径。

## 当前最终选择逻辑

批量评测和单网表优化会评估多个候选：

```text
Original
ABC_Global
ABC_LowPower
ABC_Local
PONO_Aggressive
PONO_Conservative
PONO_LowPower
PONO_Strict
PONO_PositiveOnly
```

最终选择真实测得功耗最低的候选。这个策略能避免输出比原始网表功耗更高的结果，但也意味着结果是 portfolio/oracle 风格选择。

如果要与只输出单一优化结果的论文方法公平比较，建议额外报告：

- `PONO-only` 最优结果
- `ABC-only` 最优结果
- `Final portfolio` 最优结果

## 延迟和面积字段

当前日志中可能出现：

```text
Area: -1.000
Delay: -1.000
```

这表示物理评测脚本没有解析到对应字段。功耗有效时仍可用于低功耗比较，但正式结果表格应明确说明无效字段，或修复脚本解析。

## 重跑失败或超时 case

只重跑失败：

```bash
./build/fes_app evaluate --config config/evaluate_default.json --rerun failed
```

只重跑超时：

```bash
./build/fes_app evaluate --config config/evaluate_default.json --rerun timeout
```

恢复已完成任务：

```bash
./build/fes_app generate --config config/build_library.json --resume
```

## 单网表复现

```bash
./build/fes_app optimize-blif \
  --blif /path/to/design.blif \
  --lib library_middle \
  --input-probs 0.1,0.2,0.3,0.4 \
  --out single_job_001 \
  --json
```

为了保证复现实验可重复，建议显式写出 `--input-probs` 数值列表。`--input-probs random` 适合快速试运行，但每次执行会生成新的随机输入概率。

归档：

```text
result.json
optimized.blif
stdout.log
stderr.log
输入 BLIF
输入概率和翻转率
```

`result.json` 中的 `selectedStrategy` 是最终选择的候选来源。
