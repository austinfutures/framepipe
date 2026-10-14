// v4l2_source.cpp — see v4l2_source.hpp.
#include "v4l2_source.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fp {
namespace {

int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

std::string errstr(const char* what) { return std::string(what) + ": " + strerror(errno); }

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

}  // namespace

bool V4L2Camera::open(const std::string& dev, uint32_t width, uint32_t height, uint32_t fps,
                      std::string& err) {
  fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
  if (fd_ < 0) return err = errstr(("open " + dev).c_str()), false;

  v4l2_capability cap{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) return err = errstr("VIDIOC_QUERYCAP"), false;
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING))
    return err = dev + " is not a streaming capture device", false;

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) return err = errstr("VIDIOC_S_FMT"), false;
  if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height ||
      fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
    err = "camera refused YUYV " + std::to_string(width) + "x" + std::to_string(height) +
          " (got " + std::to_string(fmt.fmt.pix.width) + "x" + std::to_string(fmt.fmt.pix.height) +
          "); try `v4l2-ctl --list-formats-ext`";
    return false;
  }

  // Ask for the frame rate. Many webcams cap YUYV at 30 fps (USB 2 bandwidth); we
  // read back what the driver actually chose and report it.
  v4l2_streamparm parm{};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = fps;
  xioctl(fd_, VIDIOC_S_PARM, &parm);  // best effort
  if (xioctl(fd_, VIDIOC_G_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator)
    actual_fps_ = double(parm.parm.capture.timeperframe.denominator) /
                  parm.parm.capture.timeperframe.numerator;

  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) return err = errstr("VIDIOC_REQBUFS"), false;
  if (req.count < 2) return err = "driver gave fewer than 2 buffers", false;

  bufs_.resize(req.count);
  for (uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return err = errstr("VIDIOC_QUERYBUF"), false;
    bufs_[i].len = b.length;
    bufs_[i].ptr = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
    if (bufs_[i].ptr == MAP_FAILED) {
      bufs_[i].ptr = nullptr;
      return err = errstr("mmap buffer"), false;
    }
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return err = errstr("VIDIOC_QBUF"), false;
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return err = errstr("VIDIOC_STREAMON"), false;
  streaming_ = true;
  return true;
}

bool V4L2Camera::dequeue(const uint8_t** data, size_t* bytes, int64_t* ts_ns, bool* ts_monotonic,
                         uint32_t* driver_seq, std::string& err) {
  pollfd pfd{fd_, POLLIN, 0};
  int pr;
  do {
    pr = poll(&pfd, 1, 2000);
  } while (pr < 0 && errno == EINTR);
  if (pr <= 0) return err = (pr == 0 ? "camera timeout (2 s without a frame)" : errstr("poll")), false;

  v4l2_buffer b{};
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) return err = errstr("VIDIOC_DQBUF"), false;

  cur_index_ = b.index;
  have_cur_ = true;
  *data = static_cast<const uint8_t*>(bufs_[b.index].ptr);
  *bytes = b.bytesused;
  *ts_ns = int64_t(b.timestamp.tv_sec) * 1'000'000'000 + int64_t(b.timestamp.tv_usec) * 1000;
  *ts_monotonic = (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
  *driver_seq = b.sequence;
  return true;
}

bool V4L2Camera::requeue(std::string& err) {
  if (!have_cur_) return true;
  v4l2_buffer b{};
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = cur_index_;
  have_cur_ = false;
  if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return err = errstr("VIDIOC_QBUF"), false;
  return true;
}

void V4L2Camera::close() {
  if (fd_ < 0) return;
  if (streaming_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
  }
  for (auto& b : bufs_)
    if (b.ptr) munmap(b.ptr, b.len);
  bufs_.clear();
  ::close(fd_);
  fd_ = -1;
}

void yuyv_to_rgb(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
  const size_t pairs = size_t(width) * height / 2;
  for (size_t i = 0; i < pairs; ++i) {
    const int y0 = src[4 * i + 0] - 16, u = src[4 * i + 1] - 128;
    const int y1 = src[4 * i + 2] - 16, v = src[4 * i + 3] - 128;
    const int ruv = 409 * v + 128, guv = -100 * u - 208 * v + 128, buv = 516 * u + 128;
    const int c0 = 298 * y0, c1 = 298 * y1;
    uint8_t* d = dst + 6 * i;
    d[0] = clamp8((c0 + ruv) >> 8);
    d[1] = clamp8((c0 + guv) >> 8);
    d[2] = clamp8((c0 + buv) >> 8);
    d[3] = clamp8((c1 + ruv) >> 8);
    d[4] = clamp8((c1 + guv) >> 8);
    d[5] = clamp8((c1 + buv) >> 8);
  }
}

}  // namespace fp
