// ring_test.cpp — fp_ring_test: correctness + throughput check of SpscRing.
//
// Two threads, small slots, tens of millions of messages. The producer writes a
// sequence number and its bitwise complement into every slot; the consumer checks
// that it receives every number exactly once, in order, never torn. If the memory
// ordering in spsc_ring.hpp were wrong (e.g. relaxed instead of release/acquire),
// this is the kind of test that eventually catches it — run it a few times, and on a
// loaded machine.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "common/frame.hpp"
#include "common/spsc_ring.hpp"

using namespace fp;

int main(int argc, char** argv) {
  const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000ULL;
  constexpr uint32_t kCap = 1024;
  constexpr size_t kSlot = 64;

  static RingCtl ctl;  // RingCtl's members are already alignas(64)
  alignas(64) static uint8_t slots[kCap * kSlot];
  SpscRing prod(&ctl, slots, kCap, kSlot), cons(&ctl, slots, kCap, kSlot);

  uint64_t errors = 0;
  const int64_t t0 = now_ns();
  std::thread producer([&] {
    for (uint64_t i = 0; i < n;) {
      uint8_t* s = prod.try_claim();
      if (!s) continue;  // full: spin
      const uint64_t a = i, b = ~i;
      std::memcpy(s, &a, 8);
      std::memcpy(s + 8, &b, 8);
      prod.publish();
      ++i;
    }
  });
  for (uint64_t expect = 0; expect < n;) {
    uint8_t* s = cons.try_peek();
    if (!s) continue;  // empty: spin
    uint64_t a, b;
    std::memcpy(&a, s, 8);
    std::memcpy(&b, s + 8, 8);
    if (a != expect || b != ~expect) {
      if (errors < 10) std::fprintf(stderr, "mismatch: expected %" PRIu64 " got %" PRIu64 "/%" PRIx64 "\n", expect, a, b);
      ++errors;
    }
    cons.release();
    ++expect;
  }
  producer.join();
  const double secs = double(now_ns() - t0) / 1e9;
  std::printf("%s: %" PRIu64 " messages, %" PRIu64 " errors, %.1f M msg/s, %.1f ns/msg\n",
              errors ? "FAIL" : "PASS", n, errors, double(n) / secs / 1e6, secs * 1e9 / double(n));
  return errors ? 1 : 0;
}
