// policy_model.cpp — see policy_model.hpp.
#include "policy_model.hpp"

#include <sstream>
#include <stdexcept>

namespace fp {
namespace {

std::string shape_str(const std::vector<int64_t>& s) {
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < s.size(); ++i) o << (i ? "," : "") << s[i];
  o << "]";
  return o.str();
}

}  // namespace

PolicyModel::PolicyModel(const std::string& path, int intra_threads)
    : env_(ORT_LOGGING_LEVEL_WARNING, "framepipe"),
      mem_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
  opts_.SetIntraOpNumThreads(intra_threads);
  opts_.SetInterOpNumThreads(1);
  opts_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  opts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  opts_.AddConfigEntry("session.intra_op.allow_spinning", "0");
  opts_.AddConfigEntry("session.set_denormal_as_zero", "1");
  session_ = Ort::Session(env_, path.c_str(), opts_);

  Ort::AllocatorWithDefaultOptions alloc;

  // ---- input: expect one float NCHW tensor with C == 3
  if (session_.GetInputCount() != 1) throw std::runtime_error("model must have exactly one input");
  name_holders_.push_back(session_.GetInputNameAllocated(0, alloc));
  in_names_.push_back(name_holders_.back().get());
  {
    Ort::TypeInfo ti = session_.GetInputTypeInfo(0);
    auto tsi = ti.GetTensorTypeAndShapeInfo();
    if (tsi.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
      throw std::runtime_error("model input must be float32");
    in_shape_ = tsi.GetShape();
  }
  if (in_shape_.size() != 4) throw std::runtime_error("model input must be NCHW, got " + shape_str(in_shape_));
  if (in_shape_[0] <= 0) in_shape_[0] = 1;  // dynamic batch -> 1
  if (in_shape_[1] <= 0) in_shape_[1] = 3;
  if (in_shape_[2] <= 0) in_shape_[2] = 224;  // dynamic spatial dims -> 224
  if (in_shape_[3] <= 0) in_shape_[3] = 224;
  if (in_shape_[0] != 1 || in_shape_[1] != 3)
    throw std::runtime_error("model input must be [1,3,H,W], got " + shape_str(in_shape_));
  in_h_ = in_shape_[2];
  in_w_ = in_shape_[3];
  in_buf_.assign(size_t(3 * in_h_ * in_w_), 0.f);
  in_tensor_ = Ort::Value::CreateTensor<float>(mem_, in_buf_.data(), in_buf_.size(), in_shape_.data(),
                                               in_shape_.size());

  // ---- outputs: preallocate every one (float only)
  const size_t n_out = session_.GetOutputCount();
  out_shapes_.resize(n_out);
  out_bufs_.resize(n_out);
  for (size_t i = 0; i < n_out; ++i) {
    name_holders_.push_back(session_.GetOutputNameAllocated(i, alloc));
    out_names_.push_back(name_holders_.back().get());
    Ort::TypeInfo ti = session_.GetOutputTypeInfo(i);
    auto tsi = ti.GetTensorTypeAndShapeInfo();
    if (tsi.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
      throw std::runtime_error("model outputs must be float32");
    out_shapes_[i] = tsi.GetShape();
    size_t n = 1;
    for (auto& d : out_shapes_[i]) {
      if (d <= 0) d = 1;
      n *= size_t(d);
    }
    out_bufs_[i].assign(n, 0.f);
    out_tensors_.push_back(Ort::Value::CreateTensor<float>(mem_, out_bufs_[i].data(), n, out_shapes_[i].data(),
                                                           out_shapes_[i].size()));
  }

  // ImageNet normalisation folded into one multiply-add per channel:
  //   x = (p/255 - mean) / std  ==  p * (1/(255*std)) + (-mean/std)
  const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
  for (int c = 0; c < 3; ++c) {
    scale_[c] = 1.f / (255.f * stdv[c]);
    bias_[c] = -mean[c] / stdv[c];
  }
}

void PolicyModel::preprocess(const uint8_t* rgb, uint32_t width, uint32_t height) {
  if (width != map_src_w_ || height != map_src_h_) {  // built once, on the first frame
    xoff_.resize(size_t(in_w_));
    yrow_.resize(size_t(in_h_));
    for (int64_t x = 0; x < in_w_; ++x) xoff_[size_t(x)] = uint32_t((uint64_t(x) * width / in_w_) * 3);
    for (int64_t y = 0; y < in_h_; ++y) yrow_[size_t(y)] = uint32_t(uint64_t(y) * height / in_h_);
    map_src_w_ = width;
    map_src_h_ = height;
  }
  const size_t plane = size_t(in_h_ * in_w_);
  float* r = in_buf_.data();
  float* g = r + plane;
  float* b = g + plane;
  const size_t stride = size_t(width) * 3;
  for (int64_t y = 0; y < in_h_; ++y) {
    const uint8_t* row = rgb + size_t(yrow_[size_t(y)]) * stride;
    const size_t o = size_t(y * in_w_);
    for (int64_t x = 0; x < in_w_; ++x) {
      const uint8_t* p = row + xoff_[size_t(x)];
      r[o + x] = p[0] * scale_[0] + bias_[0];
      g[o + x] = p[1] * scale_[1] + bias_[1];
      b[o + x] = p[2] * scale_[2] + bias_[2];
    }
  }
}

void PolicyModel::run() {
  session_.Run(Ort::RunOptions{nullptr}, in_names_.data(), &in_tensor_, 1, out_names_.data(),
               out_tensors_.data(), out_tensors_.size());
}

std::string PolicyModel::describe() const {
  std::ostringstream o;
  o << "input " << in_names_[0] << " " << shape_str(in_shape_) << "; outputs";
  for (size_t i = 0; i < out_names_.size(); ++i) o << " " << out_names_[i] << " " << shape_str(out_shapes_[i]);
  return o.str();
}

}  // namespace fp
