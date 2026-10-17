#!/usr/bin/env bash
# D430 stereo IR + PX4 MAVROS IMU + vins_npu.
# Optional packages: realsense2_camera, mavros. Not part of this repo.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/_common.sh"
source_workspace
ensure_output_dir
raise_usbfs

FCU_URL="${FCU_URL:-/dev/ttyACM0:921600}"
ENABLE_DEPTH="${ENABLE_DEPTH:-false}"

need_pkg() {
  if ! rospack find "$1" >/dev/null 2>&1; then
    echo "[ERROR] ROS package '$1' not found. Install it in the same workspace or skip this script and run scripts/run_vins_npu.sh after your own camera/IMU are up." >&2
    exit 1
  fi
}

need_pkg realsense2_camera
need_pkg mavros
need_pkg vins_npu

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
}
PIDS=()
trap cleanup EXIT INT TERM

roslaunch mavros px4.launch fcu_url:="${FCU_URL}" &
PIDS+=($!)
sleep 5

roslaunch realsense2_camera rs_camera.launch \
  enable_color:=false \
  enable_infra:=false \
  enable_infra1:=true \
  enable_infra2:=true \
  enable_depth:="${ENABLE_DEPTH}" \
  enable_pointcloud:=false \
  enable_sync:=false \
  align_depth:=false \
  depth_width:=640 \
  depth_height:=480 \
  infra_width:=640 \
  infra_height:=480 \
  depth_fps:=15 \
  infra_fps:=15 &
PIDS+=($!)

echo "[INFO] waiting for /camera/infra1/image_rect_raw ..." >&2
ok=0
for _ in $(seq 1 20); do
  if timeout -k 1 2 rostopic echo -n 1 /camera/infra1/image_rect_raw >/dev/null 2>&1; then
    ok=1
    break
  fi
done
if [ "$ok" -ne 1 ]; then
  echo "[ERROR] D430 did not publish infra1. Check USB3 and that no other process holds the camera." >&2
  exit 1
fi
echo "[INFO] D430 infra1 is streaming." >&2

# Request HIGHRES_IMU at 200 Hz if the FCU accepts it (best-effort).
rosrun mavros mavcmd long 511 105 5000 0 0 0 0 0 >/dev/null 2>&1 || true
rosrun mavros mavcmd long 511 31 5000 0 0 0 0 0 >/dev/null 2>&1 || true

roslaunch vins_npu fast_drone_250.launch &
PIDS+=($!)
wait
