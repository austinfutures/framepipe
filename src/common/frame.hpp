// frame.hpp — frame format, per-frame header, and clock helpers shared by every process.
//
// Design notes (interview-relevant):
//  * All timestamps are CLOCK_MONOTONIC nanoseconds. MONOTONIC is shared by every
//    process on the machine and never jumps (unlike CLOCK_REALTIME under NTP), so
//    a timestamp taken in the producer can be subtracted from one taken in the
//    consumer.
//  * FrameHeader is exactly 64 bytes (one cache line), so the pixel payload that
//    follows it in a ring slot starts cache-line aligned.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace fp {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 480;
constexpr uint32_t kChannels = 3;  // packed RGB, 8 bits per channel
constexpr size_t kFrameBytes = size_t(kWidth) * kHeight * kChannels;  // 921,600 B
constexpr int64_t kNsPerSec = 1'000'000'000;

enum class Source : uint32_t { Synthetic = 0, V4L2 = 1 };

struct alignas(64) FrameHeader {
  uint32_t stream;    // 0 or 1
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint64_t seq;       // frame index within the stream (k-th period)
  int64_t t_deadline; // when the frame was *scheduled* (synthetic); -1 for V4L2
  int64_t t_capture;  // when the frame was captured (synthetic: wake-up time; V4L2: driver timestamp)
  int64_t t_enqueue;  // just before the producer publishes the slot
  uint32_t bytes;     // payload size
  uint32_t source;    // fp::Source
};
static_assert(sizeof(FrameHeader) == 64, "FrameHeader must be one cache line");

inline int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

// Absolute-deadline sleep. Using TIMER_ABSTIME (not a relative sleep) means the
// error of one period never accumulates into the next: frame k is always
// scheduled at t0 + k*period, no matter how late frame k-1 woke up.
inline void sleep_until_ns(int64_t t) {
  timespec ts;
  ts.tv_sec = time_t(t / kNsPerSec);
  ts.tv_nsec = long(t % kNsPerSec);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

}  // namespace fp
