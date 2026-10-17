#!/usr/bin/env python3
"""MagicLeap SuperPoint (detect-only) -> ONNX -> RKNN FP16 for RK3588.

Board decoder expects:
  input  UINT8 gray  640x480  mean=0 std=255
  output semi [1, 65, 60, 80]
"""
from __future__ import print_function
import argparse
import os
import platform
import sys
import urllib.request

WEIGHT_URL = (
    "https://github.com/magicleap/SuperPointPretrainedNetwork/raw/master/superpoint_v1.pth"
)


def _need_x86():
    mach = platform.machine().lower()
    if mach not in ("x86_64", "amd64"):
        sys.exit("convert on x86_64 Linux/WSL only, this machine is " + mach)


def build_net():
    import torch.nn as nn

    class SuperPointDetect(nn.Module):
        def __init__(self):
            super(SuperPointDetect, self).__init__()
            self.relu = nn.ReLU(inplace=True)
            self.pool = nn.MaxPool2d(kernel_size=2, stride=2)
            c1, c2, c3, c4, c5 = 64, 64, 128, 128, 256
            self.conv1a = nn.Conv2d(1, c1, 3, 1, 1)
            self.conv1b = nn.Conv2d(c1, c1, 3, 1, 1)
            self.conv2a = nn.Conv2d(c1, c2, 3, 1, 1)
            self.conv2b = nn.Conv2d(c2, c2, 3, 1, 1)
            self.conv3a = nn.Conv2d(c2, c3, 3, 1, 1)
            self.conv3b = nn.Conv2d(c3, c3, 3, 1, 1)
            self.conv4a = nn.Conv2d(c3, c4, 3, 1, 1)
            self.conv4b = nn.Conv2d(c4, c4, 3, 1, 1)
            self.convPa = nn.Conv2d(c4, c5, 3, 1, 1)
            self.convPb = nn.Conv2d(c5, 65, 1, 1, 0)
            self.convDa = nn.Conv2d(c4, c5, 3, 1, 1)
            self.convDb = nn.Conv2d(c5, 256, 1, 1, 0)

        def forward(self, x):
            x = self.relu(self.conv1a(x))
            x = self.relu(self.conv1b(x))
            x = self.pool(x)
            x = self.relu(self.conv2a(x))
            x = self.relu(self.conv2b(x))
            x = self.pool(x)
            x = self.relu(self.conv3a(x))
            x = self.relu(self.conv3b(x))
            x = self.pool(x)
            x = self.relu(self.conv4a(x))
            x = self.relu(self.conv4b(x))
            return self.convPb(self.relu(self.convPa(x)))

    return SuperPointDetect()


def export_onnx(out_path, h, w, pth):
    import torch

    if not os.path.isfile(pth):
        print("download SuperPoint weights ->", pth)
        urllib.request.urlretrieve(WEIGHT_URL, pth)

    net = build_net()
    try:
        sd = torch.load(pth, map_location="cpu", weights_only=True)
    except TypeError:
        sd = torch.load(pth, map_location="cpu")
    net.load_state_dict(sd, strict=True)
    net.eval()
    dummy = torch.randn(1, 1, h, w)
    torch.onnx.export(
        net,
        dummy,
        out_path,
        input_names=["image"],
        output_names=["semi"],
        opset_version=11,
        do_constant_folding=True,
    )
    print("wrote", out_path, "NCHW 1x1x%dx%d -> semi 1x65x%dx%d" % (h, w, h // 8, w // 8))


def to_rknn(onnx_path, rknn_path, quant, dataset):
    try:
        from rknn.api import RKNN
    except ImportError:
        print("rknn.api 不存在。先运行: python3 install_rknn_toolkit2.py")
        sys.exit(1)

    if not os.path.isfile(onnx_path):
        sys.exit("missing onnx: " + onnx_path)

    rknn = RKNN(verbose=True)
    rknn.config(
        mean_values=[[0]],
        std_values=[[255]],
        target_platform="rk3588",
    )
    ret = rknn.load_onnx(model=onnx_path, input_size_list=[[1, 1, 480, 640]])
    if ret != 0:
        print("load_onnx with input_size_list failed, retry without it")
        ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        raise RuntimeError("load_onnx failed %s" % ret)
    if quant:
        if not dataset:
            raise RuntimeError("INT8 needs --dataset")
        ret = rknn.build(do_quantization=True, dataset=dataset)
    else:
        ret = rknn.build(do_quantization=False)
    if ret != 0:
        raise RuntimeError("build failed %s" % ret)
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        raise RuntimeError("export_rknn failed %s" % ret)
    rknn.release()
    size = os.path.getsize(rknn_path)
    print("wrote", rknn_path, "bytes", size)
    if size < 10000:
        raise RuntimeError("rknn file too small, conversion likely failed")


def main():
    _need_x86()
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser()
    p.add_argument("--all", action="store_true", help="export onnx then convert rknn (default if no flags)")
    p.add_argument("--export-onnx", action="store_true")
    p.add_argument("--to-rknn", action="store_true")
    p.add_argument("--h", type=int, default=480)
    p.add_argument("--w", type=int, default=640)
    p.add_argument("--pth", default=os.path.join(here, "superpoint_v1.pth"))
    p.add_argument("--onnx", default=os.path.join(here, "superpoint_640x480.onnx"))
    p.add_argument("--rknn", default=os.path.join(here, "superpoint_640x480.rknn"))
    p.add_argument("--int8", action="store_true")
    p.add_argument("--dataset", default="")
    args = p.parse_args()

    if args.h != 480 or args.w != 640:
        sys.exit("board C++ is wired for 640x480 only")

    do_onnx = args.export_onnx or args.all or (not args.export_onnx and not args.to_rknn)
    do_rknn = args.to_rknn or args.all or (not args.export_onnx and not args.to_rknn)

    if do_onnx:
        export_onnx(args.onnx, args.h, args.w, args.pth)
    if do_rknn:
        to_rknn(args.onnx, args.rknn, args.int8, args.dataset)
        print("DONE. Copy to Orange Pi:")
        print(
            "  scp %s <user>@<board>:<catkin_ws>/src/VINS-Fusion-NPU/models/superpoint_640x480.rknn"
            % args.rknn
        )


if __name__ == "__main__":
    main()
