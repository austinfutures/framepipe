// consumer.cpp — fp_consumer: the "policy" process. Attaches to the producer's shared
// memory, takes frames off each stream's ring, runs the ONNX model on stream 0, and
// logs the per-frame stage timestamps to consumer.csv.
//
// Stage timestamps per frame (all CLOCK_MONOTONIC ns):
//   t_capture      producer: frame captured (from the header)
//   t_enqueue      producer: just before publish (from the header)
//   t_dequeue      consumer: slot observed on the ring
//   t_infer_start  consumer: preprocessing done, Run() about to start
//   t_infer_end    consumer: Run() returned
// End-to-end latency = t_infer_end - t_capture.
//
// Stream 1 is consumed (copied out in copy mode, released) but not run through the
// model: it exists to measure two-stream skew and to load the transport.
//
// Copy transport: memcpy the whole frame out of the slot into a private buffer and
// release the slot right away. Zero-copy: preprocess directly from the slot and only
// release it afterwards — no 900 KB copy, but the slot is held longer.
//
// Logging never touches the disk in the hot loop: records go into a vector reserved up
// front (no reallocation), and are written to CSV after the run.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "common/args.hpp"
#include "common/frame.hpp"
#include "common/rt.hpp"
#include "common/shm.hpp"
#include "policy_model.hpp"

using namespace fp;

namespace {

enum ConsStatus : int32_t { kProcessed = 0, kSkippedStale = 1 };

struct ConsRecord {
  uint32_t stream;
  uint64_t seq;
  int64_t t_deadline, t_capture, t_enqueue, t_dequeue, t_infer_start, t_infer_end;
  int32_t status;
  uint32_t occupancy;  // frames waiting in this ring when we dequeued (incl. this one)
};

void usage() {
  std::puts(
      "fp_consumer — shared-memory frame consumer + ONNX policy\n"
      "  --transport copy|zerocopy   must match the producer (default zerocopy)\n"
      "  --model PATH|none           ONNX model (default models/tiny_policy.onnx); none = transport only\n"
      "  --threads N                 ORT intra-op threads (default 1)\n"
      "  --fifo PRIO                 SCHED_FIFO priority (default 0 = off)\n"
      "  --cpu N                     pin to CPU N (default: no pinning)\n"
      "  --mlock                     mlockall(MCL_CURRENT|MCL_FUTURE)\n"
      "  --timer-slack NS            prctl(PR_SET_TIMERSLACK)\n"
      "  --latest-only               if frames queued up, skip to the newest (logged as skipped)\n"
      "  --max-frames N              log capacity (default 200000)\n"
      "  --warmup N                  warm-up inferences before signalling ready (default 50)\n"
      "  --shm NAME                  (default /framepipe)\n"
      "  --out DIR                   where consumer.csv goes (default .)");
}

void write_csv(const std::string& path, const std::vector<ConsRecord>& recs) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) ArgParser::die("cannot write " + path);
  std::fprintf(f, "stream,seq,t_deadline,t_capture,t_enqueue,t_dequeue,t_infer_start,t_infer_end,status,occupancy\n");
  for (const auto& r : recs)
    std::fprintf(f, "%u,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%u\n",
                 r.stream, r.seq, r.t_deadline, r.t_capture, r.t_enqueue, r.t_dequeue, r.t_infer_start,
                 r.t_infer_end, r.status, r.occupancy);
  std::fclose(f);
}

int run(int argc, char** argv) {
  ArgParser ap(argc, argv,
               {"transport", "model", "threads", "fifo", "cpu", "timer-slack", "max-frames", "warmup", "shm", "out"},
               {"mlock", "latest-only"});
  if (ap.help()) return usage(), 0;

  const std::string tr = ap.str("transport", "zerocopy");
  if (tr != "copy" && tr != "zerocopy") ArgParser::die("--transport must be copy or zerocopy");
  const bool zerocopy = tr == "zerocopy";
  const std::string model_path = ap.str("model", "models/tiny_policy.onnx");
  const int threads = int(ap.num("threads", 1));
  const int fifo = int(ap.num("fifo", 0));
  const int cpu = int(ap.num("cpu", -1));
  const long slack = ap.num("timer-slack", -1);
  const bool mlock = ap.flag("mlock");
  const bool latest_only = ap.flag("latest-only");
  const size_t max_frames = size_t(ap.num("max-frames", 200000));
  const int warmup = int(ap.num("warmup", 50));
  const std::string shm_name = ap.str("shm", "/framepipe");
  const std::string out_dir = ap.str("out", ".");
  {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) ArgParser::die("cannot create --out " + out_dir + ": " + ec.message());
  }
  constexpr uint32_t kInferStream = 0;

  std::string err;
  set_thread_name("fp_cons");
  // mlock first, so everything allocated afterwards (ORT arena, our buffers) is locked too.
  if (mlock && !lock_all_memory(err)) ArgParser::die(err);
  if (cpu >= 0 && !pin_to_cpu(cpu, err)) ArgParser::die(err);
  if (slack >= 0 && !set_timer_slack(slack, err)) ArgParser::die(err);

  // ---- model (loaded before attaching, so the producer only starts once we're warm)
  std::unique_ptr<PolicyModel> model;
  if (model_path != "none") {
    model = std::make_unique<PolicyModel>(model_path, threads);
    std::printf("[consumer] model %s: %s\n", model_path.c_str(), model->describe().c_str());
  }

  // ---- attach to shared memory
  ShmRegion shm;
  const int64_t give_up = now_ns() + 60 * kNsPerSec;
  for (;;) {
    shm = ShmRegion::try_open(shm_name);
    if (shm.valid()) {
      auto* c = reinterpret_cast<ShmControl*>(shm.data());
      if (c->producer_ready.load(std::memory_order_acquire)) break;
      shm = ShmRegion();  // mapped before the producer finished initialising; retry
    }
    if (now_ns() > give_up) ArgParser::die("no producer found at " + shm_name + " within 60 s");
    sleep_until_ns(now_ns() + 10'000'000);
  }
  auto* ctl = reinterpret_cast<ShmControl*>(shm.data());
  if (ctl->magic != kMagic || ctl->version != kLayoutVersion) ArgParser::die("shm layout mismatch (stale segment?)");
  if (ctl->slot_size < sizeof(FrameHeader) + kFrameBytes || ctl->total_size > shm.size())
    ArgParser::die("shm sizes inconsistent");
  const uint32_t n_streams = ctl->n_streams;
  std::vector<SpscRing> rings;
  for (uint32_t s = 0; s < n_streams; ++s) rings.push_back(ring_view(shm.data(), ctl, s));

  // ---- buffers, touched now so they never page-fault in the loop
  std::vector<uint8_t> local(kFrameBytes, 0);
  std::vector<ConsRecord> recs;
  recs.resize(max_frames);  // writes every element -> every page is faulted in now
  recs.clear();             // size 0, capacity (and the touched pages) kept
  size_t log_overflow = 0;
  prefault_stack();

  // ---- warm-up: first inferences allocate ORT's arena, JIT-select kernels, fill caches
  if (model) {
    std::vector<uint8_t> dummy(kFrameBytes, 128);
    int64_t worst = 0;
    for (int i = 0; i < warmup; ++i) {
      const int64_t t = now_ns();
      model->preprocess(dummy.data(), kWidth, kHeight);
      model->run();
      worst = std::max(worst, now_ns() - t);
    }
    std::printf("[consumer] warm-up %d runs, slowest %.3f ms\n", warmup, double(worst) / 1e6);
  }

  // RT priority last: loading/warming the model at FIFO priority could starve the
  // system for seconds on a small machine.
  if (fifo > 0 && !set_fifo(fifo, err)) ArgParser::die(err);

  ctl->consumer_ready.store(1, std::memory_order_release);
  std::printf("[consumer] ready: transport=%s fifo=%d cpu=%d mlock=%d latest_only=%d streams=%u\n",
              zerocopy ? "zerocopy" : "copy", fifo, cpu, int(mlock), int(latest_only), n_streams);
  std::fflush(stdout);

  volatile float sink = 0.f;
  volatile uint32_t touch = 0;

  auto log_rec = [&](const ConsRecord& r) {
    if (recs.size() < max_frames) recs.push_back(r);
    else ++log_overflow;
  };

  // Process one slot from ring s. The slot is released inside.
  auto process = [&](SpscRing& ring, uint8_t* slot, uint32_t s) {
    const int64_t t_deq = now_ns();
    const uint32_t occ = uint32_t(ring.occupancy());
    const FrameHeader h = *header_of(slot);  // 64-byte copy of the metadata
    const uint8_t* pixels = payload_of(slot);
    if (!zerocopy) {
      std::memcpy(local.data(), pixels, kFrameBytes);
      ring.release();  // slot back to the producer immediately
      pixels = local.data();
    }
    int64_t t_is = 0, t_ie = 0;
    if (s == kInferStream && model) {
      model->preprocess(pixels, h.width, h.height);
      if (zerocopy) ring.release();  // done reading shared memory
      t_is = now_ns();
      model->run();
      t_ie = now_ns();
      sink = model->first_output();
    } else {
      // No inference on this frame: still read the pixels (sparse) so zero-copy isn't
      // "free" just because nobody looked at the data.
      uint32_t acc = 0;
      for (size_t i = 0; i < kFrameBytes; i += 4096) acc += pixels[i];
      touch = acc;
      if (zerocopy) ring.release();
    }
    log_rec({s, h.seq, h.t_deadline, h.t_capture, h.t_enqueue, t_deq, t_is, t_ie, kProcessed, occ});
  };

  int64_t last_frame_at = now_ns();
  for (;;) {
    // Read the doorbell BEFORE checking the rings (see shm.hpp for why).
    const uint32_t bell = ctl->doorbell.load(std::memory_order_acquire);
    bool got = false;
    for (uint32_t s = 0; s < n_streams; ++s) {
      uint8_t* slot = rings[s].try_peek();
      if (!slot) continue;
      got = true;
      if (latest_only) {
        while (rings[s].occupancy() > 1) {  // drop stale frames, keep the newest
          const FrameHeader h = *header_of(slot);
          log_rec({s, h.seq, h.t_deadline, h.t_capture, h.t_enqueue, now_ns(), 0, 0, kSkippedStale, 0});
          rings[s].release();
          slot = rings[s].try_peek();
        }
      }
      process(rings[s], slot, s);
    }
    if (got) {
      last_frame_at = now_ns();
      continue;
    }
    if (ctl->producer_done.load(std::memory_order_acquire)) {
      bool empty = true;
      for (auto& r : rings) empty = empty && r.occupancy() == 0;
      if (empty) break;
      continue;
    }
    if (now_ns() - last_frame_at > 10 * kNsPerSec) ArgParser::die("no frames for 10 s — producer died?");
    futex_wait(&ctl->doorbell, bell, 50'000'000);  // sleeps only if no publish since `bell`
  }

  size_t processed = 0, skipped = 0;
  for (auto& r : recs) (r.status == kProcessed ? processed : skipped)++;
  std::printf("[consumer] processed=%zu skipped_stale=%zu log_overflow=%zu (sink %.3f, %u)\n", processed, skipped,
              log_overflow, double(sink), unsigned(touch));
  write_csv(out_dir + "/consumer.csv", recs);
  return log_overflow ? 5 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const Ort::Exception& e) {
    std::fprintf(stderr, "error: onnxruntime: %s\n", e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
  }
  return 1;
}
