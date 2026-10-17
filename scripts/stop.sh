#!/usr/bin/env bash
# Stop vins_npu and the optional D430 / MAVROS processes started by this repo's scripts.
set -u
python3 - <<'PY'
import os, signal, time

me = os.getpid()
needles = (
    "/lib/vins_npu/vins_npu_node",
    "roslaunch vins_npu ",
    "/lib/mavros/mavros_node",
    "roslaunch mavros px4.launch",
    "roslaunch realsense2_camera rs_camera.launch",
    "realsense2_camera_manager",
    "realsense2_camera/RealSenseNodeFactory",
    "scripts/run_d430_px4_vins_npu.sh",
    "scripts/run_vins_npu.sh",
)

def cmdline(pid):
    try:
        return open("/proc/%d/cmdline" % pid, "rb").read().replace(b"\x00", b" ").decode("utf-8", "replace")
    except OSError:
        return ""

kill = []
for name in os.listdir("/proc"):
    if not name.isdigit():
        continue
    n = int(name)
    if n == me:
        continue
    cmd = cmdline(n)
    if not cmd:
        continue
    if any(s in cmd for s in needles):
        kill.append(n)

for n in kill:
    try:
        os.kill(n, signal.SIGTERM)
    except OSError:
        pass
time.sleep(1.5)
for n in kill:
    try:
        os.kill(n, signal.SIGKILL)
    except OSError:
        pass
print("stopped %d process(es): %s" % (len(kill), " ".join(str(x) for x in kill) or "-"))
PY
