//
// The MIT License (MIT)
//
// Copyright (c) 2022 Livox. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#ifndef LIVOX_DRIVER_PUB_HANDLER_H_
#define LIVOX_DRIVER_PUB_HANDLER_H_

#include <atomic>
#include <cstring>
#include <condition_variable> // std::condition_variable
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>              // std::mutex
#include <thread>

#include "livox_lidar_api.h"
#include "comm/comm.h"

namespace livox_ros {

class LidarPubHandler {
 public:
  LidarPubHandler();
  ~ LidarPubHandler() {}

  void PointCloudProcess(RawPacket& pkt);
  void SetLidarsExtParam(LidarExtParameter param);
  void GetLidarPointClouds(std::vector<PointXyzlt>& points_clouds);

  uint64_t GetRecentTimeStamp();
  uint32_t GetLidarPointCloudsSize();
  uint64_t GetLidarBaseTime();

 private:
  void LivoxLidarPointCloudProcess(RawPacket & pkt);
  void ProcessCartesianHighPoint(RawPacket & pkt);
  void ProcessCartesianLowPoint(RawPacket & pkt);
  void ProcessSphericalPoint(RawPacket & pkt);
  std::vector<PointXyzlt> points_clouds_;
  ExtParameterDetailed extrinsic_ = {
    {0, 0, 0},
    {
      {1, 0, 0},
      {0, 1, 1},
      {0, 0, 1}
    }
  };
  std::mutex mutex_;
  std::atomic_bool is_set_extrinsic_params_;
};
  
class PubHandler {
 public:
  using PointCloudsCallback = std::function<void(PointFrame*, void *)>;
  using ImuDataCallback = std::function<void(ImuData*, void*)>;
  using TimePoint = std::chrono::high_resolution_clock::time_point;

  PubHandler() {}

  ~ PubHandler() { Uninit(); }

  void Uninit();
  void RequestExit();
  void Init();
  void SetPointCloudConfig(const double publish_freq);
  void SetPointCloudsCallback(PointCloudsCallback cb, void* client_data);
  void AddLidarsExtParam(LidarExtParameter& extrinsic_params);
  void ClearAllLidarsExtrinsicParams();
  void SetImuDataCallback(ImuDataCallback cb, void* client_data);

  /// Bound how far raw_packet_queue_ may lag behind the newest packet.
  /// Call before SetPointCloudConfig(); takes effect on the next push.
  void SetQueueAgeLimit(uint32_t max_age_ms);

  /// Configure SCHED_FIFO promotion for point_process_thread_. Call before
  /// SetPointCloudConfig() -- the thread is created there and promoted
  /// immediately after creation using this config.
  void SetRealtimeScheduling(bool enable, int priority);

 private:
  //thread to process raw data
  void RawDataProcess();
  std::atomic<bool> is_quit_{false};
  std::shared_ptr<std::thread> point_process_thread_;
  std::mutex packet_mutex_;
  std::condition_variable packet_condition_;

  /// Drop the oldest queued packets (must be called with packet_mutex_ held)
  /// so the producer-consumer age gap never exceeds max_queue_age_ns_. This
  /// clamps publish latency to a bounded value instead of letting it grow
  /// unboundedly whenever RawDataProcess() is starved of CPU time.
  void EnforceQueueBoundLocked();

  // 200ms default matches roughly two publish frames at 10Hz -- generous
  // enough to absorb normal jitter, tight enough to keep the "point cloud
  // is stale" symptom from becoming user-visible in downstream consumers.
  uint64_t max_queue_age_ns_ = 200ULL * 1000000ULL;
  // Absolute backstop against queue growth if packet timestamps ever appear
  // to go backwards (e.g. a PTP resync), where age-based pruning above
  // can't tell "oldest" from "newest" reliably.
  static constexpr size_t kMaxQueueHardCapPackets = 8192;
  uint64_t dropped_packet_count_ = 0;
  TimePoint last_drop_log_time_{};

  bool rt_scheduling_enabled_ = true;
  int rt_priority_ = 60;

  //publish callback
  void CheckTimer(uint32_t id);
  void PublishPointCloud();
  static void OnLivoxLidarPointCloudCallback(uint32_t handle, const uint8_t dev_type,
                                             LivoxLidarEthernetPacket *data, void *client_data);
  
  static bool GetLidarId(LidarProtoType lidar_type, uint32_t handle, uint32_t& id);
  static uint64_t GetEthPacketTimestamp(uint8_t timestamp_type, uint8_t* time_stamp, uint8_t size);

  PointCloudsCallback points_callback_;
  void* pub_client_data_ = nullptr;

  ImuDataCallback imu_callback_;
  void* imu_client_data_ = nullptr;

  PointFrame frame_;

  std::deque<RawPacket> raw_packet_queue_;

  //pub config
  uint64_t publish_interval_ = 100000000; //100 ms
  uint64_t publish_interval_tolerance_ = 100000000; //100 ms
  uint64_t publish_interval_ms_ = 100; //100 ms
  TimePoint last_pub_time_;

  std::map<uint32_t, std::unique_ptr<LidarPubHandler>> lidar_process_handlers_;
  std::map<uint32_t, std::vector<PointXyzlt>> points_;
  std::map<uint32_t, LidarExtParameter> lidar_extrinsics_;
  static std::atomic<bool> is_timestamp_sync_;
  uint16_t lidar_listen_id_ = 0;
};

PubHandler &pub_handler();

}  // namespace livox_ros

#endif  // LIVOX_DRIVER_PUB_HANDLER_H_