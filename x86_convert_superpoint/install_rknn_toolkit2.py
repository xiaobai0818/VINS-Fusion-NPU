#!/usr/bin/env python3
"""Install CPU torch + onnx + RKNN-Toolkit2 2.3.2 (x86_64 Linux / WSL)."""
from __future__ import print_function
import os
import platform
import subprocess
import sys
import urllib.request

WHEEL_BY_PY = {
    (3, 8): "rknn_toolkit2-2.3.2-cp38-cp38-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
    (3, 9): "rknn_toolkit2-2.3.2-cp39-cp39-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
    (3, 10): "rknn_toolkit2-2.3.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
    (3, 11): "rknn_toolkit2-2.3.2-cp311-cp311-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
    (3, 12): "rknn_toolkit2-2.3.2-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
}

BASE = (
    "https://github.com/airockchip/rknn-toolkit2/raw/master/"
    "rknn-toolkit2/packages/x86_64/"
)


def pip(args):
    cmd = [sys.executable, "-m", "pip"] + args
    print("+", " ".join(cmd))
    subprocess.check_call(cmd)


def main():
    if sys.platform.startswith("win") and "microsoft" not in platform.release().lower():
        # WSL reports linux. Native Windows must stop.
        if os.name == "nt":
            print("RKNN-Toolkit2 没有官方 Windows wheel。请在 WSL2 Ubuntu 里跑本脚本。")
            sys.exit(2)

    mach = platform.machine().lower()
    if mach not in ("x86_64", "amd64"):
        print("需要 x86_64，当前是", mach)
        sys.exit(2)

    py = sys.version_info[:2]
    if py not in WHEEL_BY_PY:
        print("需要 Python 3.8–3.12，当前是", sys.version)
        sys.exit(2)

    pip(["install", "-U", "pip", "wheel", "setuptools"])
    pip(["install", "torch", "onnx", "numpy", "opencv-python-headless"])

    name = WHEEL_BY_PY[py]
    here = os.path.dirname(os.path.abspath(__file__))
    whl = os.path.join(here, name)
    url = BASE + name
    if not os.path.isfile(whl):
        print("download", url)
        urllib.request.urlretrieve(url, whl)
    pip(["install", whl])

    from rknn.api import RKNN  # noqa: F401
    import torch
    import onnx

    print("ok: python", sys.version.split()[0], "torch", torch.__version__, "onnx", onnx.__version__)
    print("ok: rknn.api imported")


if __name__ == "__main__":
    main()
