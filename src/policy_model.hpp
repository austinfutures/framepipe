// policy_model.hpp — thin ONNX Runtime wrapper with zero per-frame allocation.
//
// Real-time choices:
//  * intra-op threads = 1 (default). Inference runs on the *calling* thread, so the
//    consumer's SCHED_FIFO priority and CPU pin apply to the model too, and there is
//    no thread-pool hand-off (a hidden wake-up latency) inside every frame.
//  * Input and output tensors wrap buffers we allocate once; Run() writes into them.
//  * Thread-pool spinning off, denormals flushed to zero (denormal floats can make
//    some frames 10-100x slower — a classic, data-dependent jitter source).
//  * Preprocessing uses precomputed nearest-neighbour index tables: no divisions and
//    no allocation per frame.
#pragma once

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fp {

class PolicyModel {
 public:
  PolicyModel(const std::string& path, int intra_threads);

  // Resize (nearest) + normalise a packed-RGB frame into the model's NCHW float input.
  void preprocess(const uint8_t* rgb, uint32_t width, uint32_t height);
  // One forward pass into the preallocated outputs.
  void run();
  // First output's first value — read after run() so the work can't be optimised away.
  float first_output() const { return out_bufs_.empty() || out_bufs_[0].empty() ? 0.f : out_bufs_[0][0]; }

  std::string describe() const;

 private:
  Ort::Env env_;
  Ort::SessionOptions opts_;
  Ort::Session session_{nullptr};
  Ort::MemoryInfo mem_;

  std::vector<Ort::AllocatedStringPtr> name_holders_;
  std::vector<const char*> in_names_, out_names_;

  std::vector<int64_t> in_shape_;
  std::vector<float> in_buf_;
  Ort::Value in_tensor_{nullptr};

  std::vector<std::vector<int64_t>> out_shapes_;
  std::vector<std::vector<float>> out_bufs_;
  std::vector<Ort::Value> out_tensors_;

  int64_t in_h_ = 0, in_w_ = 0;
  uint32_t map_src_w_ = 0, map_src_h_ = 0;
  std::vector<uint32_t> xoff_;  // byte offset of source pixel for each dst x
  std::vector<uint32_t> yrow_;  // source row for each dst y
  float scale_[3], bias_[3];
};

}  // namespace fp
