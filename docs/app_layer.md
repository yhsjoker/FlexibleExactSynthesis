# CLI 应用层结构

本项目的命令行入口刻意保持较薄。`src/main.cpp` 只负责把进程控制权交给 `fes::app::runApplication()`，主要命令编排集中在 `src/app/Application.cpp`。

## 分层原则

项目按职责分为四层：

| 层级 | 主要目录 | 职责 |
| --- | --- | --- |
| CLI 应用层 | `src/app/` | 命令分发、配置解析、路径解析、依赖检查、结果输出 |
| 流程层 | `src/flow/` | 建库流程、活动模式生成、运行 manifest |
| 求解与编码层 | `src/encoders/`、`src/solvers/` | 精确综合约束编码、Z3/Kissat 求解 |
| 工具层 | `src/utils/` | BLIF 读写、库加载、等价性检查、物理评测、逻辑重写 |

应用层不直接实现算法细节，而是负责把用户命令转换成底层流程调用。

## 命令入口

支持的主要命令：

```text
generate
evaluate
benchmark
optimize
optimize-blif
evaluate-blif
doctor
validate-config
```

其中：

- `evaluate`、`benchmark`、`optimize` 目前都进入批量物理评测流程。
- `optimize-blif` 和 `evaluate-blif` 面向单个 BLIF，适合前后端系统调用。
- `doctor` 和 `validate-config` 用于运行前检查。

## 配置解析

配置相关实现：

- `include/fes/app/AppConfig.h`
- `src/app/AppConfig.cpp`
- `src/app/GenerateCli.cpp`

解析规则：

1. 读取严格 JSON。
2. 合并命令行覆盖参数。
3. 按项目根目录或 `results_repo/` 解析路径。
4. 检查必需依赖和输入。
5. 构造底层流程对象。

命令行参数优先级高于 JSON 配置。

## 建库调用链

```text
fes_app generate
  -> runGenerateCommand()
  -> parseGenerateOptions()
  -> SynthesisFlow::generateLibrary()
  -> final_results.csv / detailed_infos/
```

建库使用：

- `ActivityPatternGenerator` 生成活动模式。
- `SynthesisLibraryLoader` 从标准单元 CSV 派生门集。
- `PatternEncoder` 编码精确综合约束。
- Z3/Kissat 执行求解。
- `BlifWriter` 输出候选子电路。

## 批量评测调用链

```text
fes_app evaluate
  -> runOptimizeCommand()
  -> prepareEvaluationLibraries()
  -> InnovusBatchEvaluator::runBatchVerification()
  -> ppa_complete_validation.csv / evaluation_summary.json
```

评测流程会执行：

1. 原始网表 PPA 评测。
2. ABC 全局优化候选。
3. ABC 低功耗候选。
4. ABC local library rewrite 候选。
5. 多个 PONO rewrite profile。
6. 对所有合法候选做真实 PPA 评测。
7. 选择功耗最低的候选作为最终结果。

若启用 `verify`，每个重写结果会通过 ABC CEC 检查，失败时回退到输入网表。

## 单网表调用链

```text
fes_app optimize-blif --json
  -> runSingleBlifCommand()
  -> InnovusBatchEvaluator::analyzeSingleBlif()
  -> result.json / optimized.blif
```

`--json` 模式会捕获普通 stdout/stderr，并写入：

```text
<output_dir>/stdout.log
<output_dir>/stderr.log
```

stdout 只输出最终 JSON，便于前后端解析。

## 兼容入口

项目仍保留旧 stdin API：

```bash
cat design.blif | ./build/fes_app -c 0.1 0.2 0.3 0.4
```

该入口仅用于兼容旧调用方式。新系统应优先使用 `optimize-blif --json`。

## 扩展建议

- 新增用户可见命令时，优先在 `src/app/Application.cpp` 增加分发和帮助文本。
- 新增配置字段时，同时更新 `AppConfig`、`config/README.md` 和测试。
- 不要在 CLI 应用层实现算法逻辑；算法变化应进入 `flow`、`utils` 或 `encoders`。
- 对前后端集成场景，应保持 JSON 输出结构稳定。
