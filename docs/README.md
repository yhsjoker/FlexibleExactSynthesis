# 文档目录

本目录保存 FlexibleExactSynthesis 的中文项目文档。首次使用建议先阅读根目录 [README.md](../README.md)，再根据任务进入对应专题。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [configuration.md](configuration.md) | JSON 配置系统、路径规则、建库/评测/单网表配置说明 |
| [deployment.md](deployment.md) | 编译部署、Z3/Kissat/ABC、物理评测环境、WSL 调用方式 |
| [activity_patterns.md](activity_patterns.md) | 建库阶段输入活动模式的生成规则 |
| [app_layer.md](app_layer.md) | CLI 应用层结构和模块职责 |
| [reproducibility.md](reproducibility.md) | 实验复现、统计口径、结果归档和论文对比注意事项 |

## 快速命令

```bash
./build/fes_app doctor --config config/evaluate_default.json
./build/fes_app generate --config config/build_library.json
./build/fes_app evaluate --config config/evaluate_default.json
./build/fes_app optimize-blif --config config/optimize_blif.json --json
```

## 发布前检查

推送到 GitHub 前建议确认：

- `config/*.json` 中没有不可公开的本机路径或账号信息。
- `scripts/single_power_run.py` 中的远端 SSH 配置已经按发布策略处理。
- `resources/` 下第三方工艺库文件的授权允许公开发布，或文档中已说明用户需要自行准备。
- 根目录已添加符合项目发布策略的 `LICENSE` 文件。
- `results_repo/`、`build/`、`logs/` 未被提交。
