# 部署与物理评测环境

本文档说明如何构建 `fes_app`，以及如何准备运行期工具链。由于物理功耗评测依赖 Yosys、Innovus 和工艺库，本项目的可复现实验需要同时关注本地 C++ 环境和远端物理验证环境。

## 构建期依赖

必须：

- CMake 3.15 或更高版本
- 支持 C++17 的编译器
- Z3

可选：

- Kissat

运行期必须：

- ABC
- Python 3
- `scripts/single_power_run.py` 所需 Python 包，例如 `paramiko`

物理评测需要：

- Yosys
- Cadence Innovus
- 与 `scripts/run_job.sh` 匹配的 Liberty/LEF 工艺库

## 编译 Z3

```bash
cd /path/to/z3
python scripts/mk_make.py
cd build
make -j"$(nproc)"
```

配置本项目时传入 Z3 根目录：

```bash
cmake -S . -B build -DZ3_ROOT=/path/to/z3
```

`Z3_ROOT` 应指向 Z3 源码或安装根目录，而不是某个单独的可执行文件。

## 编译 Kissat

```bash
cd /path/to/kissat
./configure
make -j"$(nproc)"
```

配置本项目：

```bash
cmake -S . -B build \
  -DZ3_ROOT=/path/to/z3 \
  -DKISSAT_ROOT=/path/to/kissat
```

如果不提供 Kissat，项目仍可构建，但相关 SAT 后端不可用。

## 编译 ABC

```bash
cd /path/to/abc
make -j"$(nproc)"
```

在 JSON 中配置：

```json
{
  "dependencies": {
    "abc_path": "/path/to/abc/abc"
  }
}
```

也可以临时使用环境变量：

```bash
export ABC_PATH=/path/to/abc/abc
```

但推荐在配置文件中显式写出路径，方便复现实验。

## 编译本项目

```bash
cmake -S . -B build \
  -DZ3_ROOT=/path/to/z3 \
  -DKISSAT_ROOT=/path/to/kissat

cmake --build build -j"$(nproc)"
```

验证：

```bash
./build/fes_app help
./build/test_generate_cli
./build/test_activity_patterns
./build/test_npn
./build/test_run_manifest
./build/test_synthesis_library_loader
```

## 资源文件

默认配置会引用下列文件：

```text
resources/nangate_45nm.genlib
resources/NangateOpenCellLibrary_typical.lib
resources/standard_cells.csv
resources/standard_cells_small.csv
```

物理评测脚本可能引用：

```text
resources/NanGate_15nm_OCL_typical_conditional_nldm.lib
resources/NanGate_15nm_OCL.tech.lef
resources/NanGate_15nm_OCL.macro.lef
resources/NangateOpenCellLibrary.lef
```

如果开源仓库不包含第三方工艺库文件，用户需要自行准备并放到相同路径，或修改配置和脚本中的路径。

## 物理评测脚本

C++ 评测器调用：

```text
scripts/single_power_run.py
```

该脚本会通过 SSH 把 BLIF、活动文件和 `scripts/run_job.sh` 上传到远端工作目录，然后解析 Innovus 日志中的功耗、面积和时延。

当前默认远端配置位于 `single_power_run.py`：

```python
VM_IP = "127.0.0.1"
VM_PORT = 2222
VM_USER = "joker"
VM_PASS = "joker"
VM_WORK_DIR = "/home/joker/remote_work"
```

正式开源或迁移环境时，应把这些值改成部署环境对应配置。若不希望在仓库中保存账号密码，应改为环境变量读取。

## 15nm 与 45nm 评测脚本

当前默认上传执行的是：

```text
scripts/run_job.sh
```

该脚本使用：

```text
NanGate_15nm_OCL_typical_conditional_nldm.lib
NanGate_15nm_OCL.tech.lef
NanGate_15nm_OCL.macro.lef
```

仓库中还提供：

```text
scripts/run_job_45nm.sh
```

作为 45nm Nangate 评测模板。

注意：如果建库使用 45nm 资源，而物理评测使用 15nm 资源，那么库内 cost 与最终物理功耗不完全一致。论文复现或方法对比时，应保证工艺库和工具链设置一致。

## 运行前检查

检查默认环境：

```bash
./build/fes_app doctor
```

检查评测配置：

```bash
./build/fes_app doctor --config config/evaluate_default.json
```

检查建库配置：

```bash
./build/fes_app doctor --config config/build_library.json
```

## Windows 前后端调用 WSL

如果前端和后端运行在 Windows，而 `fes_app` 在 WSL Ubuntu 中，后端应通过 `wsl.exe` 调用：

```powershell
wsl.exe -d Ubuntu --cd /home/yhs_joker/project/FES2.0/FlexibleExactSynthesis -- ./build/fes_app doctor --config config/evaluate_default.json
```

建议所有任务文件都放在 WSL 文件系统中，例如：

```text
/home/yhs_joker/project/FES2.0/FlexibleExactSynthesis/results_repo/jobs/<job_id>/
```

Windows 后端可以通过 `\\wsl$\Ubuntu\...` 读取文件，但前端下载应统一走后端 HTTP 接口。

## 常见问题

### `doctor` 提示 ABC 未配置

使用：

```bash
./build/fes_app doctor --config config/evaluate_default.json
```

或设置：

```bash
export ABC_PATH=/path/to/abc/abc
```

### 物理评测返回 `-1.0`

通常说明远端 Yosys/Innovus 流程失败，或脚本没有解析到功耗数据。检查：

- `logs/ssh_debug/`
- 远端 `/home/joker/remote_work/innovus.log`
- `scripts/run_job.sh` 中的 Liberty/LEF 路径
- Innovus license 和二进制路径

### 面积显示 `-1.000`

说明脚本没有成功解析面积报告。功耗仍可用于优化比较，但正式论文数据应修复面积解析，或在表格中明确说明面积无效。
