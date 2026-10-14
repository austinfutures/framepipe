// spsc_ring.hpp — lock-free single-producer / single-consumer ring of fixed-size slots.
//
// The ring itself does not own memory: it is a *view* over a RingCtl (two indices)
// and a slot array, both of which live in POSIX shared memory so two processes can
// use the same ring.
//
// Protocol
//   producer:  slot = try_claim();  ...write header+pixels into slot...;  publish();
//   consumer:  slot = try_peek();   ...read slot...;                       release();
//
// Why it is correct without locks
//   * Exactly one thread ever writes `head` (producer) and one ever writes `tail`
//     (consumer), so neither index needs a read-modify-write (no CAS, no fetch_add).
//   * publish() is a store-RELEASE of head. The consumer's load-ACQUIRE of head that
//     observes the new value is guaranteed to also observe every byte the producer
//     wrote into the slot before publish(). That pairing is the whole "lock-free" part.
//   * release() is a store-RELEASE of tail, paired with the producer's ACQUIRE load of
//     tail in try_claim(): the producer can't overwrite a slot until the consumer's
//     reads of it are complete.
//   * head and tail are free-running 64-bit counters (never wrapped); the slot index is
//     counter & (capacity-1). Occupancy is simply head - tail. At 120 frames/s a 64-bit
//     counter overflows in ~4.8 billion years.
//
// Performance details
//   * head and tail sit on separate 64-byte cache lines (alignas(64)) so the
//     producer's writes to head don't invalidate the line holding tail (false sharing).
//   * Each side caches the *other* side's index (tail_cache_ / head_cache_) and only
//     re-reads the shared one when the cached value says "full"/"empty". In the common
//     case that saves a cross-core cache-line transfer per operation.
//
// Zero-copy: try_claim()/try_peek() hand out a pointer *into* the ring. The producer
// renders pixels straight into shared memory and the consumer reads them in place.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fp {

struct RingCtl {
  alignas(64) std::atomic<uint64_t> head{0};  // next slot the producer will publish
  alignas(64) std::atomic<uint64_t> tail{0};  // next slot the consumer will read
};

// std::atomic in shared memory is only valid if it is lock-free (a lock-based
// atomic would use a process-local lock). On x86-64/aarch64 these are lock-free.
static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");

class SpscRing {
 public:
  SpscRing() = default;
  // capacity must be a power of two.
  SpscRing(RingCtl* ctl, uint8_t* slots, uint32_t capacity, size_t slot_size)
      : ctl_(ctl), slots_(slots), capacity_(capacity), slot_size_(slot_size) {}

  // ---------------- producer side ----------------
  // Returns a writable slot, or nullptr if the ring is full.
  uint8_t* try_claim() noexcept {
    const uint64_t h = ctl_->head.load(std::memory_order_relaxed);  // we are the only writer
    if (h - tail_cache_ >= capacity_) {
      tail_cache_ = ctl_->tail.load(std::memory_order_acquire);
      if (h - tail_cache_ >= capacity_) return nullptr;
    }
    return slot_at(h);
  }
  // Makes the slot returned by the last try_claim() visible to the consumer.
  void publish() noexcept {
    const uint64_t h = ctl_->head.load(std::memory_order_relaxed);
    ctl_->head.store(h + 1, std::memory_order_release);
  }

  // ---------------- consumer side ----------------
  // Returns the oldest unread slot, or nullptr if the ring is empty.
  uint8_t* try_peek() noexcept {
    const uint64_t t = ctl_->tail.load(std::memory_order_relaxed);  // we are the only writer
    if (t == head_cache_) {
      head_cache_ = ctl_->head.load(std::memory_order_acquire);
      if (t == head_cache_) return nullptr;
    }
    return slot_at(t);
  }
  // Hands the slot returned by the last try_peek() back to the producer.
  void release() noexcept {
    const uint64_t t = ctl_->tail.load(std::memory_order_relaxed);
    ctl_->tail.store(t + 1, std::memory_order_release);
  }

  // Number of published-but-unreleased slots (exact when called by either endpoint
  // between operations; approximate from anywhere else).
  uint64_t occupancy() const noexcept {
    return ctl_->head.load(std::memory_order_acquire) - ctl_->tail.load(std::memory_order_acquire);
  }
  uint32_t capacity() const noexcept { return capacity_; }

 private:
  uint8_t* slot_at(uint64_t i) const noexcept {
    return slots_ + size_t(i & (capacity_ - 1)) * slot_size_;
  }

  RingCtl* ctl_ = nullptr;
  uint8_t* slots_ = nullptr;
  uint32_t capacity_ = 0;
  size_t slot_size_ = 0;
  uint64_t tail_cache_ = 0;  // producer's last view of tail
  uint64_t head_cache_ = 0;  // consumer's last view of head
};

}  // namespace fp
