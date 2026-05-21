# FlexibleExactSynthesis

FlexibleExactSynthesis 是一个面向低功耗逻辑优化实验的命令行工具。项目核心流程包括精确综合建库、基于 4-cut/LUT 的逻辑重写、ABC 基线优化、物理功耗评测，以及面向前后端系统调用的单网表优化接口。

当前项目的主要目标是：在给定 BLIF 网表和输入信号概率的条件下，利用预生成的子电路库和多种重写候选，选择物理评测功耗最低的合法网表。

## 功能概览

- `generate`：生成精确综合子电路库，输出 `final_results.csv` 和 `detailed_infos/`。
- `evaluate`：批量评测 benchmark，统一比较 `Original / ABC Global / ABC Local / PONO portfolio`，并选择真实测得功耗最低的候选。
- `optimize-blif`：对单个 BLIF 网表执行优化，导出 `optimized.blif` 和结构化 `result.json`。
- `evaluate-blif`：对单个 BLIF 网表做四路评测，不要求输出优化网表。
- `doctor`：检查环境、配置文件和关键输入路径。
- `validate-config`：只解析配置和检查必要输入，不执行长任务。

## 项目结构

```text
.
├── config/                 # 可直接运行的 JSON 配置
├── docs/                   # 中文说明文档
├── include/fes/            # C++ 头文件
├── resources/              # 标准单元、Liberty、LEF、genlib 等资源
├── scripts/                # 物理评测脚本与 Liberty 解析工具
├── src/                    # 核心实现
├── tests/                  # 测试辅助代码
└── results_repo/           # 默认实验输出目录
```

`results_repo/`、`build/`、`logs/` 通常不提交到 Git 仓库。若你准备公开发布，请确认 `resources/` 下第三方工艺库文件的授权状态，再决定是否随仓库发布。

## 依赖

构建期依赖：

- CMake 3.15 或更高版本
- C++17 编译器
- Z3，使用 `Z3_ROOT` 指定根目录
- Kissat，可选，使用 `KISSAT_ROOT` 指定根目录

运行期依赖：

- ABC，可在 JSON 中通过 `dependencies.abc_path` 指定
- Python 3
- 物理评测脚本需要远端或本地可用的 Yosys、Innovus 和对应工艺库

当前默认物理评测脚本 [scripts/run_job.sh](scripts/run_job.sh) 使用 `NanGate_15nm_OCL` Liberty/LEF；[scripts/run_job_45nm.sh](scripts/run_job_45nm.sh) 是 45nm 版本模板。建库默认配置仍使用 45nm Nangate 资源。比较论文或实验数据时必须保证建库库文件、物理评测库文件和论文设置一致。

## 构建

```bash
cmake -S . -B build \
  -DZ3_ROOT=/path/to/z3 \
  -DKISSAT_ROOT=/path/to/kissat

cmake --build build -j"$(nproc)"
```

如果系统路径中已经能找到 Z3，可省略 `Z3_ROOT`。如果没有 Kissat，项目仍可构建，但 Kissat 后端不可用。

## 快速检查

检查默认环境：

```bash
./build/fes_app doctor
```

检查某个配置文件：

```bash
./build/fes_app doctor --config config/evaluate_default.json
./build/fes_app validate-config --config config/build_library.json
```

`doctor --config` 会读取 JSON 中的依赖路径，并检查 benchmark、library、BLIF 等关键输入是否存在。

## 建库

主库建库：

```bash
./build/fes_app generate --config config/build_library.json
```

小门集快速建库：

```bash
./build/fes_app generate --config config/build_library_small.json
```

默认建库输出：

```text
results_repo/<library_name>/
├── final_results.csv
├── generation_summary.json
├── run_manifest.csv
└── detailed_infos/
```

当前主配置使用 `activity.mode = "uniform"`，会生成 `(0.1,0.1,0.1,0.1)` 到 `(0.9,0.9,0.9,0.9)` 的均匀输入翻转模式。更多活动模式见 [docs/activity_patterns.md](docs/activity_patterns.md)。

## 批量评测

```bash
./build/fes_app evaluate --config config/evaluate_default.json
```

也可以显式覆盖库、benchmark 和输出目录：

```bash
./build/fes_app evaluate \
  --config config/evaluate_default.json \
  --lib library_middle \
  --path /path/to/benchmarks \
  --out evaluation_default
```

相对 `--lib` 和 `--out` 会自动解析到 `results_repo/` 下。评测输出包括：

```text
results_repo/<evaluation_name>/
├── ppa_complete_validation.csv
├── evaluation_cases.csv
├── evaluation_manifest.csv
└── evaluation_summary.json
```

评测阶段会真实测量多个候选网表的 PPA，并选择功耗最低的候选作为最终结果。若原始网表功耗最低，系统会选择 `Original`，避免输出比原始网表功耗更高的结果。

## 单个 BLIF 优化

```bash
./build/fes_app optimize-blif \
  --blif /path/to/design.blif \
  --lib library_middle \
  --input-probs random \
  --out single_job_001 \
  --json
```

`--json` 模式下，stdout 只输出最终 JSON，适合后端服务调用。结果同时写入：

```text
results_repo/single_job_001/
├── result.json
├── optimized.blif
├── stdout.log
└── stderr.log
```

`input-probs` 可以写成逗号分隔数值，例如 `0.1,0.2,0.3,0.4`，其数量必须与 BLIF `.inputs` 数量一致；也可以写成 `random`，工具会按 `.inputs` 数量生成一组随机输入概率。若不提供 `input-acts`，工具会按 `2*p*(1-p)` 自动推导输入翻转率。

## 单个 BLIF 评测

```bash
./build/fes_app evaluate-blif \
  --blif /path/to/design.blif \
  --lib library_middle \
  --input-probs random \
  --out single_eval_001 \
  --json
```

该命令返回 `Original / ABC / ABC Local / PONO portfolio` 的评测结果，但不强制导出优化后的 BLIF。

## 前后端集成

推荐把本项目作为后端执行内核，由外部 Java Web、Node.js 或其他服务通过子进程调用 `fes_app`。Windows 前后端调用 WSL 内的工具时，推荐命令形态如下：

```powershell
wsl.exe -d Ubuntu --cd /home/yhs_joker/project/FES2.0/FlexibleExactSynthesis -- ./build/fes_app doctor --config config/evaluate_default.json
```

单网表任务应优先使用 `--json`，批量任务应以输出目录中的 JSON/CSV 产物为准。更完整的集成说明见 [docs/deployment.md](docs/deployment.md) 和 [docs/configuration.md](docs/configuration.md)。

## 测试

```bash
cmake --build build -j"$(nproc)"
./build/test_generate_cli
./build/test_activity_patterns
./build/test_npn
./build/test_run_manifest
./build/test_synthesis_library_loader
```

## 许可证

当前仓库尚未包含 `LICENSE` 文件。公开发布前应根据你的论文、课程、实验室或项目要求选择许可证，并确认第三方资源文件是否允许随仓库分发。

## 文档索引

- [配置文件说明](config/README.md)
- [配置系统与路径规则](docs/configuration.md)
- [活动模式生成](docs/activity_patterns.md)
- [CLI 应用层结构](docs/app_layer.md)
- [部署与物理评测环境](docs/deployment.md)
- [实验复现建议](docs/reproducibility.md)

## 开源发布注意事项

- 检查 `resources/` 下工艺库、Liberty、LEF、genlib 文件的授权。
- 检查 `config/*.json` 中是否含有本机绝对路径，例如 `abc_path` 和 `benchmark_dir`。
- 添加或确认 `LICENSE` 文件。
- 不要提交 `results_repo/`、`build/`、`logs/` 等运行产物。
- 若公开仓库不包含第三方工艺库，请在 README 或 release 说明中明确列出用户需要自行准备的文件名和放置路径。
