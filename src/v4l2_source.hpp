// v4l2_source.hpp — optional real webcam source (V4L2, memory-mapped buffers, YUYV).
//
// Only used with `fp_producer --source v4l2:/dev/video0`. Works on native Linux and
// most VMs with USB passthrough; on WSL2 it needs usbipd-win plus a kernel with UVC
// support, which stock WSL2 kernels usually lack — check_env.sh tells you.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fp {

class V4L2Camera {
 public:
  ~V4L2Camera() { close(); }

  // Opens the device, negotiates YUYV at width x height and requests `fps`.
  bool open(const std::string& dev, uint32_t width, uint32_t height, uint32_t fps, std::string& err);

  // Blocks (up to 2 s) for the next filled buffer. The buffer stays owned by us until
  // requeue(). ts_ns is the driver's capture timestamp converted to ns; ts_monotonic
  // says whether the driver stamped it with CLOCK_MONOTONIC (UVC does).
  bool dequeue(const uint8_t** data, size_t* bytes, int64_t* ts_ns, bool* ts_monotonic,
               uint32_t* driver_seq, std::string& err);
  bool requeue(std::string& err);
  void close();

  double actual_fps() const { return actual_fps_; }

 private:
  struct Buf {
    void* ptr = nullptr;
    size_t len = 0;
  };
  int fd_ = -1;
  bool streaming_ = false;
  std::vector<Buf> bufs_;
  uint32_t cur_index_ = 0;
  bool have_cur_ = false;
  double actual_fps_ = 0;
};

// BT.601 limited-range YUYV (YUY2) -> packed RGB24.
void yuyv_to_rgb(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);

}  // namespace fp
