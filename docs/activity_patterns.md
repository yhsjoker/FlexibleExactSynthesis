# 活动模式生成

建库阶段会为每个目标布尔函数生成一组输入活动模式。活动模式决定精确综合时用于估算功耗的输入概率，因此会直接影响库中候选电路的排序和最终重写质量。

相关实现位于：

- `include/fes/flow/ActivityPatternGenerator.h`
- `src/flow/ActivityPatternGenerator.cpp`

## 支持的模式

### `uniform`

`uniform` 表示所有输入引脚使用相同概率。当前默认行为是：

```text
(0.1,0.1,0.1,0.1)
(0.2,0.2,0.2,0.2)
...
(0.9,0.9,0.9,0.9)
```

配置示例：

```json
{
  "generate": {
    "activity": {
      "mode": "uniform"
    }
  }
}
```

如果显式给出 levels：

```json
{
  "generate": {
    "activity": {
      "mode": "uniform",
      "levels": [0.2, 0.5, 0.8]
    }
  }
}
```

则 K=4 时生成：

```text
(0.2,0.2,0.2,0.2)
(0.5,0.5,0.5,0.5)
(0.8,0.8,0.8,0.8)
```

### `cartesian`

`cartesian` 会对每个输入引脚做笛卡尔积组合。K=4、levels 为 `[0.3, 0.7]` 时，会生成 `2^4 = 16` 个模式。

```json
{
  "generate": {
    "activity": {
      "mode": "cartesian",
      "levels": [0.3, 0.7]
    }
  }
}
```

注意：如果 levels 是 `[0.1,0.2,...,0.9]`，K=4 时会生成 `9^4 = 6561` 个模式，建库时间和输出体积都会显著增加。

### `explicit`

`explicit` 用于手工指定完整活动向量：

```json
{
  "generate": {
    "activity": {
      "mode": "explicit",
      "explicit": [
        [0.1, 0.2, 0.3, 0.4],
        [0.4, 0.3, 0.2, 0.1]
      ]
    }
  }
}
```

每个向量的长度必须等于当前目标函数输入数。

## CLI 示例

默认均匀模式：

```bash
./build/fes_app generate --mode exhaustive --k 4 --num 1 --out activity_uniform_example
```

笛卡尔积模式：

```bash
./build/fes_app generate \
  --mode exhaustive \
  --k 4 \
  --num 1 \
  --activity-mode cartesian \
  --activity-levels 0.2,0.4,0.6,0.8 \
  --out activity_cartesian_example
```

显式模式：

```bash
./build/fes_app generate \
  --mode exhaustive \
  --k 4 \
  --num 1 \
  --activity-mode explicit \
  --activity-explicit "0.1,0.2,0.3,0.4;0.4,0.3,0.2,0.1" \
  --out activity_explicit_example
```

## 输出命名

活动值会被归一化到 `[0,1]`，并以稳定格式写入库文件名和 CSV 记录中。默认整数百分比模式会保留历史标签，例如：

```text
_10_10_10_10
```

非整数百分比会使用稳定的小数标签，例如：

```text
_p0p125
```

这样可以避免浮点格式差异导致重复检测不稳定。

## 建议

- 快速实验优先使用 `uniform`。
- 需要覆盖不均匀输入概率时使用 `explicit`。
- 只有在确实需要完整输入概率组合时才使用 `cartesian`。
- 对于当前只包含同活动输入模式的库，评测时的重写打分会更依赖实际 PPA 选择兜底，因此建议保留最终 portfolio 选择逻辑。
