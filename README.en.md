# VINS-Fusion-NPU

[中文](README.md) | [English](README.en.md)

**Author:** Qiu Zheng (TYS小白)

VINS-Fusion on RK3588: SuperPoint corner detection runs on the triple-core NPU; LK optical flow and Ceres sliding-window BA stay on CPU.

The ROS package name is `vins_npu` (separate from upstream `vins`). The launch file keeps the node name `vins_fusion`, so topics remain `/vins_fusion/...` for any downstream pose bridge or planner.

The default config targets Fast Drone 250: RealSense D430 stereo IR + PX4 IMU. Drop this directory into any catkin workspace `src/` to build.

## Problems this repo solves

| Before | What this repo does |
|---|---|
| CPU `goodFeaturesToTrack` is slow on RK3588; Mali GPU is a poor fit for CUDA/OpenCL SuperPoint | SuperPoint **detect head** on RKNN **FP16**, bound to the triple-core NPU |
| Changing the frontend usually changes the ROS package / node name and breaks downstream | Package is `vins_npu`, **node name stays `vins_fusion`**, `/vins_fusion/...` unchanged |
| Patching the upstream `vins` package makes CPU and NPU builds hard to keep side by side | `vins_estimator` is split out; backend factors / initialization match upstream |
| NPU init failure kills the whole VIO | Automatic fallback to OpenCV corners; estimator keeps running |
| Unclear which of the three NPU cores is faster | `npu_mode: 0` benches combo vs triple at startup and picks the faster one |
| Model conversion only works on x86, with little onboard documentation | Ships `x86_convert_superpoint/` (README / AGENTS.md) |

This package extracts the estimator from [HKUST VINS-Fusion](https://github.com/HKUST-Aerial-Robotics/VINS-Fusion) and **only changes frontend detection**.

## Expected runtime behavior

With the model present, `npu_enable: 1`, and a working NPU driver, startup should look like:

```
[NPU] SuperPoint context on NPU core 0
[NPU] SuperPoint context on NPU core 1
[NPU] SuperPoint context on NPU core 2
[NPU] bench combo(CORE_0_1_2)=... ms  single-core0=... ms  triple-wait-left=... ms
[NPU] using combo: ...    or    using triple: ...
```

About every 2 s while running:

```
detect feature costs: xx.xx ms (n_max_cnt=... npu=1 got=...)
```

| What you see | Meaning |
|---|---|
| `n_max_cnt>0` and tens of ms | This refill used SuperPoint |
| `n_max_cnt=0`, ~0 ms | Tracks are already dense; this frame does not call the NPU (expected) |
| `/vins_fusion/imu_propagate` present | A pose bridge / planner can subscribe |
| `/vins_fusion/odometry`, `extrinsic` present | Sliding-window pose and extrinsics are publishing |
| `image_track` when `show_track: 1` | Tracking visualization is on |

### Onboard measurement (2026-09-10)

Orange Pi 5 Pro (RK3588, 8 cores, 8 GB), D430 stereo IR 640×480@15, PX4 IMU, `npu_enable: 1`, `npu_mode: 0` chose **combo** (`CORE_0_1_2`). Aircraft stationary.

Startup bench: `combo=52.66 ms`, `single-core0=98.67 ms`, `triple-wait-left=110.18 ms` → combo.

| Item | Measured |
|---|---|
| `vins_npu_node` CPU | Steady **~200–215%** (≈ **2.1 cores**; ~25% of an 8-core SoC). `top` 15 s mean 205%, range 188–216% |
| `vins_npu_node` memory | **RSS ~370–380 MB** (VmSize ≈ 1.8 GB, 17 threads) |
| NPU load | Combo uses all three cores together. Time average **Core0 ≈ 6%, Core1/2 ≈ 4%** (most frames skip inference when tracks are dense). During a refill **Core0 ~15–35%, Core1/2 ~10–22%** |
| SuperPoint one-shot | Bench combo **52.7 ms**; in-flight refill **~70–115 ms** (mean ~91 ms) |
| Left / right `/camera/infra1`, `infra2` | **15.0 Hz** |
| `/mavros/imu/data_raw` | **≈ 148 Hz** |
| `/vins_fusion/imu_propagate` | **≈ 148 Hz** |
| `/vins_fusion/odometry` | **≈ 7.5 Hz** (Ceres window; cannot keep 15 Hz images) |
| `/vins_fusion/image_track` | **≈ 15 Hz** |

Other nodes on the same board (not in the table): `mavros_node` ~12% CPU / 45 MB, RealSense manager ~9% / 89 MB. Whole-machine RAM used ~1.4 GB (including the OS), ~6 GB available.

Data flow:

```
D430 infra1/infra2 ──┐
                     ├──> vins_npu_node (node name vins_fusion)
PX4 IMU via MAVROS ──┘         │
                               ├ SuperPoint (NPU) new points
                               ├ LK tracking, stereo
                               └ Ceres sliding window
                                     │
                                     ├ /vins_fusion/imu_propagate
                                     ├ /vins_fusion/odometry
                                     └ /vins_fusion/extrinsic
```

## Differences from upstream VINS-Fusion

| | Upstream `vins` | This package `vins_npu` |
|---|---|---|
| New corners | OpenCV `goodFeaturesToTrack` | SuperPoint (RKNN), OpenCV fallback |
| Tracking | CPU LK | Same |
| Sliding-window BA | Ceres | Same |
| Node / topics | `vins_fusion` / `/vins_fusion/...` | Unchanged |

Without a `.rknn`, with `npu_enable: 0`, or if NPU init fails, behavior is close to CPU VINS.

## Layout

```
VINS-Fusion-NPU/
├── config/
│   ├── fast_drone_250.yaml   # main config (topics, extrinsics, NPU)
│   ├── left.yaml             # left pinhole intrinsics 640×480
│   └── right.yaml
├── launch/
│   ├── fast_drone_250.launch # flight
│   └── rviz.launch           # needs the upstream vins rviz config
├── scripts/
│   ├── run_d430_px4_vins_npu.sh  # D430 + PX4 MAVROS + this package
│   ├── run_vins_npu.sh           # estimator only
│   └── stop.sh
├── models/
│   └── superpoint_640x480.rknn   # deliverable; convert on x86 (not in git by default)
├── src/
│   ├── npu/                  # RKNN wrapper + SuperPoint decode / 3-core schedule
│   ├── featureTracker/       # tracking; NPU refill when n_max_cnt>0
│   ├── estimator/            # sliding window, parameters
│   ├── factor/  initial/  utility/
│   └── rosNodeTest.cpp       # vins_npu_node
├── third_party/rknn/         # rknn_api.h; optional vendored librknnrt.so
├── x86_convert_superpoint/   # copy this folder to x86 for conversion
├── CMakeLists.txt            # project name vins_npu
└── package.xml
```

## Dependencies

Board side (Orange Pi 5 / RK3588; this repo targets **ROS Noetic**):

- ROS Noetic: `roscpp`, `cv_bridge`, `image_transport`, `tf`, `nav_msgs`, `geometry_msgs`
- `camera_models` in the same workspace (from [HKUST VINS-Fusion](https://github.com/HKUST-Aerial-Robotics/VINS-Fusion); must be findable before building this package)
- OpenCV 4, Ceres, Eigen3
- **librknnrt.so** (hard CMake dependency even with `npu_enable: 0`)
  1. `third_party/rknn/lib/librknnrt.so`
  2. otherwise `/usr/lib` or `/usr/lib/aarch64-linux-gnu`

Launch prepends `$(find vins_npu)/third_party/rknn/lib` to `LD_LIBRARY_PATH`. If the system already has the RKNN runtime, the vendored path can be ignored.

Convert the model on **x86_64 Linux / WSL2**, not on the ARM board. Tooling is in `x86_convert_superpoint/`.

## SuperPoint model

Copy the whole `x86_convert_superpoint/` folder to an x86 machine. In Cursor, open that folder and ask the agent to follow `AGENTS.md`, or run manually:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 install_rknn_toolkit2.py
python3 convert_superpoint_rknn.py --all
scp superpoint_640x480.rknn \
  <user>@<board-ip>:<catkin_ws>/src/VINS-Fusion-NPU/models/
```

Must be **x86_64 Linux**. Native Windows Python cannot convert; use WSL2.

Deliverable: `models/superpoint_640x480.rknn`

| Item | Required (otherwise the board cannot decode points) |
|---|---|
| Chip | `rk3588` |
| Quantization | **FP16** (`do_quantization=False`) |
| Input | grayscale, 1 channel, 480×640, UINT8 |
| Preprocess | `mean=0`, `std=255` (0–255 → float 0–1) |
| Output | `semi` only, `[1, 65, 60, 80]` (64 cells + 1 dustbin) |
| Filename | must be `superpoint_640x480.rknn` |

Do not switch to RGB, INT8 (unless you recalibrate), or export descriptors / SuperGlue.

## Build

Place this repo under a catkin workspace `src/`. This package depends on upstream `camera_models`; include it in the whitelist:

```bash
cd <catkin_ws>
source /opt/ros/noetic/setup.bash
catkin_make -DCATKIN_WHITELIST_PACKAGES="camera_models;vins_npu"
source devel/setup.bash
```

`librknnrt.so not found`: copy the runtime into `third_party/rknn/lib/`, or install the system RKNN runtime.

## Configuration

Main file: `config/fast_drone_250.yaml`. NPU-related:

```yaml
npu_enable: 1
npu_mode: 0                 # 0=bench then pick combo/triple, 1=force combo, 2=force triple
npu_model: "../models/superpoint_640x480.rknn"
npu_score_th: 0.015         # SuperPoint score threshold; higher → fewer points
```

Relative paths are relative to the **yaml directory** (`config/`), so the default model is `models/superpoint_640x480.rknn`. Absolute paths also work.

| `npu_mode` | Behavior |
|---|---|
| `0` auto | Bench dummy images at startup: `CORE_0_1_2` combo vs triple wait-left; pick the faster one |
| `1` combo | One `rknn_run` bound to `CORE_0_1_2`, single thread, decode left only |
| `2` triple | Three resident NPU workers; wait on left only; core1/2 run right/previous frame but do not decode |

Current onboard pipeline (matches the yaml):

| Item | Value |
|---|---|
| IMU | `/mavros/imu/data_raw` |
| Left / right | `/camera/infra1/image_rect_raw`, `/camera/infra2/image_rect_raw` |
| Resolution | 640×480 |
| Sensors | 1 IMU + 2 cameras |
| `max_cnt` / `min_dist` | 150 / 30 |

When changing cameras or extrinsics, update `left.yaml` / `right.yaml` and `body_T_cam0` / `body_T_cam1` together. `output_path` defaults to `/tmp/vins_npu_output` (the scripts create it).

Disable NPU with `npu_enable: 0` (pure CPU `goodFeaturesToTrack`).

## Launch

Estimator only, when camera and IMU topics are already publishing:

```bash
source <catkin_ws>/devel/setup.bash
bash $(rospack find vins_npu)/scripts/run_vins_npu.sh
```

or:

```bash
roslaunch vins_npu fast_drone_250.launch
rosrun vins_npu vins_npu_node $(rospack find vins_npu)/config/fast_drone_250.yaml
```

Repo script for D430 stereo IR + PX4 MAVROS + this package (`realsense2_camera` and `mavros` must already be installed):

```bash
source <catkin_ws>/devel/setup.bash
bash $(rospack find vins_npu)/scripts/run_d430_px4_vins_npu.sh
```

FCU serial defaults to `/dev/ttyACM0:921600`; override with `FCU_URL`. Stop:

```bash
bash $(rospack find vins_npu)/scripts/stop.sh
```

RViz (needs the upstream `vins` package in the same workspace):

```bash
roslaunch vins_npu rviz.launch
```

## ROS interface

The node private namespace is `/vins_fusion/` (`name="vins_fusion"` in the launch file).

**Subscribe**

| Topic | Type | Notes |
|---|---|---|
| yaml `imu_topic` | `sensor_msgs/Imu` | default `/mavros/imu/data_raw` |
| yaml `image0_topic` | `sensor_msgs/Image` | left, converted to mono8 |
| yaml `image1_topic` | `sensor_msgs/Image` | right (stereo) |
| `/vins_restart` | `std_msgs/Bool` | reset estimator |
| `/vins_imu_switch` | `std_msgs/Bool` | enable/disable IMU |
| `/vins_cam_switch` | `std_msgs/Bool` | enable/disable second camera |

Stereo timestamp tolerance is 3 ms; mismatched frames are dropped.

**Publish (all under `/vins_fusion/`)**

| Topic | Type | Downstream |
|---|---|---|
| `imu_propagate` | `nav_msgs/Odometry` | IMU-propagated pose for a pose bridge / planner |
| `odometry` | `nav_msgs/Odometry` | sliding-window pose |
| `path` | `nav_msgs/Path` | visualization |
| `extrinsic` | `nav_msgs/Odometry` | camera extrinsics for mapping / grid_map |
| `image_track` | `sensor_msgs/Image` | tracking viz (`show_track: 1`) |
| `point_cloud` / `margin_cloud` / `key_poses` / `camera_pose` / … | viz | RViz |

## Known issues

The NPU **only accelerates frontend feature detection**. The rest of VINS cannot be moved onto the NPU.

| Module | Hardware | Status |
|---|---|---|
| SuperPoint corners | NPU, three cores | Done |
| LK optical flow | CPU (OpenCV) | Not a network; RKNN cannot run it |
| Ceres sliding-window BA | CPU | Sparse nonlinear optimization; RKNN cannot run it |
| Loop closure `loop_fusion` | — | **Not in this package** |

Other limits:

- SuperPoint conversion is **x86_64 Linux / WSL2** only; not on ARM. `.rknn` is not in git by default; copy it to `models/` on the board.
- In triple mode the right camera / previous frame only occupy cores; they are not decoded. VINS still gets left-camera new points + LK.
- `src/KITTIOdomTest.cpp` and `src/KITTIGPSTest.cpp` are leftover from upstream and **are not built**.
- `rviz.launch` depends on the upstream `vins` rviz config. On some RK3588 desktops RViz/Ogre SIGSEGV; that is independent of whether this package is publishing.
- Frontend point distribution differs from CPU VINS, so extrinsics/scale may feel different; tune `npu_score_th`, `max_cnt`, `min_dist`.

**Troubleshooting**

| Symptom | What to do |
|---|---|
| `[NPU] cannot open model` | `models/superpoint_640x480.rknn` missing or `npu_model` path wrong |
| `[NPU] rknn_init failed` | `librknnrt.so` / NPU driver; confirm the model is RK3588 FP16 |
| `SuperPoint unavailable` / `fall back to OpenCV` | CPU corners continue; VIO still runs |
| `npu=1` but `got=0` | raise contrast or slightly lower `npu_score_th`; confirm 640×480 gray input |
| build `librknnrt.so not found` | see Dependencies |
| `/vins_fusion/imu_propagate` gone | launch must use `name="vins_fusion"`, not `vins_npu` |
| extrinsics/scale differ from CPU VINS | see “frontend point distribution” above |

## Author

| | |
|---|---|
| This repo (NPU port) | **Qiu Zheng (TYS小白)** |
| Upstream VINS-Fusion | HKUST Aerial Robotics Group |

## License

Estimator: GPLv3, HKUST Aerial Robotics Group (VINS / VINS-Fusion).  
NPU frontend and changes in this repository: Qiu Zheng (TYS小白), also GPLv3.  
SuperPoint weights: MagicLeap `superpoint_v1.pth`; the conversion script exports the detect head only.  
RKNN headers / runtime: Rockchip.
