# Shared helpers for VINS-Fusion-NPU scripts. Sourced, not executed.

PKG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

source_workspace() {
  if [ -f /opt/ros/noetic/setup.bash ]; then
    # shellcheck disable=SC1091
    source /opt/ros/noetic/setup.bash
  else
    echo "[ERROR] ROS Noetic not found at /opt/ros/noetic" >&2
    return 1
  fi

  if [ -n "${CATKIN_WS:-}" ] && [ -f "${CATKIN_WS}/devel/setup.bash" ]; then
    # shellcheck disable=SC1091
    source "${CATKIN_WS}/devel/setup.bash"
    return 0
  fi

  # Typical layout: <catkin_ws>/src/VINS-Fusion-NPU/scripts
  local ws
  ws="$(cd "${PKG_ROOT}/../.." && pwd)"
  if [ -f "${ws}/devel/setup.bash" ]; then
    # shellcheck disable=SC1091
    source "${ws}/devel/setup.bash"
    return 0
  fi

  if command -v rospack >/dev/null && rospack find vins_npu >/dev/null 2>&1; then
    return 0
  fi

  echo "[ERROR] catkin workspace not found. Build this package under <ws>/src, then:" >&2
  echo "  source <ws>/devel/setup.bash" >&2
  echo "  or set CATKIN_WS=/path/to/catkin_ws" >&2
  return 1
}

ensure_output_dir() {
  mkdir -p /tmp/vins_npu_output
}

raise_usbfs() {
  [ -r /sys/module/usbcore/parameters/usbfs_memory_mb ] || return 0
  local cur
  cur="$(cat /sys/module/usbcore/parameters/usbfs_memory_mb)"
  if [ "${cur:-0}" -ge 256 ]; then
    echo "[INFO] usbfs_memory_mb=${cur}" >&2
    return 0
  fi
  if echo 256 | sudo -n tee /sys/module/usbcore/parameters/usbfs_memory_mb >/dev/null 2>&1; then
    echo "[INFO] usbfs_memory_mb=256" >&2
  else
    echo "[WARN] could not raise usbfs_memory_mb (need passwordless sudo). current=${cur}" >&2
  fi
}
