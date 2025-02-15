#pragma once

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// WARNING: Be careful when adding new includes here. This header will be used
// in model.so, and should not refer to any aten/c10 headers except the stable
// C ABI defined in torch/csrc/inductor/aoti_torch/c/shim.h. The same rule
// applies to other files under torch/csrc/inductor/aoti_runtime/.
#include <torch/csrc/inductor/aoti_runtime/device_utils.h>
#include <torch/csrc/inductor/aoti_torch/c/shim.h>

#define AOTI_RUNTIME_CHECK(EXPR, MSG) \
  do {                                \
    bool ok = EXPR;                   \
    if (!ok) {                        \
      throw std::runtime_error(MSG);  \
    }                                 \
  } while (0)

#define AOTI_TORCH_ERROR_CODE_CHECK(call)                                  \
  if ((call) != AOTI_TORCH_SUCCESS) {                                      \
    throw std::runtime_error(                                              \
        std::string(#call " API call failed at ") + __FILE__ + ", line " + \
        std::to_string(__LINE__));                                         \
  }

using DeleterFnPtr = void (*)(void*);

namespace torch {
namespace aot_inductor {

inline void delete_tensor_object(void* ptr) {
  AOTI_TORCH_ERROR_CODE_CHECK(
      aoti_torch_delete_tensor_object(reinterpret_cast<AtenTensorHandle>(ptr)));
}

// RAIIAtenTensorHandle steals the tensor objects created by the libtorch C ABI
class RAIIAtenTensorHandle {
 public:
  RAIIAtenTensorHandle() = delete;
  RAIIAtenTensorHandle(const RAIIAtenTensorHandle& other) = delete;
  RAIIAtenTensorHandle& operator=(const RAIIAtenTensorHandle& other) = delete;

  // Steal the ownership from another RAIIAtenTensorHandle using std::move
  RAIIAtenTensorHandle(RAIIAtenTensorHandle&& other) = default;
  RAIIAtenTensorHandle& operator=(RAIIAtenTensorHandle&& other) = default;

  // Steal the ownership from raw AtenTensorHandle
  RAIIAtenTensorHandle(AtenTensorHandle handle)
      : handle_(handle, delete_tensor_object) {}

  ~RAIIAtenTensorHandle() {
    handle_.reset();
  }

  // Return a raw AtenTensorHandle to be used by aoti_torch functions
  // Note: this function does NOT transfer the ownership of the handle
  operator AtenTensorHandle() const {
    return handle_.get();
  }

  AtenTensorHandle release() {
    return handle_.release();
  }

  AtenTensorHandle get() {
    return handle_.get();
  }

  void reset() {
    handle_.reset();
  }

 private:
  std::unique_ptr<AtenTensorOpaque, DeleterFnPtr> handle_;
};

using ConstantMap = std::unordered_map<std::string, RAIIAtenTensorHandle>;

// Steal the ownership from raw AtenTensorHandle to RAIIAtenTensorHandle
inline std::vector<RAIIAtenTensorHandle> steal_from_raw_handles_to_raii_handles(
    AtenTensorHandle* handles,
    size_t size) {
  std::vector<RAIIAtenTensorHandle> result;
  result.reserve(size);
  for (size_t i = 0; i < size; i++) {
    result.emplace_back(handles[i]);
    handles[i] = nullptr;
  }
  return result;
}

// Defines the base class for AOTInductorModel, which is generated by the
// AOTInductor cpp codegen. Since we do not need dynamic dispatch, we rely
// on curiously recurring template pattern (CRTP) to save some runtime
// v-table overhead. The generated AOTInductorModel is specialized with
// methods such as run_impl and members like shape params used for dynamic
// shape cases.
template <typename Model>
class AOTInductorModelBase {
 public:
  AOTInductorModelBase(
      size_t num_inputs,
      size_t num_outputs,
      size_t num_constants,
      std::optional<std::string> cubin_dir)
      : inputs_info_(num_inputs),
        outputs_info_(num_outputs),
        constants_info_(num_constants),
        cubin_dir_(cubin_dir),
        device_idx_(-1) {
#ifdef USE_CUDA
    AOTI_RUNTIME_DEVICE_CHECK(cudaGetDevice(&device_idx_));
#endif // USE_CUDA
  }

  ~AOTInductorModelBase() {
#ifdef USE_CUDA
    if (run_finished_) {
      auto code = cudaEventDestroy(*run_finished_);
      if (code != cudaSuccess) {
        std::cerr << "Failed to destroy CUDA event in AOTInductor model: "
                  << cudaGetErrorString(code) << std::endl;
      }
    }
#endif // USE_CUDA
  }

  AOTInductorModelBase(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase& operator=(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase(const AOTInductorModelBase&) = delete;
  AOTInductorModelBase& operator=(const AOTInductorModelBase&) = delete;

  void run(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor) {
#ifdef USE_CUDA
    if (!run_finished_) {
      cudaEvent_t run_finished;
      AOTI_RUNTIME_DEVICE_CHECK(cudaEventCreate(&run_finished));
      run_finished_.emplace(run_finished);
    }

    auto* model = static_cast<Model*>(this);
    model->run_impl(input_handles, output_handles, stream, proxy_executor);
    AOTI_RUNTIME_DEVICE_CHECK(cudaEventRecord(*run_finished_, stream));
#else // !USE_CUDA
    run_finished_ = false;
    auto* model = static_cast<Model*>(this);
    model->run_impl(input_handles, output_handles, stream, proxy_executor);
    run_finished_ = true;
#endif // USE_CUDA
  }

  size_t num_inputs() const {
    return inputs_info_.size();
  }

  size_t num_outputs() const {
    return outputs_info_.size();
  }

  size_t num_constants() const {
    return constants_info_.size();
  }

  const char* input_name(int64_t idx) const {
    return inputs_info_.at(idx).name;
  }

  const char* output_name(int64_t idx) const {
    return outputs_info_.at(idx).name;
  }

  const char* get_input_dtype(int64_t idx) const {
    return inputs_info_.at(idx).dtype;
  }

  const char* get_output_dtype(int64_t idx) const {
    return outputs_info_.at(idx).dtype;
  }

  const char* constant_name(int64_t idx) const {
    return constants_info_.at(idx).name;
  }

  std::vector<int64_t> max_input_shape(int64_t idx) const {
    return max_shape(inputs_info_, idx);
  }

  std::vector<int64_t> max_output_shape(int64_t idx) const {
    return max_shape(outputs_info_, idx);
  }

  size_t constant_ndim(int64_t idx) {
    return constants_info_.at(idx).shape.size();
  }

  const int64_t* constant_shape(int64_t idx) const {
    return constants_info_.at(idx).shape.data();
  }

  const int64_t* constant_stride(int64_t idx) const {
    return constants_info_.at(idx).stride.data();
  }

  int32_t constant_type(int64_t idx) const {
    return constants_info_.at(idx).dtype;
  }

  size_t constant_offset(int64_t idx) const {
    return constants_info_.at(idx).offset;
  }

  size_t constant_data_size(int64_t idx) const {
    return constants_info_.at(idx).data_size;
  }

  std::vector<int64_t> input_shape(int64_t idx) const {
    return shape(inputs_info_, idx);
  }

  std::vector<int64_t> output_shape(int64_t idx) const {
    return shape(outputs_info_, idx);
  }

  void update_constants_map(std::shared_ptr<ConstantMap>&& constants_map) {
    constants_ = std::move(constants_map);
  }

  /// Returns true if the model is complete.
  bool is_finished() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model CUDA event was not initialized"};
    }

    auto event_status = cudaEventQuery(*run_finished_);
    if (event_status == cudaSuccess) {
      return true;
    } else if (event_status == cudaErrorNotReady) {
      return false;
    }

    throw std::runtime_error(
        std::string("The model did not finish successfully. Error: ") +
        cudaGetErrorString(cudaGetLastError()));
#else // !USE_CUDA
    return run_finished_;
#endif // USE_CUDA
  }

  /// Synchronizes completion event.
  void wait_for_completion() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model event was not initialized"};
    }

    AOTI_RUNTIME_DEVICE_CHECK(cudaEventSynchronize(*run_finished_));
#endif // USE_CUDA
  }

 protected:
  class DimInfo {
   public:
    virtual int64_t value() const = 0;
    virtual void set_value(int64_t val) = 0;
    virtual int64_t lower_bound() const = 0;
    virtual int64_t upper_bound() const = 0;
    virtual ~DimInfo() {}
  };

  class StaticDimInfo : public DimInfo {
   public:
    StaticDimInfo(int64_t val) : value_(val) {}

    int64_t value() const {
      return value_;
    }

    void set_value(int64_t val) {
      throw std::runtime_error("cannot change the value of a StaticDim");
    }

    int64_t lower_bound() const {
      return value_;
    }

    int64_t upper_bound() const {
      return value_;
    }

   private:
    const int64_t value_;
  };

  class DynamicDimInfo : public DimInfo {
   public:
    DynamicDimInfo(const char* name, int64_t lb, int64_t ub)
        : name_(name), lower_bound_(lb), upper_bound_(ub), value_(-1) {}

    void set_value(int64_t val) {
      if (val != 1 && (val < lower_bound_ || val > upper_bound_)) {
        throw std::runtime_error(
            std::string(
                "dim value out of bounds: expected value to be between (") +
            std::to_string(lower_bound_) + ", " + std::to_string(upper_bound_) +
            "), but got " + std::to_string(val));
      }
      value_ = val;
    }

    int64_t value() const {
      return value_;
    }

    int64_t lower_bound() const {
      return lower_bound_;
    }

    int64_t upper_bound() const {
      return upper_bound_;
    }

   private:
    const std::string name_;
    const int64_t lower_bound_;
    const int64_t upper_bound_;
    int64_t value_;
  };

  DynamicDimInfo* find_dynamic_dim(const char* name) {
    auto iter = dynamic_dims_.find(name);
    if (iter == dynamic_dims_.end()) {
      throw std::runtime_error(
          std::string("dynamic_dim `") + name + "` does not exist");
    }
    return iter->second.get();
  }

  DynamicDimInfo* make_dynamic_dim(const char* name, int64_t lb, int64_t ub) {
    if (dynamic_dims_.find(name) != dynamic_dims_.end()) {
      throw std::runtime_error(
          std::string("dynamic_dim `") + name + "` already exists");
    }
    auto iter = dynamic_dims_.emplace(
        name, std::make_unique<DynamicDimInfo>(name, lb, ub));
    return (iter.first->second).get();
  }

  StaticDimInfo* make_static_dim(int64_t val) {
    static_dims_.push_back(std::make_unique<StaticDimInfo>(val));
    return static_dims_.back().get();
  }

  struct ParamInfo {
    const char* name = nullptr;
    const char* dtype = nullptr;
    std::vector<DimInfo*> shape;
  };

  struct ConstInfo {
    const char* name = nullptr;
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
    int32_t dtype;
    int64_t offset;
    size_t data_size;
  };

  std::vector<ParamInfo> inputs_info_;
  std::vector<ParamInfo> outputs_info_;
  std::vector<ConstInfo> constants_info_;

  std::shared_ptr<ConstantMap> constants_;

  // A directory with CUDA binary files, e.g. compiled kernels, etc.
  const std::optional<std::string> cubin_dir_;

  // Record if the model finishes an inference run so that its owning
  // AOTModelContainer can re-use this instance.
#ifdef USE_CUDA
  std::optional<cudaEvent_t> run_finished_;
#else // !USE_CUDA
  bool run_finished_;
#endif

  // Generated model uses this device index to create CUDA guards.
  int device_idx_;

 protected:
  std::vector<std::unique_ptr<StaticDimInfo>> static_dims_;
  // A map from dynamic symbol names to their dim info
  std::unordered_map<std::string, std::unique_ptr<DynamicDimInfo>>
      dynamic_dims_;

 private:
  std::vector<int64_t> shape(
      const std::vector<ParamInfo>& params,
      int64_t idx,
      bool max = false) const {
    std::vector<int64_t> shape;
    const ParamInfo& param = params.at(idx);
    auto rank = param.shape.size();
    shape.reserve(rank);
    for (size_t i = 0; i < rank; i++) {
      if (max) {
        shape.push_back(param.shape[i]->upper_bound());
      } else {
        shape.push_back(param.shape[i]->value());
      }
    }
    return shape;
  }

  std::vector<int64_t> max_shape(
      const std::vector<ParamInfo>& params,
      int64_t idx) const {
    return shape(params, idx, /*max=*/true);
  }
};

class AOTInductorModel : public AOTInductorModelBase<AOTInductorModel> {
 public:
  AOTInductorModel(std::shared_ptr<ConstantMap>, std::optional<std::string>);

  void run_impl(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  static std::unique_ptr<AOTInductorModel> Create(
      std::shared_ptr<ConstantMap> constants,
      std::optional<std::string> cubin_dir) {
    return std::make_unique<AOTInductorModel>(constants, cubin_dir);
  }
};

#ifdef USE_CUDA
class AOTICudaStreamGuard {
 public:
  AOTICudaStreamGuard(cudaStream_t stream, int32_t device_index) {
    CUDAStreamGuardHandle ptr;
    AOTI_TORCH_ERROR_CODE_CHECK(
        aoti_torch_create_cuda_stream_guard(stream, device_index, &ptr));
    guard_ =
        std::unique_ptr<void, std::function<void(void*)>>(ptr, [](void* ptr) {
          AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_delete_cuda_stream_guard(
              reinterpret_cast<CUDAStreamGuardHandle>(ptr)));
        });
  }

 private:
  std::unique_ptr<void, std::function<void(void*)>> guard_;
};
#endif // USE_CUDA

} // namespace aot_inductor
} // namespace torch
