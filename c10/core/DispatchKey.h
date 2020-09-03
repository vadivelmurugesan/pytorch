#pragma once

#include <vector>
#include <iostream>
#include <string>
#include <c10/macros/Macros.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Exception.h>

namespace c10 {

// Semantically, a dispatch key identifies a possible "level" in our
// dispatch, for which a handler may be registered.  Traditional
// backends like CPU and CUDA get dispatch keys; however, so do
// "wrapping" layers like Variable (for autograd handling).
//
// In implementation terms, the dispatch key identifies a specific "bit" in a
// DispatchKeySet.  Higher bit indexes get handled by dispatching first (because
// we "count leading zeros" when we extract the highest priority dispatch
// key.)
enum class DispatchKey : uint8_t {

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~ UNDEFINED ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  // This is not a "real" tensor id, but it exists to give us a "nullopt"
  // element we can return for cases when a DispatchKeySet contains no elements.
  // You can think a more semantically accurate definition of DispatchKey is:
  //
  //    using DispatchKey = optional<RealDispatchKey>
  //
  // and Undefined == nullopt.  We didn't actually represent
  // it this way because optional<RealDispatchKey> would take two
  // words, when DispatchKey fits in eight bits.

  Undefined = 0,

  // Define an alias for Undefined to represent CatchAll (long term
  // this will get eliminated, but for now it's convenient)
  CatchAll = Undefined,

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~ BACKENDS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  // A "backend" is colloquially used to refer to handlers for dispatch
  // which actually implement the numerics of an operation in question.
  //
  // Due to the nature of the enum, these backends are specified in
  // an ordered way, but for most backends this order is not semantically
  // meaningful (e.g., it's valid to reorder these backends without changing
  // semantics).  The only situation when backend ordering is meaningful
  // is when the backend participates in multiple dispatch with another
  // backend; e.g., CPU and SparseCPU (sparse must have
  // higher priority).

  // Here are backends which you think of as traditionally specifying
  // how to implement operations on some device.
  CPU, // registered at build/aten/src/ATen/CPUType.cpp
  CUDA, // registered at build/aten/src/ATen/CUDAType.cpp
  HIP, // NB: I think this is not actually used, due to Note [Masquerading as
       // CUDA]
  FPGA, // Xilinx support lives out of tree at https://gitlab.com/pytorch-complex/vitis_kernels
  MSNPU, // unused externally, but tested at
         // test/cpp_extensions/msnpu_extension.cpp
  XLA, // lives out of tree at https://github.com/pytorch/xla
  Vulkan,

  // These are Caffe2 device types which we grandfathered into
  // DispatchKey.
  // TODO: Caffe2-only DispatchKeys actually should be removed from this enum
  // and just simply be undispatchable.
  MKLDNN, // (MKLDNN is treated as another "device" in Caffe2)
  OpenGL,
  OpenCL,
  IDEEP,

  // Here are backends which specify more specialized operators
  // based on the dtype of the tensor.
  QuantizedCPU, // registered at build/aten/src/ATen/QuantizedCPUType.cpp
  QuantizedCUDA, // registered at build/aten/src/ATen/QuantizedCUDAType.cpp
  ComplexCPU, // lives out of tree at
              // https://gitlab.com/pytorch-complex/pytorch-cpu-strided-complex
  ComplexCUDA, // and
               // https://gitlab.com/pytorch-complex/pytorch-cuda-strided-complex
  // tested at test/cpp_extensions/complex_registration_extension.cpp
  // TODO: Remove Complex dispatch keys when Complex is moved in tree

  // This backend is to support custom RNGs; it lets you go
  // to a different kernel if you pass in a generator that is not a
  // traditional CPUGeneratorImpl/CUDAGeneratorImpl.  To make use of this
  // key:
  //  1) set it as a second parameter of at::Generator constructor call in
  //     the user-defined PRNG class.
  //  2) use it as a dispatch key while registering custom kernels
  //     (templatized kernels specialized for user-defined PRNG class)
  // intended for out of tree use; tested by aten/src/ATen/test/rng_test.cpp
  CustomRNGKeyId,

  // Here are backends which specify more specialized operators
  // based on the layout of the tensor.  Note that the sparse backends
  // are one case where ordering matters: sparse multi-dispatches with
  // the corresponding dense tensors, and must be handled before them.
  MkldnnCPU, // registered at build/aten/src/ATen/MkldnnCPUType.cpp
  // NB: not to be confused with MKLDNN, which is Caffe2 only
  SparseCPU, // registered at build/aten/src/ATen/SparseCPUType.cpp
  SparseCUDA, // registered at build/aten/src/ATen/SparseCUDAType.cpp
  SparseHIP, // TODO: I think this is not actually used, due to Note
             // [Masquerading as CUDA]

  // Here are reserved backends for user-defined backends, see Note [Private use
  // DispatchKey]
  // To see some example about how to use this, check out MSNPU
  PrivateUse1,
  PrivateUse2,
  PrivateUse3,

  // Define an alias key to represent end of backend dispatch keys.
  // If you add new backend keys after PrivateUse3, please also update it here.
  EndOfBackendKeys = PrivateUse3,

  // The meta function characterizes how an operation affects the metadata of a
  // tensor (shape, dtype) without doing any of the actual computation.  A
  // meta tensor can be used to dry run operators without actually doing
  // any computation, e.g., add on two meta tensors would give you another
  // meta tensor with the output shape and dtype, but wouldn't actually
  // add anything.  A meta implementation typically would look something like:
  //
  //  Tensor meta::add(const Tensor& self, const Tensor& other) {
  //    TORCH_CHECK(self.size().equals(other.size()));
  //    return at::empty_like(self, self.size());
  //  }
  //
  // The meta function would get invoked if you ran an operator passing
  // in meta tensors.  The call stack in such a case would look something like
  // this:
  //
  //  at::add(x: Meta, y: Meta) {
  //    return [dispatch] meta::add(x: Meta, y: Meta) {
  //      output_shape = ...
  //      [dispatch] meta::empty(output_shape) {
  //        return ... meta tensor with output_shape but no data allocated ...
  //      }
  //    }
  //  }
  //
  // Meta functions have an important secondary function, which is they can
  // be used as tensor "allocators".  A typical backend implementation should
  // be implemented in this way:
  //
  //  Tensor cpu::add(const Tensor& self, const Tensor& other) {
  //    Tensor result = meta::add(self, other);
  //    // ... do the actual computation into result ...
  //    return result;
  //  }
  //
  // In this case, the internal at::empty_like invocation would dispatch to the
  // CPU factory function, not the meta factory function.  The call stack in
  // this case looks like:
  //
  //  at::add(x: CPU, y: CPU) {
  //    return [dispatch] cpu::add(x: CPU, y: CPU) {
  //      output = [direct] meta::add(x: CPU, y: CPU) {
  //        output_shape = ...
  //        [dispatch] cpu::empty(output_shape)
  //      }
  //      ... compute on output ...
  //      return output;
  //    }
  //  }
  //
  Meta,

  // In some situations, it is not immediately obvious what the correct
  // backend for function is, because the function in question doesn't
  // have any "tensor" arguments.  In this case, a BackendSelect function
  // can be registered to implement the custom determination of the
  // correct backend.
  BackendSelect,

  // The named dispatch key is set for any tensors with named dimensions.
  // Although we have a dispatch key for named tensors, for historical reasons,
  // this dispatch key doesn't do any of the substantive functionality for named
  // tensor (though, hypothetically, it could!)  At the moment, it's just
  // responsible for letting us give good error messages when operations
  // don't support named tensors.
  //
  // NB: If you ever consider moving named tensor functionality into
  // this dispatch key, note that it might be necessary add another dispatch
  // key that triggers before composite operators, in case a composite operator
  // has named dimension propagation that doesn't match that of its
  // constituent parts.
  Named,

  // Note [Alias Dispatch Key : Autograd]
  // All backends are oblivious to autograd; autograd is handled as a
  // layer which happens on top of all backends. It inspects the autograd
  // metadata of all inputs, determines what autograd metadata should be
  // constructed by the output, and otherwise defers to the backend to
  // actually do the numeric computation.  Autograd contains
  // the bulk of this logic.

  // Autograd is now an alias dispatch key which by default maps to all
  // backend-specific autograd keys.
  // Backend-specific allow backends to override the default kernel registered
  // to Autograd key as needed.
  // For example, XLA wants to define autograd for einsum directly.
  // Registering a custom autograd implementation at the XLA key won't work
  // because we process Autograd before XLA.  This key has higher priority and
  // gets processed first.  You generally should NOT redispatch after handling
  // autograd here (since that would result in execution of the Autograd
  // operator, which you're trying to skip).  In AutogradXLA implementations,
  // you are responsible for handling autograd yourself, or deferring to other
  // operators which support autograd.

  // Currently we only have backend-specific autograd keys for CPU/CUDA/XLA and
  // reserved user-defined backends. All other in-tree backends share the
  // AutogradOther key. We can add specific autograd key for those backends
  // upon request.
  AutogradOther,
  AutogradCPU,
  AutogradCUDA,
  AutogradXLA,
  // Here are some reserved pre-autograd keys for user-defined backends, see
  // Note [Private use DispatchKey]
  AutogradPrivateUse1,
  AutogradPrivateUse2,
  AutogradPrivateUse3,

  Tracer,

  // Autocasting precedes VariableTypeId, to ensure casts are autograd-exposed
  // and inputs are saved for backward in the post-autocast type.
  Autocast,

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~ WRAPPERS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  // There are a number of alternative modes which may want to handle before
  // autograd; for example, error checking, tracing, profiling or vmap.  They
  // go here.

  // This is the dispatch key for BatchedTensorImpl, which is used to implement
  // batching rules for vmap.
  Batched,

  // When we are inside a vmap, all tensors dispatch on this key.
  // See Note: [DispatchKey::VmapMode usage] for more details.
  VmapMode,

  // TESTING: This is intended to be a generic testing tensor type id.
  // Don't use it for anything real; its only acceptable use is within a single
  // process test.  Use it by creating a TensorImpl with this DispatchKey, and
  // then registering operators to operate on this type id.  See
  // aten/src/ATen/core/dispatch/backend_fallback_test.cpp for a usage example.
  TESTING_ONLY_GenericWrapper,

  // TESTING: This is intended to be a generic testing tensor type id.
  // Don't use it for anything real; its only acceptable use is within a ingle
  // process test.  Use it by toggling the mode on and off via
  // TESTING_ONLY_tls_generic_mode_set_enabled and then registering operators
  // to operate on this type id.  See
  // aten/src/ATen/core/dispatch/backend_fallback_test.cpp
  // for a usage example
  TESTING_ONLY_GenericMode,

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ FIN ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  NumDispatchKeys, // Sentinel, end of runtime keys.

  // ~~~~~~~~~~~~~~~~~~~~~~ Alias Dispatch Keys ~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  // Alias dispatch keys are synthetic dispatch keys which map to multiple
  // runtime dispatch keys. Alisa keys have precedence, but they are always
  // lower precedence than runtime keys. You can register a kernel to an
  // alias key, the kernel might be populated to the mapped runtime keys
  // during dispatch table computation.
  // If a runtime dispatch key has multiple kernels from alias keys, which
  // kernel wins is done based on the precedence of alias keys (but runtime
  // keys always have precedence over alias keys).
  // Alias keys won't be directly called during runtime.

  // See Note [Alias Dispatch Key : Autograd]
  Autograd,

  // ~~~~~~~~~~~~~~~~~~~~~~~~~ BC ALIASES ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
  // The aliases exist for backwards compatibility reasons, they shouldn't
  // be used
  CPUTensorId = CPU,
  CUDATensorId = CUDA,
  PrivateUse1_PreAutograd = AutogradPrivateUse1,
  PrivateUse2_PreAutograd = AutogradPrivateUse2,
  PrivateUse3_PreAutograd = AutogradPrivateUse3,
};

// Note [Private use DispatchKey]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Private use tensor IDs are preallocated tensor type IDs for use in user
// applications.  Similar to private use fields in HTTP, they can be used
// by end users for experimental or private applications, without needing
// to "standardize" the tensor ID (which would be done by submitting a PR
// to PyTorch to add your type ID).
//
// Private use tensor IDs are appropriate to use if you want to experiment
// with adding a new tensor type (without having to patch PyTorch first) or
// have a private, non-distributed application that needs to make use of a
// new tensor type.  Private use tensor IDs are NOT appropriate to use for
// libraries intended to be distributed to further users: please contact
// the PyTorch developers to get a type ID registered in this case.
//
// We provide two classes of private user tensor id: regular DispatchKeys
// and Autograd DispatchKeys.  DispatchKeys serve the role of ordinary "backend"
// DispatchKeys; if you were adding support for a new type of accelerator, you
// would use a backend DispatchKey, and ideally automatically reuse AutogradOther
// definitions already defined in PyTorch.  AutogradPrivateUse DispatchKeys serve
// as "wrapper" DispatchKeys: they are only necessary for tensors that compose
// multiple internal tensors, and for cases when the built-in autograd formulas
// for operators are not appropriate.

static_assert(
  static_cast<uint8_t>(DispatchKey::NumDispatchKeys) < 64,
  "DispatchKey is used as index into 64-bit bitmask; you must have less than 64 entries");

C10_API const char* toString(DispatchKey);
C10_API std::ostream& operator<<(std::ostream&, DispatchKey);

C10_API DispatchKey getAutogradKeyFromBackend(DispatchKey t);

// These are some convenience identifiers for dispatch keys which are
// shorter to type than their long counterparts.  Note that some of these
// dispatch keys directly correspond to DeviceType; and most APIs that
// accept DispatchKey also accept DeviceType; e.g.,
// torch::dispatch(torch::kCPU, ...) is also valid.
constexpr DispatchKey kAutograd = DispatchKey::Autograd;

// Check if a DispatchKey is an alias mapping to other runtime keys.
inline bool isAliasDispatchKey(DispatchKey k) {
  return k == DispatchKey::Autograd;
}
} // namespace c10

namespace torch {
  // Expose the constant, but not the TYPE (DispatchKey is an implementation
  // detail!)
  using c10::kAutograd;
}

// NB: You really shouldn't use this instance; this enum is guaranteed
// to be pretty small so a regular array should be acceptable.
namespace std {
template <>
struct hash<c10::DispatchKey> {
  typedef size_t result_type;
  typedef c10::DispatchKey argument_type;

  size_t operator()(c10::DispatchKey x) const {
    return static_cast<size_t>(x);
  }
};
}
