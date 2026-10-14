// shm.hpp — POSIX shared-memory segment layout, RAII mapping, and the futex doorbell.
//
// Segment layout (one segment holds every stream's ring):
//
//   offset 0      ShmControl   (magic, sizes, ready/done flags, doorbell)   4 KiB page
//   ring_offset[0] RingCtl      (head, tail)                                 4 KiB page
//                  slot 0 .. slot capacity-1   (FrameHeader + 640x480x3 pixels, 4 KiB rounded)
//   ring_offset[1] RingCtl ...  (second stream)
//
// Everything is page-aligned so MAP_POPULATE/mlockall touch whole pages and no
// structure straddles a page used for something else.
//
// Wake-ups: the data path is lock-free, but a consumer with nothing to do should
// sleep, not spin. The producer increments `doorbell` after every publish and calls
// FUTEX_WAKE; the consumer reads doorbell *before* checking the rings and passes that
// value to FUTEX_WAIT, which only sleeps if doorbell is still unchanged. That makes
// "check rings, then sleep" race-free (no lost wake-ups). We use the shared (not
// FUTEX_PRIVATE_FLAG) futex ops because the word is shared between processes.
#pragma once

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "common/frame.hpp"
#include "common/spsc_ring.hpp"

namespace fp {

constexpr uint64_t kMagic = 0x4650495045303031ULL;  // "FPIPE001"
constexpr uint32_t kLayoutVersion = 1;
constexpr uint32_t kMaxStreams = 2;
constexpr size_t kPage = 4096;

inline constexpr size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

struct ShmControl {
  uint64_t magic;
  uint32_t version;
  uint32_t n_streams;
  uint32_t capacity;
  uint32_t reserved;
  uint64_t slot_size;
  uint64_t ring_offset[kMaxStreams];
  uint64_t total_size;
  alignas(64) std::atomic<uint32_t> producer_ready{0};
  std::atomic<uint32_t> consumer_ready{0};
  std::atomic<uint32_t> producer_done{0};
  alignas(64) std::atomic<uint32_t> doorbell{0};  // futex word, on its own cache line
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "futex needs a plain 32-bit word");

struct Layout {
  size_t slot_size = 0;
  size_t ring_offset[kMaxStreams] = {0, 0};
  size_t total = 0;
};

inline Layout compute_layout(uint32_t n_streams, uint32_t capacity) {
  Layout L;
  L.slot_size = round_up(sizeof(FrameHeader) + kFrameBytes, kPage);
  size_t off = round_up(sizeof(ShmControl), kPage);
  for (uint32_t s = 0; s < n_streams; ++s) {
    L.ring_offset[s] = off;
    off += round_up(sizeof(RingCtl), kPage) + size_t(capacity) * L.slot_size;
  }
  L.total = off;
  return L;
}

// Build the ring view for stream s on top of a mapped segment.
inline SpscRing ring_view(uint8_t* base, const ShmControl* c, uint32_t s) {
  uint8_t* ring_base = base + c->ring_offset[s];
  auto* ctl = reinterpret_cast<RingCtl*>(ring_base);
  uint8_t* slots = ring_base + round_up(sizeof(RingCtl), kPage);
  return SpscRing(ctl, slots, c->capacity, c->slot_size);
}

inline FrameHeader* header_of(uint8_t* slot) { return reinterpret_cast<FrameHeader*>(slot); }
inline uint8_t* payload_of(uint8_t* slot) { return slot + sizeof(FrameHeader); }

// ---------------------------------------------------------------- RAII mapping
class ShmRegion {
 public:
  ShmRegion() = default;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;
  ShmRegion(ShmRegion&& o) noexcept { *this = std::move(o); }
  ShmRegion& operator=(ShmRegion&& o) noexcept {
    if (this != &o) {
      unmap();
      base_ = o.base_;
      size_ = o.size_;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~ShmRegion() { unmap(); }

  // Producer: remove any stale segment, create a fresh zero-filled one, map it.
  static ShmRegion create(const std::string& name, size_t size) {
    shm_unlink(name.c_str());  // ignore ENOENT
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) throw std::runtime_error("shm_open(create " + name + "): " + strerror(errno));
    if (ftruncate(fd, off_t(size)) != 0) {
      int e = errno;
      close(fd);
      throw std::runtime_error("ftruncate: " + std::string(strerror(e)));
    }
    ShmRegion r = map_fd(fd, size);
    close(fd);  // the mapping keeps the object alive
    return r;
  }

  // Consumer: returns an invalid region if the segment does not exist (yet) or has
  // not been sized yet; the caller retries.
  static ShmRegion try_open(const std::string& name) {
    int fd = shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) {
      if (errno == ENOENT) return ShmRegion();
      throw std::runtime_error("shm_open(" + name + "): " + strerror(errno));
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
      close(fd);
      return ShmRegion();
    }
    ShmRegion r = map_fd(fd, size_t(st.st_size));
    close(fd);
    return r;
  }

  bool valid() const { return base_ != nullptr; }
  uint8_t* data() const { return base_; }
  size_t size() const { return size_; }

 private:
  static ShmRegion map_fd(int fd, size_t size) {
    // MAP_POPULATE pre-faults every page now, so the first frames don't pay for
    // page faults inside the timed path.
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap: " + std::string(strerror(errno)));
    ShmRegion r;
    r.base_ = static_cast<uint8_t*>(p);
    r.size_ = size;
    return r;
  }
  void unmap() {
    if (base_) munmap(base_, size_);
    base_ = nullptr;
    size_ = 0;
  }
  uint8_t* base_ = nullptr;
  size_t size_ = 0;
};

// ---------------------------------------------------------------- futex doorbell
inline uint32_t* futex_word(std::atomic<uint32_t>* a) { return reinterpret_cast<uint32_t*>(a); }

// Sleep while *a == expected, at most timeout_ns. Returns immediately (EAGAIN) if the
// value already changed — that is what makes check-then-sleep race-free.
inline void futex_wait(std::atomic<uint32_t>* a, uint32_t expected, int64_t timeout_ns) {
  timespec ts{time_t(timeout_ns / kNsPerSec), long(timeout_ns % kNsPerSec)};
  syscall(SYS_futex, futex_word(a), FUTEX_WAIT, expected, &ts, nullptr, 0);
}

inline void futex_wake_all(std::atomic<uint32_t>* a) {
  syscall(SYS_futex, futex_word(a), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

inline void ring_doorbell(ShmControl* c) {
  c->doorbell.fetch_add(1, std::memory_order_release);
  futex_wake_all(&c->doorbell);
}

}  // namespace fp
