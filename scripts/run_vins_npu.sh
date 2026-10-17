#!/usr/bin/env bash
# Launch vins_npu only. Camera and IMU topics must already be publishing.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/_common.sh"
source_workspace
ensure_output_dir

if ! rospack find vins_npu >/dev/null 2>&1; then
  echo "[ERROR] package vins_npu not on ROS_PACKAGE_PATH" >&2
  exit 1
fi

exec roslaunch vins_npu fast_drone_250.launch
