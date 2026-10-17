#!/usr/bin/env python3
"""Export SuperPoint and convert to RKNN for RK3588.

Run this on an x86_64 PC with RKNN-Toolkit2. The Orange Pi cannot convert
.onnx -> .rknn (toolkit2 is not supported on-device).

  python3 convert_superpoint_rknn.py --export-onnx
  python3 convert_superpoint_rknn.py --to-rknn --onnx superpoint_640x480.onnx

Copy the resulting superpoint_640x480.rknn into this models/ directory.

Input:  uint8 gray NHWC 1x480x640x1  (mean=0, std=255 -> float 0..1)
Output: semi  [1, 65, 60, 80]  (dustbin in channel 64)
"""
from __future__ import print_function
import argparse
import os
import sys
import urllib.request

WEIGHT_URL = (
    "https://github.com/magicleap/SuperPointPretrainedNetwork/raw/master/superpoint_v1.pth"
)


def build_net():
    import torch
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
            # Keep descriptor weights so magicleap pth loads cleanly.
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
    print("wrote", out_path, "input 1x1x%dx%d" % (h, w))


def to_rknn(onnx_path, rknn_path, quant, dataset):
    try:
        from rknn.api import RKNN
    except ImportError:
        print(
            "rknn.api not found. Install RKNN-Toolkit2 on an x86_64 PC:\n"
            "  https://github.com/airockchip/rknn-toolkit2\n"
            "Then copy the .rknn file onto this board."
        )
        sys.exit(1)

    rknn = RKNN(verbose=True)
    # uint8 gray 0..255 -> network float 0..1
    rknn.config(
        mean_values=[[0]],
        std_values=[[255]],
        target_platform="rk3588",
    )
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        raise RuntimeError("load_onnx failed %s" % ret)
    if quant:
        if not dataset:
            raise RuntimeError("--dataset txt is required for INT8")
        ret = rknn.build(do_quantization=True, dataset=dataset)
    else:
        # RK3588 NPU runs this as FP16. SuperPoint scores are quantization-sensitive.
        ret = rknn.build(do_quantization=False)
    if ret != 0:
        raise RuntimeError("build failed %s" % ret)
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        raise RuntimeError("export_rknn failed %s" % ret)
    rknn.release()
    print("wrote", rknn_path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser()
    p.add_argument("--export-onnx", action="store_true")
    p.add_argument("--to-rknn", action="store_true")
    p.add_argument("--h", type=int, default=480)
    p.add_argument("--w", type=int, default=640)
    p.add_argument("--pth", default=os.path.join(here, "superpoint_v1.pth"))
    p.add_argument("--onnx", default=os.path.join(here, "superpoint_640x480.onnx"))
    p.add_argument("--rknn", default=os.path.join(here, "superpoint_640x480.rknn"))
    p.add_argument("--int8", action="store_true", help="INT8 (needs --dataset). Default FP16.")
    p.add_argument("--dataset", default="", help="calibration list for INT8")
    args = p.parse_args()

    if not args.export_onnx and not args.to_rknn:
        p.print_help()
        print("\nTypical PC workflow:")
        print("  python3 convert_superpoint_rknn.py --export-onnx")
        print("  python3 convert_superpoint_rknn.py --to-rknn")
        return

    if args.export_onnx:
        export_onnx(args.onnx, args.h, args.w, args.pth)
    if args.to_rknn:
        to_rknn(args.onnx, args.rknn, args.int8, args.dataset)


if __name__ == "__main__":
    main()
