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

#ifndef LIVOX_DRIVER_RT_SCHEDULING_H_
#define LIVOX_DRIVER_RT_SCHEDULING_H_

#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <thread>

namespace livox_ros {

// Best-effort SCHED_FIFO promotion for a driver worker thread. Point cloud
// packets are produced on this thread's peers under a shared mutex; on a
// CPU-contended host (e.g. co-located with another SCHED_FIFO consumer such
// as a downstream SLAM/localization stack), a plain CFS thread here can be
// starved arbitrarily long, letting published point cloud latency grow
// unbounded. Raising priority here does not fix contention by itself --
// pair with a bounded producer queue -- but it stops this thread from being
// the first one starved.
//
// Failure (e.g. missing CAP_SYS_NICE / rtprio ulimit) is non-fatal: this
// logs one line and leaves the thread on the default scheduler so driver
// startup is never blocked by a missing scheduling permission.
inline void ApplyRealtimeScheduling(std::thread &t, int priority,
                                     const char *thread_desc) {
  if (priority <= 0) {
    return;
  }
  sched_param sch{};
  sch.sched_priority = priority;
  int ret = pthread_setschedparam(t.native_handle(), SCHED_FIFO, &sch);
  if (ret != 0) {
    fprintf(stderr,
            "[livox_ros_driver2] WARNING: failed to set SCHED_FIFO(prio=%d) "
            "on %s thread (errno=%d: %s). Falling back to default CFS "
            "scheduling -- point cloud latency may grow under CPU "
            "contention. Grant CAP_SYS_NICE or raise the rtprio ulimit to "
            "fix, or set rt_scheduling:=false to silence this warning.\n",
            priority, thread_desc, ret, strerror(ret));
  } else {
    fprintf(stderr,
            "[livox_ros_driver2] %s thread set to SCHED_FIFO priority %d.\n",
            thread_desc, priority);
  }
}

}  // namespace livox_ros

#endif  // LIVOX_DRIVER_RT_SCHEDULING_H_
