// producer.cpp — fp_producer: the "camera". Emits frames at a fixed rate into
// shared-memory SPSC rings, one ring per stream, and logs every frame it was supposed
// to produce (including the ones it dropped) to producer.csv.
//
// Synthetic mode (default): two threads, one per stream. Both are clocked from the
// same t0, so frame k of each stream is *scheduled* at exactly the same instant:
//      deadline(k) = t0 + round(k * 1e9 / fps)
// Any difference between the two streams' capture timestamps for the same k is
// therefore pure scheduling/execution skew — which is what we want to measure.
//
// "Capture" for a synthetic frame = the moment the thread wakes up for its deadline.
// So (t_capture - t_deadline) is the wake-up latency of the producer thread: the
// scheduler-induced jitter a software-timed sensor loop suffers.
//
// Transport modes:
//   zerocopy: claim a ring slot, render pixels directly into shared memory, publish.
//   copy:     render into a private buffer, then memcpy 921,600 B into the slot
//             (the consumer also copies out — see consumer.cpp). This is the baseline.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common/args.hpp"
#include "common/frame.hpp"
#include "common/rt.hpp"
#include "common/shm.hpp"
#include "v4l2_source.hpp"

using namespace fp;

namespace {

enum ProdStatus : int32_t {
  kOk = 0,              // produced and published (or rendered, in --standalone)
  kRingFull = 1,        // produced but the ring was full -> dropped at the producer
  kMissedDeadline = 2,  // woke up after the *next* deadline had already passed -> never produced
};

struct ProdRecord {
  uint32_t stream;
  uint64_t seq;
  int64_t t_deadline;
  int64_t t_capture;
  int64_t t_enqueue;
  int32_t status;
};

struct Config {
  bool zerocopy = true;
  double duration_s = 30;
  double fps = 60;
  uint32_t n_streams = 2;
  uint32_t capacity = 8;
  int fifo_prio = 0;          // 0 = default scheduler (SCHED_OTHER / CFS / EEVDF)
  std::vector<int> cpus;      // one CPU per stream thread; empty = no pinning
  bool mlock = false;
  long timer_slack_ns = -1;   // -1 = leave default
  bool standalone = false;    // no shared memory, no consumer: measure the clock loop alone
  std::string source = "synthetic";
  std::string shm_name = "/framepipe";
  std::string out_dir = ".";
};

struct StreamCtx {
  uint32_t stream = 0;
  int cpu = -1;
  SpscRing ring;
  std::vector<uint8_t> scratch;  // private frame buffer (copy mode / standalone)
  std::vector<ProdRecord> log;
  std::string err;
};

// Cheap deterministic test pattern that changes every frame (so a consumer reading a
// stale or torn frame would see different pixels).
void render_synthetic(uint8_t* dst, uint32_t stream, uint64_t seq) {
  const uint8_t sx = uint8_t(seq * 4), sy = uint8_t(seq * 2), sb = uint8_t(stream * 97);
  for (uint32_t y = 0; y < kHeight; ++y) {
    uint8_t* row = dst + size_t(y) * kWidth * kChannels;
    const uint8_t g = uint8_t(y + sy);
    for (uint32_t x = 0; x < kWidth; ++x) {
      row[3 * x + 0] = uint8_t(x + sx);
      row[3 * x + 1] = g;
      row[3 * x + 2] = uint8_t((x ^ y) + sb);
    }
  }
}

void fill_header(FrameHeader* h, uint32_t stream, uint64_t seq, int64_t t_deadline,
                 int64_t t_capture, Source src) {
  h->stream = stream;
  h->width = kWidth;
  h->height = kHeight;
  h->channels = kChannels;
  h->seq = seq;
  h->t_deadline = t_deadline;
  h->t_capture = t_capture;
  h->bytes = uint32_t(kFrameBytes);
  h->source = uint32_t(src);
}

// Common per-thread real-time setup. Runs *inside* the thread because scheduling
// policy, affinity and timer slack are per-thread attributes.
void setup_thread(StreamCtx& sc, const Config& cfg) {
  char name[16];
  std::snprintf(name, sizeof(name), "fp_prod%u", sc.stream);
  set_thread_name(name);
  std::string err;
  if (sc.cpu >= 0 && !pin_to_cpu(sc.cpu, err)) sc.err = err;
  if (sc.err.empty() && cfg.fifo_prio > 0 && !set_fifo(cfg.fifo_prio, err)) sc.err = err;
  if (sc.err.empty() && cfg.timer_slack_ns >= 0 && !set_timer_slack(cfg.timer_slack_ns, err))
    sc.err = err;
  prefault_stack();
  // Touch the scratch buffer and the log so neither page-faults in the hot loop.
  std::fill(sc.scratch.begin(), sc.scratch.end(), uint8_t(0));
}

// Publish one frame. Returns status; fills t_enqueue on success.
int32_t emit(StreamCtx& sc, const Config& cfg, ShmControl* ctl, uint64_t seq, int64_t t_deadline,
             int64_t t_capture, Source src, const uint8_t* prerendered, int64_t* t_enqueue) {
  if (cfg.standalone) {
    if (!prerendered) render_synthetic(sc.scratch.data(), sc.stream, seq);
    *t_enqueue = now_ns();
    return kOk;
  }
  if (cfg.zerocopy) {
    uint8_t* slot = sc.ring.try_claim();
    if (!slot) return kRingFull;
    if (prerendered)
      std::memcpy(payload_of(slot), prerendered, kFrameBytes);  // not hit today: V4L2 zerocopy converts in place
    else
      render_synthetic(payload_of(slot), sc.stream, seq);  // write straight into shared memory
    FrameHeader* h = header_of(slot);
    fill_header(h, sc.stream, seq, t_deadline, t_capture, src);
    h->t_enqueue = *t_enqueue = now_ns();
    sc.ring.publish();
    ring_doorbell(ctl);
    return kOk;
  }
  // copy transport: render privately, then copy the whole frame into the ring.
  const uint8_t* pixels = prerendered;
  if (!pixels) {
    render_synthetic(sc.scratch.data(), sc.stream, seq);
    pixels = sc.scratch.data();
  }
  uint8_t* slot = sc.ring.try_claim();
  if (!slot) return kRingFull;
  std::memcpy(payload_of(slot), pixels, kFrameBytes);
  FrameHeader* h = header_of(slot);
  fill_header(h, sc.stream, seq, t_deadline, t_capture, src);
  h->t_enqueue = *t_enqueue = now_ns();
  sc.ring.publish();
  ring_doorbell(ctl);
  return kOk;
}

struct StartGate {
  std::atomic<int> n_ready{0};
  std::atomic<int64_t> t0{0};
  std::atomic<bool> abort{false};
  // Returns t0, or 0 if aborted.
  int64_t arrive_and_wait() {
    n_ready.fetch_add(1);
    int64_t t;
    while ((t = t0.load(std::memory_order_acquire)) == 0) {
      if (abort.load()) return 0;
      sleep_until_ns(now_ns() + 1'000'000);
    }
    return t;
  }
};

void synthetic_thread(StreamCtx& sc, const Config& cfg, ShmControl* ctl, StartGate& gate,
                      uint64_t n_frames) {
  setup_thread(sc, cfg);
  const int64_t t0 = gate.arrive_and_wait();
  if (t0 == 0 || !sc.err.empty()) return;

  const double period_ns = 1e9 / cfg.fps;
  auto deadline_of = [&](uint64_t k) { return t0 + int64_t(std::llround(double(k) * period_ns)); };

  uint64_t k = 0;
  while (k < n_frames) {
    sleep_until_ns(deadline_of(k));
    const int64_t t_cap = now_ns();
    // Woke up so late that later deadlines have also passed: those frames were never
    // captured. A real camera would have produced them; we record them as missed.
    while (k + 1 < n_frames && t_cap >= deadline_of(k + 1)) {
      sc.log.push_back({sc.stream, k, deadline_of(k), t_cap, 0, kMissedDeadline});
      ++k;
    }
    const int64_t dl = deadline_of(k);
    int64_t t_enq = 0;
    const int32_t st = emit(sc, cfg, ctl, k, dl, t_cap, Source::Synthetic, nullptr, &t_enq);
    sc.log.push_back({sc.stream, k, dl, t_cap, t_enq, st});
    ++k;
  }
}

// V4L2 mode: the camera, not our timer, sets the cadence. capture = driver timestamp.
// YUYV->RGB conversion is unavoidable work. In zerocopy mode we convert straight into
// the ring slot; in copy mode we convert into scratch and then copy into the slot.
// (True end-to-end zero-copy would export the driver buffer itself via DMABUF — see
// README "Limitations".)
void v4l2_thread(StreamCtx& sc, const Config& cfg, ShmControl* ctl, StartGate& gate,
                 V4L2Camera& cam) {
  setup_thread(sc, cfg);
  const int64_t t0 = gate.arrive_and_wait();
  if (t0 == 0 || !sc.err.empty()) return;
  const int64_t t_end = t0 + int64_t(cfg.duration_s * 1e9);

  bool have_first = false;
  uint32_t first_seq = 0, last_seq = 0;
  std::string err;
  while (now_ns() < t_end) {
    const uint8_t* yuyv = nullptr;
    size_t bytes = 0;
    int64_t ts = 0;
    bool mono = false;
    uint32_t dseq = 0;
    if (!cam.dequeue(&yuyv, &bytes, &ts, &mono, &dseq, err)) {
      sc.err = err;
      return;
    }
    const int64_t t_dq = now_ns();
    const int64_t t_cap = mono ? ts : t_dq;  // fall back to dequeue time if not MONOTONIC
    if (!have_first) {
      have_first = true;
      first_seq = last_seq = dseq;
    } else {
      // driver sequence gap = frames the camera produced that we never received
      for (uint32_t s = last_seq + 1; s < dseq && sc.log.size() < sc.log.capacity(); ++s)
        sc.log.push_back({sc.stream, uint64_t(s - first_seq), -1, 0, 0, kMissedDeadline});
      last_seq = dseq;
    }
    const uint64_t seq = uint64_t(dseq - first_seq);
    int64_t t_enq = 0;
    int32_t st;
    if (!cfg.standalone && cfg.zerocopy) {
      uint8_t* slot = sc.ring.try_claim();
      if (!slot) {
        st = kRingFull;
      } else {
        yuyv_to_rgb(yuyv, payload_of(slot), kWidth, kHeight);  // convert straight into shm
        FrameHeader* h = header_of(slot);
        fill_header(h, sc.stream, seq, -1, t_cap, Source::V4L2);
        h->t_enqueue = t_enq = now_ns();
        sc.ring.publish();
        ring_doorbell(ctl);
        st = kOk;
      }
    } else {
      yuyv_to_rgb(yuyv, sc.scratch.data(), kWidth, kHeight);
      st = emit(sc, cfg, ctl, seq, -1, t_cap, Source::V4L2, sc.scratch.data(), &t_enq);
    }
    if (!cam.requeue(err)) {
      sc.err = err;
      return;
    }
    if (sc.log.size() < sc.log.capacity()) sc.log.push_back({sc.stream, seq, -1, t_cap, t_enq, st});
  }
}

void write_csv(const std::string& path, const std::vector<StreamCtx>& streams) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) ArgParser::die("cannot write " + path);
  std::fprintf(f, "stream,seq,t_deadline,t_capture,t_enqueue,status\n");
  for (const auto& sc : streams)
    for (const auto& r : sc.log)
      std::fprintf(f, "%u,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d\n", r.stream, r.seq,
                   r.t_deadline, r.t_capture, r.t_enqueue, r.status);
  std::fclose(f);
}

void usage() {
  std::puts(
      "fp_producer — 60 Hz frame source into shared memory\n"
      "  --transport copy|zerocopy   (default zerocopy)\n"
      "  --duration SEC              (default 30)\n"
      "  --fps HZ                    (default 60)\n"
      "  --streams 1|2               (default 2; V4L2 forces 1)\n"
      "  --capacity N                ring slots per stream, power of two (default 8)\n"
      "  --fifo PRIO                 SCHED_FIFO priority 1..99 (default 0 = off)\n"
      "  --cpus A[,B]                pin stream thread i to CPU list[i]\n"
      "  --mlock                     mlockall(MCL_CURRENT|MCL_FUTURE)\n"
      "  --timer-slack NS            prctl(PR_SET_TIMERSLACK) (optional extra experiment)\n"
      "  --source synthetic|v4l2:/dev/videoN\n"
      "  --standalone                no shm/consumer: measure the timing loop alone\n"
      "  --shm NAME                  (default /framepipe)\n"
      "  --out DIR                   where producer.csv goes (default .)");
}

}  // namespace

int main(int argc, char** argv) {
  ArgParser ap(argc, argv,
               {"transport", "duration", "fps", "streams", "capacity", "fifo", "cpus",
                "timer-slack", "source", "shm", "out"},
               {"mlock", "standalone"});
  if (ap.help()) return usage(), 0;

  Config cfg;
  const std::string tr = ap.str("transport", "zerocopy");
  if (tr != "copy" && tr != "zerocopy") ArgParser::die("--transport must be copy or zerocopy");
  cfg.zerocopy = tr == "zerocopy";
  cfg.duration_s = ap.real("duration", 30);
  cfg.fps = ap.real("fps", 60);
  cfg.n_streams = uint32_t(ap.num("streams", 2));
  cfg.capacity = uint32_t(ap.num("capacity", 8));
  cfg.fifo_prio = int(ap.num("fifo", 0));
  cfg.cpus = ap.int_list("cpus");
  cfg.mlock = ap.flag("mlock");
  cfg.timer_slack_ns = ap.num("timer-slack", -1);
  cfg.standalone = ap.flag("standalone");
  cfg.source = ap.str("source", "synthetic");
  cfg.shm_name = ap.str("shm", "/framepipe");
  cfg.out_dir = ap.str("out", ".");
  {
    std::error_code ec;
    std::filesystem::create_directories(cfg.out_dir, ec);
    if (ec) ArgParser::die("cannot create --out " + cfg.out_dir + ": " + ec.message());
  }

  const bool use_v4l2 = cfg.source.rfind("v4l2:", 0) == 0;
  if (!use_v4l2 && cfg.source != "synthetic") ArgParser::die("--source must be synthetic or v4l2:/dev/videoN");
  if (use_v4l2) cfg.n_streams = 1;
  if (cfg.n_streams < 1 || cfg.n_streams > kMaxStreams) ArgParser::die("--streams must be 1 or 2");
  if (cfg.capacity < 2 || (cfg.capacity & (cfg.capacity - 1))) ArgParser::die("--capacity must be a power of two >= 2");
  if (cfg.fifo_prio < 0 || cfg.fifo_prio > 99) ArgParser::die("--fifo must be 0..99");
  if (cfg.fps <= 0 || cfg.duration_s <= 0) ArgParser::die("--fps and --duration must be > 0");
  if (!cfg.cpus.empty() && cfg.cpus.size() != cfg.n_streams)
    ArgParser::die("--cpus needs exactly one CPU per stream");

  std::string err;
  if (cfg.mlock && !lock_all_memory(err)) ArgParser::die(err);

  // Optional real camera.
  V4L2Camera cam;
  if (use_v4l2) {
    if (!cam.open(cfg.source.substr(5), kWidth, kHeight, uint32_t(std::lround(cfg.fps)), err))
      ArgParser::die("V4L2: " + err);
    std::printf("[producer] V4L2 camera open, driver frame rate %.2f fps\n", cam.actual_fps());
  }

  // Shared memory + rings.
  ShmRegion shm;
  ShmControl* ctl = nullptr;
  std::vector<StreamCtx> streams(cfg.n_streams);
  const uint64_t n_frames = uint64_t(std::llround(cfg.duration_s * cfg.fps));
  for (uint32_t s = 0; s < cfg.n_streams; ++s) {
    streams[s].stream = s;
    streams[s].cpu = cfg.cpus.empty() ? -1 : cfg.cpus[s];
    streams[s].scratch.resize(kFrameBytes);
    streams[s].log.reserve(n_frames * (use_v4l2 ? 2 : 1) + 16);
  }
  if (!cfg.standalone) {
    const Layout L = compute_layout(cfg.n_streams, cfg.capacity);
    shm = ShmRegion::create(cfg.shm_name, L.total);
    ctl = new (shm.data()) ShmControl();
    ctl->magic = kMagic;
    ctl->version = kLayoutVersion;
    ctl->n_streams = cfg.n_streams;
    ctl->capacity = cfg.capacity;
    ctl->slot_size = L.slot_size;
    ctl->total_size = L.total;
    for (uint32_t s = 0; s < cfg.n_streams; ++s) {
      ctl->ring_offset[s] = L.ring_offset[s];
      new (shm.data() + L.ring_offset[s]) RingCtl();
      streams[s].ring = ring_view(shm.data(), ctl, s);
    }
    ctl->producer_ready.store(1, std::memory_order_release);
    std::printf("[producer] shm %s: %.1f MiB, %u stream(s) x %u slots x %zu B\n", cfg.shm_name.c_str(),
                double(L.total) / (1 << 20), cfg.n_streams, cfg.capacity, L.slot_size);
    std::fflush(stdout);

    // Wait for the consumer to finish loading/warming up its model.
    const int64_t give_up = now_ns() + 120 * kNsPerSec;
    while (!ctl->consumer_ready.load(std::memory_order_acquire)) {
      if (now_ns() > give_up) {
        shm_unlink(cfg.shm_name.c_str());
        ArgParser::die("consumer never attached (120 s)");
      }
      sleep_until_ns(now_ns() + 5'000'000);
    }
  }

  StartGate gate;
  std::vector<std::thread> threads;
  for (uint32_t s = 0; s < cfg.n_streams; ++s) {
    if (use_v4l2)
      threads.emplace_back(v4l2_thread, std::ref(streams[s]), std::cref(cfg), ctl, std::ref(gate), std::ref(cam));
    else
      threads.emplace_back(synthetic_thread, std::ref(streams[s]), std::cref(cfg), ctl, std::ref(gate), n_frames);
  }
  while (gate.n_ready.load() < int(cfg.n_streams)) sleep_until_ns(now_ns() + 1'000'000);

  bool setup_ok = true;
  for (auto& sc : streams)
    if (!sc.err.empty()) {
      std::fprintf(stderr, "error: stream %u setup: %s\n", sc.stream, sc.err.c_str());
      setup_ok = false;
    }
  if (!setup_ok) {
    gate.abort.store(true);
  } else {
    gate.t0.store(now_ns() + 100'000'000, std::memory_order_release);  // start in 100 ms
    std::printf("[producer] running %.1f s at %.2f Hz, transport=%s fifo=%d mlock=%d\n", cfg.duration_s,
                cfg.fps, cfg.zerocopy ? "zerocopy" : "copy", cfg.fifo_prio, int(cfg.mlock));
    std::fflush(stdout);
  }
  for (auto& t : threads) t.join();

  if (ctl) {
    ctl->producer_done.store(1, std::memory_order_release);
    ring_doorbell(ctl);
    shm_unlink(cfg.shm_name.c_str());  // the consumer keeps its mapping
  }
  if (!setup_ok) return 3;

  int rc = 0;
  for (auto& sc : streams) {
    if (!sc.err.empty()) {
      std::fprintf(stderr, "error: stream %u: %s\n", sc.stream, sc.err.c_str());
      rc = 4;
    }
    size_t ok = 0, full = 0, missed = 0;
    for (auto& r : sc.log) (r.status == kOk ? ok : r.status == kRingFull ? full : missed)++;
    std::printf("[producer] stream %u: ok=%zu ring_full=%zu missed_deadline=%zu\n", sc.stream, ok, full, missed);
  }
  write_csv(cfg.out_dir + "/producer.csv", streams);
  return rc;
}
