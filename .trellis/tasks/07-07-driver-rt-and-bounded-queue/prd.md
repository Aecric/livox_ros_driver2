# driver-rt-and-bounded-queue: livox 驱动 RT 化 + 点云队列限界丢旧

## Goal

消除 livox_ros_driver2 点云发布的延迟棘轮：实测（jqrzx0706, 2026-07-06 日志）点云 stamp→发布延迟从 0.2s 阶梯式爬升到 0.9s 且永不回落，导致下游 lightning 定位 TF 滞后近 1 秒、nav2 跟不上。根因两条腿都要修：
1. **调度**：驱动全程 CFS，被 lightning 的 SCHED_FIFO 78–90 线程在共享核上无条件抢占（lightning 的 `protect_processes` 语义是"把 livox 留在 reserved 核上"）。
2. **设计**：`PubHandler::raw_packet_queue_` 无界、无丢帧机制，每次被饿超过一个帧周期就永久多驻留 ~100ms 数据（棘轮）。

证据链：`/home/aecriclin/3d_slam_ws/src/lightning-lm/docs/bugreport0707.md`（补充章节）。

## What I already know

* 症状仅点云通路：IMU 在 SDK 回调内直接同步发布（pub_handler.cpp:111-127，不过队列），全程新鲜；点云包走 `raw_packet_queue_`（无界 deque，pub_handler.cpp:147-150）→ 单线程 `RawDataProcess`（pub_handler.cpp:70 创建, :229 主循环）→ `CheckTimer` 定时发布。
* NoSync 模式下 stamp = 主机墙钟在 SDK 回调时刻盖章（pub_handler.cpp:274），所以"发布延迟"直接可用 `ros2 topic delay /livox/lidar` 观测。
* 线程清单（RT 化对象）：
  - `PubHandler::point_process_thread_`（RawDataProcess，积压主嫌疑）
  - `DriverNode::pointclouddata_poll_thread_` / `imudata_poll_thread_`（driver_node.h:48-49）
  - Livox-SDK2 内部收包线程（外部依赖，本 repo 不直接创建）
* 本 fork 已有参数扩展先例：`qos_lidar` / `qos_pointcloud` / `qos_imu`（livox_ros_driver2.cpp:155-157），新参数照此模式加。
* 部署形态：`build_deb.sh` Docker buildx 打 .deb（humble/jazzy/lyrical × amd64/arm64）；repo 内**无** systemd unit / postinst；机器人上由外部方式启动。
* 对照参照：lightning 侧 cpu_affinity 的做法——无权限（CAP_SYS_NICE / rtprio limits）时打 WARNING 降级运行，不阻断启动。

## Assumptions (temporary)

* 机器人上驱动与 lightning 同机运行，内核允许 rtprio（lightning 自己已成功用 FIFO 78-90，说明权限通路存在或以 root 跑）。
* Mid360 点云包速率 ~1000+ pkt/s，正常处理每包 <1ms；限界阈值以"数据时长"计比以"包数"计更直观。

## Open Questions

（无 — 三个决策已全部落定，见 Decision 节）

## Decision (ADR-lite)

### D1: RT 化落点 = 代码内参数化（2026-07-07 已确认）

**Context**: 驱动 CFS 线程被同机 lightning FIFO 线程抢占，需要给关键线程提权；候选落点为代码内 / systemd / 两者。
**Decision**: 驱动内新增 `rt_scheduling` / `rt_priority` 参数，`pthread_setschedparam` 只提关键线程（`point_process_thread_`、两个 poll 线程）；无 CAP_SYS_NICE 时 WARNING 降级不阻断。
**Consequences**: 与启动方式解耦、.deb 升级即生效、粒度最细（SDK 收包线程提不到——留在文档说明）；需要机器人放开 rtprio limits 或以 root 运行（lightning 已验证该通路存在）。

### D2: 队列限界 = 按数据年龄丢最旧，默认 200ms（2026-07-07 已确认）

**Context**: `raw_packet_queue_` 无界导致延迟棘轮；"界"可按年龄 / 包数 / 双重定义。
**Decision**: 新增 `max_queue_age_ms`（默认 200ms ≈ 2 帧）：入队时若队首包 stamp 落后队尾超阈值则丢最旧，配限频告警 + 累计丢弃计数。
**Consequences**: 直接钳制用户可见延迟上限，与雷达型号/包速率无关；依赖包内 stamp 单调性（NoSync 模式 stamp 为主机墙钟盖章，天然单调；异常回跳时按 FIFO 长度兜底可在实现时低成本加上）。

### D3: 默认全开（2026-07-07 已确认，用户选择，非推荐项）

**Context**: .deb 升级后老部署是否立即获得新行为。
**Decision**: `rt_scheduling` 与队列限界均默认开启。RT 默认优先级取 60（SCHED_FIFO）：高于一切 CFS（KISS/SC/UI），低于 lightning 关键 tier（imu 90 / lio 85 / ndt 78 / pgo 70），驱动每包工作量 sub-ms，低位 RT 已足以消除饥饿。
**Consequences**: 装上即修复，机器人无需改启动配置；无 rtprio 权限的环境启动时刷一次 WARNING（限一次，不刷屏）后按 CFS 降级运行；调度敏感环境可通过参数显式关闭或调低优先级。

## Requirements (final)

* R1: `rt_scheduling`（bool，默认 true）+ `rt_priority`（int，默认 60）参数；对 `point_process_thread_`、`pointclouddata_poll_thread_`、`imudata_poll_thread_` 调 `pthread_setschedparam(SCHED_FIFO)`；失败（EPERM 等）单次 WARNING 降级，不阻断。
* R2: `max_queue_age_ms`（int，默认 200）参数；`raw_packet_queue_` 入队时检测队首/队尾 stamp 差，超阈值丢最旧；限频告警（如每 5s 一条，含累计丢弃包数）；stamp 回跳时按队列长度上限兜底。
* R3: 参数声明/读取遵循现有 `qos_*` 模式（livox_ros_driver2.cpp），launch_ROS2 各 launch 文件同步透传；humble/jazzy/lyrical 三发行版编译通过。

## Acceptance Criteria

* [ ] 复现场景（与 lightning 同机、RT 线程满载）下 `ros2 topic delay /livox/lidar` 稳定在 ~0.1s 量级，观察 ≥5 分钟无单调爬升。**需要真实机器人/同机负载场景验证，本次会话未做（无硬件访问）。**
* [ ] 人为制造 CPU 饥饿（如 stress-ng FIFO 占核）后，延迟在压力解除后回落（不再棘轮）。**同上，待硬件验证。**
* [x] 无 CAP_SYS_NICE 权限时驱动正常启动，日志出现降级 WARNING —— `ApplyRealtimeScheduling` 失败路径为非致命 WARNING，不 abort（rt_scheduling.h）。
* [x] 丢包发生时有限频日志与计数，不刷屏 —— `EnforceQueueBoundLocked` 限频 5s 一条 + 累计计数（pub_handler.cpp）。
* [x] jazzy 编译通过 —— Docker 隔离拷贝验证，colcon build 成功（2026-07-07，见下方 Verification）。
* [ ] humble + lyrical + arm64 全矩阵 `.deb` 构建（`build_deb.sh`）——本次会话未跑，建议合并前跑一次完整矩阵。

## Definition of Done

* [x] README 记录新参数（rt_scheduling / rt_priority / max_queue_age_ms）+ 与 lightning `protect_processes` 配合的推荐配置说明。
* [x] CHANGELOG 记录新行为（[Unreleased] 节）。
* [ ] 上述硬件相关验收标准跑通（见 Acceptance Criteria 未勾选项）。

## Verification (2026-07-07)

* 编译：在 `lightning-jazzy:dev` 容器内，将 `src/livox_ros_driver2` 拷贝到 `/tmp/ws_verify`（不污染宿主机 checkout）、暂存 `package_ROS2.xml → package.xml` + `launch_ROS2/ → launch/`（此仓库 colcon 构建前必需的既有步骤，`build.sh` 亦如此做），`colcon build --packages-select livox_ros_driver2 --cmake-args -DROS_EDITION=ROS2 -DDISTRO_ROS=jazzy` **Finished, 0 error**（仅两条与本次改动无关的预置警告：FLANN CMP0144 policy、未使用的 DISTRO_ROS 变量）。
* 未做：连接真实 Mid360 / 同机 lightning 负载下的 `ros2 topic delay` 实测——需要硬件，超出本次交互式会话能力范围。建议下一步在机器人上按 Acceptance Criteria 前两项验证。

## Out of Scope (explicit)

* 不改 lightning 侧 cpu_affinity 语义（另行任务）。
* 不动 Livox-SDK2 内部线程（收包线程在 SDK 内，改不到；如需要只在文档中说明）。
* 不引入 PTP/gPTP 时间同步配置。

## Technical Notes

* 关键文件：`src/comm/pub_handler.cpp`（队列 + worker 线程 + 发布节拍）、`src/comm/pub_handler.h`、`src/driver_node.{h,cpp}`（poll 线程）、`src/livox_ros_driver2.cpp`（参数声明）、`launch_ROS2/*.py`（参数下发）。
* NoSync 发布路径的固定节拍调度器（pub_handler.cpp:195-225）`last_pub_time_ += interval` 的追赶行为与限界策略有交互，实现时需确认丢旧后 base_time 语义仍正确。
* 实测数据（07-06 日志）：阶梯步长 ≈ 整数个 100ms 帧周期；`[SyncPkg]` 处年龄 232→838ms 与 Align 入口同形。
