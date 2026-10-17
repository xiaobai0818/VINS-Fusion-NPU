# SuperPoint → RKNN 转换任务（给 AI 直接执行）

你在 **x86_64 Linux 或 WSL2 Ubuntu** 上。目标：生成板端 VINS-NPU 能直接加载的模型文件。

**禁止改输入尺寸、禁止改成 RGB、禁止默认 INT8、禁止导出 descriptor 头。** 改这些会导致板端 C++ 解不出来点。

## 成功标准（必须全部满足）

完成后当前目录必须有：

```
superpoint_640x480.rknn     # 唯一交付物，给板端用
superpoint_640x480.onnx     # 中间产物，可留着
```

`superpoint_640x480.rknn` 约束：

| 项 | 必须是 |
|---|---|
| 芯片 | `rk3588` |
| 量化 | **FP16**（`do_quantization=False`） |
| 输入 | 灰度 1 通道，高 480，宽 640 |
| 预处理 | `mean_values=[[0]]`, `std_values=[[255]]`（UINT8 0–255 → float 0–1） |
| 输出 | 仅 `semi`，形状 `[1, 65, 60, 80]`（65=64 个 cell + 1 个 dustbin） |
| 文件名 | 必须叫 `superpoint_640x480.rknn`，不要改名 |

板端 C++ 会：喂 UINT8 NHWC 灰度；取 65×(H/8)×(W/8) 的输出做 softmax 解码。H=480,W=640 → 60×80。

## 环境（先检查再装）

1. `uname -m` 必须是 `x86_64`。若是 `aarch64`，停下来，这个包不能在 ARM 上转。
2. 若是 Windows：不要用原生 PowerShell。改到 **WSL2 Ubuntu 20.04/22.04** 再做。RKNN-Toolkit2 没有官方 Windows wheel。
3. Python 用 3.8 / 3.9 / 3.10 / 3.11 / 3.12 之一（推荐 3.10）。
4. 建议 venv，不要污染系统 Python。

一键：

```bash
cd /path/to/x86_convert_superpoint
python3 -m venv .venv
source .venv/bin/activate
python3 install_rknn_toolkit2.py
python3 convert_superpoint_rknn.py --all
```

`install_rknn_toolkit2.py` 会按当前 CPython 版本从 GitHub 拉对应 `rknn_toolkit2-2.3.2-*-linux_x86_64.whl`。若 GitHub 很慢，可换：

https://github.com/airockchip/rknn-toolkit2

目录：`rknn-toolkit2/packages/x86_64/`

还需要：`torch`（CPU 即可）、`onnx`。安装脚本会装。

## 转换脚本行为

`convert_superpoint_rknn.py --all` 做两步：

1. 下载 MagicLeap `superpoint_v1.pth`，导出 detect-only ONNX（只有 detector，没有 256-d descriptor）。
2. RKNN-Toolkit2 `load_onnx` → `build(do_quantization=False)` → `export_rknn`。

不要改网络结构。权重必须 `strict=True` 加载成功。

## 失败时怎么改（允许的修复）

可以改：

- 换 Python 小版本 / 重装 wheel
- `opset_version` 11→12（仅当 ONNX 导出报错）
- `rknn.load_onnx(..., input_size_list=[[1,1,480,640]])`（仅当 load 需要显式 shape）
- 镜像源、代理、手动下载 pth/whl 放到本目录

不可以改：

- 480×640、1 通道、FP16、输出 65 通道、文件名
- 加上 LightGlue / SuperGlue / descriptor
- `target_platform` 改成别的芯片

INT8 只有用户明确要求才做，且必须提供校准图列表。默认不要。

## 交给板端

把交付物拷到板端本仓库的 `models/`：

```bash
scp superpoint_640x480.rknn <user>@<board>:<catkin_ws>/src/VINS-Fusion-NPU/models/superpoint_640x480.rknn
```

YAML 已指向该路径：

`npu_model: "../models/superpoint_640x480.rknn"`

拷完后让用户在板子上重启：

```bash
roslaunch vins_npu fast_drone_250.launch
```

日志应出现：`[NPU] SuperPoint ready on NPU core 0/1/2`。若仍是 `cannot open model`，文件没放到 `models/` 或文件名不对。

## 本目录文件

| 文件 | 用途 |
|---|---|
| `AGENTS.md` | 本说明，AI 按这个做 |
| `convert_superpoint_rknn.py` | 导出 ONNX + 转 RKNN |
| `install_rknn_toolkit2.py` | 装 torch/onnx/rknn-toolkit2 |
| `README.md` | 给人看的短说明 |

不要把 `.venv`、`.pth`、`.onnx`、`.rknn` 当成源码去重构。交付物只有 `.rknn`。
