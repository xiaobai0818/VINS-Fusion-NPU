# VINS-Fusion-NPU

[中文](README.md) | [English](README.en.md)

**作者：** Qiu Zheng（TYS小白）

RK3588 上的 VINS-Fusion：前端 SuperPoint 角点检测走三核 NPU，LK 光流和 Ceres 滑窗仍在 CPU。

ROS 包名是 `vins_npu`（独立于原版 `vins`）。Launch 里节点名仍叫 `vins_fusion`，话题仍是 `/vins_fusion/...`，下游只要订这些话题即可。

默认配置面向 Fast Drone 250：RealSense D430 双目红外 + PX4 IMU。把本目录放到任意 catkin workspace 的 `src/` 即可编译。

## 当前解决的问题

| 原先的问题 | 本仓库怎么处理 |
|---|---|
| RK3588 上 CPU `goodFeaturesToTrack` 补点偏慢，Mali GPU 又不适合 CUDA/OpenCL SuperPoint | 把 SuperPoint **detect 头**转到 RKNN **FP16**，绑三核 NPU |
| 换前端会改 ROS 包名 / 节点名，下游全部要改 | 包名独立为 `vins_npu`，**节点名仍是 `vins_fusion`**，`/vins_fusion/...` 不变 |
| 直接改原版 `vins` 包，CPU 版和 NPU 版没法并存 | 从 VINS-Fusion 拆出 `vins_estimator` 做成独立包，后端因子 / 初始化与原版同一套 |
| NPU 初始化失败就整条 VIO 挂掉 | 失败自动退回 OpenCV 角点，估计器继续跑 |
| 三核怎么用不知道哪种更快 | `npu_mode: 0` 启动时 bench combo vs triple，选更快的 |
| 模型只能在 x86 转，板端缺工具链说明 | 自带 `x86_convert_superpoint/`（README / AGENTS.md） |

本包从 [HKUST VINS-Fusion](https://github.com/HKUST-Aerial-Robotics/VINS-Fusion) 拆出估计器，**只改前端检测**。

## 运行预期效果

模型在、`npu_enable: 1`、NPU 驱动正常时，启动后应看到：

```
[NPU] SuperPoint context on NPU core 0
[NPU] SuperPoint context on NPU core 1
[NPU] SuperPoint context on NPU core 2
[NPU] bench combo(CORE_0_1_2)=... ms  single-core0=... ms  triple-wait-left=... ms
[NPU] using combo: ...    或    using triple: ...
```

运行中大约每 2 s：

```
detect feature costs: xx.xx ms (n_max_cnt=... npu=1 got=...)
```

| 现象 | 含义 |
|---|---|
| `n_max_cnt>0` 且耗时几十 ms | 本帧补点走了 SuperPoint |
| `n_max_cnt=0`、耗时 ~0 ms | 跟踪已够密，本帧不调 NPU，属正常 |
| 有 `/vins_fusion/imu_propagate` | 下游位姿桥 / 规划器可以接 |
| 有 `/vins_fusion/odometry`、`extrinsic` | 滑窗位姿和外参在发 |
| `show_track: 1` 时有 `image_track` | 能看跟踪点 |

### 机载实测（2026-09-10）

Orange Pi 5 Pro（RK3588，8 核，8 GB），D430 双目红外 640×480@15，PX4 IMU，`npu_enable: 1`，`npu_mode: 0` 选中 **combo**（`CORE_0_1_2`）。机载静止。

本次启动 bench：`combo=52.66 ms`，`single-core0=98.67 ms`，`triple-wait-left=110.18 ms` → 使用 combo。

| 项 | 实测 |
|---|---|
| `vins_npu_node` CPU | 稳态 **约 200–215%**（≈ **2.1 核**；8 核上约占整机 25%）。`top` 15 s 均值 205%，范围 188–216% |
| `vins_npu_node` 内存 | **RSS 约 370–380 MB**（VmSize ≈ 1.8 GB，17 线程） |
| NPU 占用 | combo 三核一起跑。时间平均 **Core0 ≈ 6%，Core1/2 ≈ 4%**（跟踪够密时多数帧不推理）。补点瞬间 **Core0 约 15–35%，Core1/2 约 10–22%** |
| SuperPoint 单次 | bench combo **52.7 ms**；运行中真正补点 **约 70–115 ms**（均值约 91 ms） |
| 左 / 右目 `/camera/infra1`、`infra2` | **15.0 Hz** |
| `/mavros/imu/data_raw` | **≈ 148 Hz** |
| `/vins_fusion/imu_propagate` | **≈ 148 Hz** |
| `/vins_fusion/odometry` | **≈ 7.5 Hz**（Ceres 滑窗，跟不上相机 15 Hz） |
| `/vins_fusion/image_track` | **≈ 15 Hz** |

同机其它节点量级（不计入上表）：`mavros_node` ~12% CPU / 45 MB，RealSense manager ~9% / 89 MB。整机内存占用约 1.4 GB（含系统），可用约 6 GB。

数据流：

```
D430 infra1/infra2 ──┐
                     ├──> vins_npu_node (节点名 vins_fusion)
PX4 IMU via MAVROS ──┘         │
                               ├ SuperPoint (NPU) 补新点
                               ├ LK 跟踪左右目
                               └ Ceres 滑窗
                                     │
                                     ├ /vins_fusion/imu_propagate
                                     ├ /vins_fusion/odometry
                                     └ /vins_fusion/extrinsic
```

## 和原版 VINS-Fusion 的差别

| | 原版 `vins` | 本包 `vins_npu` |
|---|---|---|
| 新点检测 | OpenCV `goodFeaturesToTrack` | SuperPoint（RKNN），失败则退回 OpenCV |
| 光流跟踪 | CPU LK | 同左 |
| 滑窗 BA | Ceres | 同左 |
| 节点名 / 话题 | `vins_fusion` / `/vins_fusion/...` | 保持不变 |

没有 `.rknn`、`npu_enable: 0`、或 NPU 初始化失败时，行为接近原版 CPU VINS。

## 目录

```
VINS-Fusion-NPU/
├── config/
│   ├── fast_drone_250.yaml   # 主配置（话题、外参、NPU）
│   ├── left.yaml             # 左目针孔内参 640×480
│   └── right.yaml
├── launch/
│   ├── fast_drone_250.launch # 正式起飞用
│   └── rviz.launch           # 依赖原版 vins 包里的 rviz 配置
├── scripts/
│   ├── run_d430_px4_vins_npu.sh  # D430 + PX4 MAVROS + 本包
│   ├── run_vins_npu.sh           # 只起估计器
│   └── stop.sh
├── models/
│   └── superpoint_640x480.rknn   # 交付物，x86 转完拷到这里（默认不进 git）
├── src/
│   ├── npu/                  # RKNN 封装 + SuperPoint 解码 / 三核调度
│   ├── featureTracker/       # 跟踪；n_max_cnt>0 时走 NPU 补点
│   ├── estimator/            # 滑窗、参数
│   ├── factor/  initial/  utility/
│   └── rosNodeTest.cpp       # vins_npu_node
├── third_party/rknn/         # rknn_api.h；可选 vendored librknnrt.so
├── x86_convert_superpoint/   # 整目录拷到 x86 转模型（见该目录 README / AGENTS.md）
├── CMakeLists.txt            # 工程名 vins_npu
└── package.xml
```

## 依赖

板端（Orange Pi 5 / RK3588，本仓库按 **ROS Noetic**）：

- ROS Noetic：`roscpp`、`cv_bridge`、`image_transport`、`tf`、`nav_msgs`、`geometry_msgs`
- 同 workspace 的 `camera_models`（来自 [HKUST VINS-Fusion](https://github.com/HKUST-Aerial-Robotics/VINS-Fusion) 的 `camera_models`，编译本包前要能找到）
- OpenCV 4、Ceres、Eigen3
- **librknnrt.so**（CMake 硬依赖，即使 `npu_enable: 0` 也要能链接）
  1. `third_party/rknn/lib/librknnrt.so`
  2. 否则 `/usr/lib` 或 `/usr/lib/aarch64-linux-gnu`

Launch 会把 `$(find vins_npu)/third_party/rknn/lib` 加到 `LD_LIBRARY_PATH`。系统已装 rknn 运行时则可忽略 vendored 路径。

转换模型在 **x86_64 Linux / WSL2**，不要在 ARM 板上转。工具链见 `x86_convert_superpoint/`。

## SuperPoint 模型

把整个 `x86_convert_superpoint/` 拷到 x86 电脑，在 Cursor 里打开后让 AI 按其中 `AGENTS.md` 执行，或手动：

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 install_rknn_toolkit2.py
python3 convert_superpoint_rknn.py --all
scp superpoint_640x480.rknn \
  <user>@<板子IP>:<catkin_ws>/src/VINS-Fusion-NPU/models/
```

必须是 **x86_64 Linux**。Windows 原生 Python 转不了，用 WSL2。

交付文件：`models/superpoint_640x480.rknn`

| 项 | 约定（改了板端解不出点） |
|---|---|
| 芯片 | `rk3588` |
| 量化 | **FP16**（`do_quantization=False`） |
| 输入 | 灰度 1 通道，480×640，UINT8 |
| 预处理 | `mean=0`，`std=255`（0–255 → float 0–1） |
| 输出 | 仅 `semi`，`[1, 65, 60, 80]`（64 cell + 1 dustbin） |
| 文件名 | 必须叫 `superpoint_640x480.rknn` |

不要改成 RGB、INT8（除非另做校准）、不要导出 descriptor / SuperGlue。

## 编译

把本仓库放到 catkin workspace 的 `src/` 下。本包依赖原版 `camera_models`，白名单里带上：

```bash
cd <catkin_ws>
source /opt/ros/noetic/setup.bash
catkin_make -DCATKIN_WHITELIST_PACKAGES="camera_models;vins_npu"
source devel/setup.bash
```

`librknnrt.so not found`：把运行时拷到 `third_party/rknn/lib/`，或安装系统 RKNN runtime。

## 配置

主文件：`config/fast_drone_250.yaml`。NPU 相关：

```yaml
npu_enable: 1
npu_mode: 0                 # 0=启动时 bench 后选 combo/triple，1=强制 combo，2=强制 triple
npu_model: "../models/superpoint_640x480.rknn"
npu_score_th: 0.015         # SuperPoint 分数阈值，越大点越少
```

相对路径相对 **yaml 所在目录**（`config/`），因此默认模型是 `models/superpoint_640x480.rknn`。也可用绝对路径。

| `npu_mode` | 行为 |
|---|---|
| `0` auto | 启动时对 dummy 图 bench：`CORE_0_1_2` combo vs 三核 wait-left，选更快的 |
| `1` combo | 一次 `rknn_run` 绑 `CORE_0_1_2`，单线程，只 decode 左目 |
| `2` triple | 三个常驻 NPU worker；只等左目结果；core1/2 跑右目/上一帧但不 decode |

当前机载链路（与 yaml 一致）：

| 项 | 值 |
|---|---|
| IMU | `/mavros/imu/data_raw` |
| 左 / 右目 | `/camera/infra1/image_rect_raw`，`/camera/infra2/image_rect_raw` |
| 分辨率 | 640×480 |
| 传感器 | 1 IMU + 2 cam |
| `max_cnt` / `min_dist` | 150 / 30 |

改相机或外参时同步改 `left.yaml` / `right.yaml` 和 `body_T_cam0` / `body_T_cam1`。`output_path` 默认 `/tmp/vins_npu_output`（脚本会创建该目录）。

关掉 NPU：`npu_enable: 0`，纯 CPU `goodFeaturesToTrack`。

## 启动

相机和 IMU 已经在发时，只起估计器：

```bash
source <catkin_ws>/devel/setup.bash
bash $(rospack find vins_npu)/scripts/run_vins_npu.sh
```

或：

```bash
roslaunch vins_npu fast_drone_250.launch
rosrun vins_npu vins_npu_node $(rospack find vins_npu)/config/fast_drone_250.yaml
```

本仓库脚本拉起 D430 双目红外 + PX4 MAVROS + 本包（需同环境已安装 `realsense2_camera`、`mavros`）：

```bash
source <catkin_ws>/devel/setup.bash
bash $(rospack find vins_npu)/scripts/run_d430_px4_vins_npu.sh
```

飞控串口默认 `/dev/ttyACM0:921600`，可用 `FCU_URL` 覆盖。停掉：

```bash
bash $(rospack find vins_npu)/scripts/stop.sh
```

RViz（需原版 `vins` 包在同一 workspace）：

```bash
roslaunch vins_npu rviz.launch
```

## ROS 接口

节点私有命名空间是 `/vins_fusion/`（launch 里 `name="vins_fusion"`）。

**订阅**

| 话题 | 类型 | 说明 |
|---|---|---|
| yaml `imu_topic` | `sensor_msgs/Imu` | 默认 `/mavros/imu/data_raw` |
| yaml `image0_topic` | `sensor_msgs/Image` | 左目，转成 mono8 |
| yaml `image1_topic` | `sensor_msgs/Image` | 右目（双目时） |
| `/vins_restart` | `std_msgs/Bool` | 复位估计器 |
| `/vins_imu_switch` | `std_msgs/Bool` | 开关 IMU |
| `/vins_cam_switch` | `std_msgs/Bool` | 开关第二目 |

双目时间戳容差 3 ms，对不齐会丢帧。

**发布（均在 `/vins_fusion/` 下）**

| 话题 | 类型 | 下游 |
|---|---|---|
| `imu_propagate` | `nav_msgs/Odometry` | IMU 传播位姿，给 pose bridge / 规划器 |
| `odometry` | `nav_msgs/Odometry` | 滑窗位姿 |
| `path` | `nav_msgs/Path` | 可视化 |
| `extrinsic` | `nav_msgs/Odometry` | 相机外参，给建图 / grid_map |
| `image_track` | `sensor_msgs/Image` | 跟踪可视化（`show_track: 1`） |
| `point_cloud` / `margin_cloud` / `key_poses` / `camera_pose` / … | 可视化 | RViz |

## 当前存在的问题

NPU **只能加速前端特征检测**，不能把整个 VINS 搬上 NPU。

| 模块 | 硬件 | 原因 |
|---|---|---|
| SuperPoint 角点检测 | NPU 三核 | 已做 |
| LK 光流跟踪 | CPU（OpenCV） | 不是网络，RKNN 做不了 |
| Ceres 滑窗 BA | CPU | 稀疏非线性优化，RKNN 做不了 |
| 回环 `loop_fusion` | — | **本包不含**，不要指望这里做回环 |

其它限制：

- SuperPoint 只能在 **x86_64 Linux / WSL2** 上转；ARM 板转不了。`.rknn` 默认不进 git，板端要自己拷到 `models/`。
- Triple 模式里右目 / 上一帧只占核、不 decode；真正进 VINS 的仍是左目新点 + LK。
- `src/KITTIOdomTest.cpp`、`src/KITTIGPSTest.cpp` 是上游残留，**没有编进本包**。
- `rviz.launch` 依赖原版 `vins` 包里的 rviz 配置；部分 RK3588 桌面上 RViz/Ogre 会 SIGSEGV，与本包话题是否在发无关。
- 前端点分布和 CPU 版不同，外参/尺度手感可能不一致，需调 `npu_score_th`、`max_cnt`、`min_dist`。

**故障排查**

| 现象 | 处理 |
|---|---|
| `[NPU] cannot open model` | `models/superpoint_640x480.rknn` 不存在或 `npu_model` 路径不对 |
| `[NPU] rknn_init failed` | `librknnrt.so` / NPU 驱动；确认是 RK3588 的 FP16 模型 |
| `SuperPoint unavailable` / `fall back to OpenCV` | 继续用 CPU 角点，VIO 仍可跑 |
| `npu=1` 但 `got=0` | 提高对比度或略降 `npu_score_th`；确认输入是 640×480 灰度 |
| 编译 `librknnrt.so not found` | 见「依赖」 |
| `/vins_fusion/imu_propagate` 没了 | launch 必须 `name="vins_fusion"`，不要改成 `vins_npu` |
| 和 CPU 版外参/尺度不一致 | 见上面「前端点分布」 |

## 作者

| | |
|---|---|
| 本仓库（NPU 适配） | **Qiu Zheng（TYS小白）** |
| 原版 VINS-Fusion | HKUST Aerial Robotics Group |

## 许可

估计器主体：GPLv3，HKUST Aerial Robotics Group（VINS / VINS-Fusion）。  
NPU 前端与本仓库改动：Qiu Zheng（TYS小白），同样遵循 GPLv3。  
SuperPoint 权重：MagicLeap `superpoint_v1.pth`，转换脚本只导出 detect 头。  
RKNN 头文件 / runtime：Rockchip。
