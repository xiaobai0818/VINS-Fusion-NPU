# 拷到 x86 电脑后转换 SuperPoint

把 **整个** `x86_convert_superpoint` 文件夹拷到 x86 电脑（Linux 或 WSL2）。

在 Cursor 里打开这个文件夹，对 AI 说：

> 按 AGENTS.md 把 SuperPoint 转成 RK3588 的 superpoint_640x480.rknn，不要改输入尺寸。

或手动：

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 install_rknn_toolkit2.py
python3 convert_superpoint_rknn.py --all
scp superpoint_640x480.rknn <user>@<board>:<catkin_ws>/src/VINS-Fusion-NPU/models/
```

必须是 **x86_64 Linux**。Windows 原生 Python 转不了，用 WSL2。
