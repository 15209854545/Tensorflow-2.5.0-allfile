/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CORE_GRAPH_MKL_GRAPH_UTIL_H_
#define TENSORFLOW_CORE_GRAPH_MKL_GRAPH_UTIL_H_
#ifdef INTEL_MKL

#include "absl/base/call_once.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/cpu_info.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {
// Since our ops are going to produce and also consume N addition tensors
// (Mkl) for N Tensorflow tensors, we can have following different
// orderings among these 2N tensors.
//
// E.g., for Tensorflow tensors A, B, and C, our ops will produce and
// consume A_m, B_m, and C_m additionally.
//
// INTERLEAVED: in this case 2N tensors are interleaved. So for above
//              example, the ordering looks like: A, A_m, B, B_m, C, C_m.
//
// CONTIGUOUS: in thi case N Tensorflow tensors are contiguous followed
//             by N Mkl tensors. So for above example, the ordering looks
//             like: A, B, C, A_m, B_m, C_m
//
// Following APIs map index of original Tensorflow tensors to their
// appropriate position based on selected ordering. For contiguous ordering,
// we need to know the total number of tensors (parameter total).
//
typedef enum { TENSORS_INTERLEAVED, TENSORS_CONTIGUOUS } MklTfTensorOrdering;
// NOTE: Currently, we use contiguous ordering. If you change this, then you
// would need to change Mkl op definitions in nn_ops.cc.
static const MklTfTensorOrdering kTensorOrdering = TENSORS_CONTIGUOUS;

// Get index of MetaData tensor from index 'n' of Data tensor.
inline int DataIndexToMetaDataIndex(int n, int total_tensors) {
  if (kTensorOrdering == MklTfTensorOrdering::TENSORS_INTERLEAVED) {
    // For interleaved ordering, Mkl tensor follows immediately after
    // Tensorflow tensor.
    return n + 1;
  } else {
    CHECK_EQ(kTensorOrdering, MklTfTensorOrdering::TENSORS_CONTIGUOUS);
    // For contiguous ordering, Mkl tensor is n+total_tensors / 2 away.
    return n + total_tensors / 2;
  }
}

int inline GetTensorDataIndex(int n, int total_tensors) {
  if (kTensorOrdering == MklTfTensorOrdering::TENSORS_INTERLEAVED) {
    return 2 * n;  // index corresponding to nth input/output tensor
  } else {
    CHECK_EQ(kTensorOrdering, MklTfTensorOrdering::TENSORS_CONTIGUOUS);
    return n;
  }
}

int inline GetTensorMetaDataIndex(int n, int total_tensors) {
  // Get index for TensorData first and then use mapping function
  // to get TensorMetaData index from TensorData index.
  int tidx = GetTensorDataIndex(n, total_tensors);
  return DataIndexToMetaDataIndex(tidx, total_tensors);
}

// check if the control between src and dst nodes already exists
bool inline DoesControlEdgeExist(const Node* src, const Node* dst) {
  for (const Edge* edge : src->out_edges()) {
    if (edge->IsControlEdge() && edge->dst() == dst) {
      return true;
    }
  }
  return false;
}

// Check if graph should run in layout-dependent mode or native format mode
// based on environment variable setting. Native format mode is default. User
// can set TF_ENABLE_MKL_NATIVE_FORMAT=0 to disable the native format mode.
bool inline NativeFormatEnabled() {
#ifndef ENABLE_MKL
  return true;
#else
  static bool native_fmt_enabled = true;
  static absl::once_flag once;
  absl::call_once(once, [&] {
    TF_CHECK_OK(ReadBoolFromEnvVar("TF_ENABLE_MKL_NATIVE_FORMAT",
                                   /*default_value*/ true,
                                   &native_fmt_enabled));
  });
  return native_fmt_enabled;
#endif
}

// Check if the data_format attribute in the node def represents 5D tensor
bool inline Check5DFormat(const NodeDef& ndef) {
  string data_format;
  TF_CHECK_OK(GetNodeAttr(ndef, "data_format", &data_format));
  if (data_format.compare("NCDHW") == 0 || data_format.compare("NDHWC") == 0) {
    return true;
  }
  return false;
}

namespace mkl_op_registry {
// MKL operators whose kernels are registered with 'MklLayoutDependentOp' label
// (e.g., MklConv2D) understand input tensors in MKL layout. These operators
// get additional meta-tensors for actual input tensors.
static const char* kMklLayoutDependentOpLabel = "MklLayoutDependentOp";
static const char* kMklLayoutDependentOpLabelPattern =
    "label='MklLayoutDependentOp'";
// MKL operators whose kernels are registered with 'MklNameChangeOp' label
// (e.g., MklMatMul, MklTranspose) do not understand input tensors in MKL
// layout. These operators do not get additional meta-tensors. The signatures of
// these operators are the same as the original TensorFlow operators that they
// correspond to. So these ops just go through a name change during graph
// rewrite pass.
static const char* kMklNameChangeOpLabel = "MklNameChangeOp";
static const char* kMklNameChangeOpLabelPattern = "label='MklNameChangeOp'";
static const char* kMklQuantizedOpLabel = "QuantizedMklOp";
static const char* kMklQuantizedOpLabelPattern = "label='QuantizedMklOp'";

// Prefix that we add to Tensorflow op name to construct Mkl op name.
static const char* const kMklOpPrefix = "_Mkl";
// TODO(intel-tf): PR review feedback (penpornk)
// Can we add eager_mode (or is_eager) as an op attribute instead?
// This way we don't need to rename the op just to pass eager_mode
// through template parameter.
static const char* const kMklEagerOpPrefix = "_MklEager";

// Prefix that we add to TF op name to construct MKL op that does not
// depend on layout propagation. It will be used in both Eager and graph
// modes unless there is a reason to have additional op name with
// _MklEager prefix.
static const char* const kMklNativeOpPrefix = "_MklNative";

// Get the name of Mkl Native (does not depend on layout propagation) op
// from original TensorFlow op.
inline string GetMklNativeOpName(const string& name) {
  // There are few operators that don't depend on layout propagation but are
  // prefixed with _Mkl instead of _MklNative.
  bool result =
      (0 == name.compare("ConjugateTranspose") ||
       0 == name.compare("BatchMatMul") || 0 == name.compare("BatchMatMulV2") ||
       0 == name.compare("MatMul") || 0 == name.compare("Transpose"));
  if (result) {
    return string(kMklOpPrefix) + name;
  } else {
    return string(kMklNativeOpPrefix) + name;
  }
}

// Get the name of Mkl op from original TensorFlow op
// We prefix the original op with _Mkl or _MklNative to get Mkl op.
inline string GetMklOpName(const string& name) {
  if (!NativeFormatEnabled()) {
    return string(kMklOpPrefix) + name;
  } else {
    return GetMklNativeOpName(name);
  }
}

// Get the name of Mkl Eager op from original TensorFlow op
// We prefix 'MklEager' to the original op to get Mkl Eager op.
inline string GetMklEagerOpName(const string& name) {
  return string(kMklEagerOpPrefix) + name;
}

static inline bool IsBF16SupportedByOneDNNOnThisCPU() {
  return port::TestCPUFeature(port::CPUFeature::AVX512F);
}

static inline void BF16UnsupportedWarning() {
  static absl::once_flag cpu_bfloat16_warn_once_flag;
  absl::call_once(cpu_bfloat16_warn_once_flag, [] {
    LOG(ERROR) << "oneDNN BFloat16 support are only on platforms with AVX512. "
                  "Falling back to default implementation if present.";
  });
}

// Check whether opname with type T is registered as MKL operator
// that can accept input tensors in MKL layout.
//
// @input: name of the op
// @input: T datatype to be used for checking op
// @return: true if opname is registered as Mkl-layout dependent op;
// false otherwise
static inline bool IsMklLayoutDependentOp(const string& op_name, DataType T) {
  string kernel = KernelsRegisteredForOp(op_name);

  // Restrict quantized ops to QUINT8 and QINT8 for now
  if (kernel.find(kMklQuantizedOpLabelPattern) != string::npos) {
    return (T == DT_QUINT8 || T == DT_QINT8 || T == DT_QINT32);
  }
  // Restrict regular ops to FLOAT and BFLOAT16
  if (kernel.find(kMklLayoutDependentOpLabelPattern) != string::npos) {
    if (T == DT_FLOAT) return true;
    if (T == DT_BFLOAT16) {
      if (IsBF16SupportedByOneDNNOnThisCPU()) {
        return true;
      } else {
        // Restrict bfloat16 ops to platforms with at least AVX512 support, fall
        // back to Eigen implementation otherwise.
        BF16UnsupportedWarning();
        return false;
      }
    }
    return false;
  }
  return false;
}

// TODO(mdfaijul): QuantizedConv2D is registered with input: QUINT8
// filter:QINT8 for mkldnn integration. First a dummy kernel is created
// and then it is replaced by an actual kernel.
static inline bool IsMklLayoutDependentOp(const string& op_name,
                                          DataType Tinput, DataType Tfilter) {
  string kernel = KernelsRegisteredForOp(op_name);

  // Restrict quantized ops to QUINT8 and QINT8 for now
  if (kernel.find(kMklQuantizedOpLabelPattern) != string::npos) {
    return (Tfilter == DT_QINT8);
  }
  return false;
}

// Check whether opname with type T is registered as an MKL operator that
// will go through name change.
//
// @input: name of the op
// @input: T datatype to be used for checking op
// @return: true if opname is registered as MKL op that will go through name
// change; false otherwise
static inline bool IsMklNameChangeOp(const string& op_name, DataType T) {
  string kernel = KernelsRegisteredForOp(op_name);
  // String returned by KernelsRegisteredForOp looks like below:
  //
  // Op = _MklMatMul, kernels =
  // device='CPU'; label='MklNameChangeOp'; T in [DT_COMPLEX128]
  // device='CPU'; label='MklNameChangeOp'; T in [DT_COMPLEX64]
  // device='CPU'; label='MklNameChangeOp'; T in [DT_DOUBLE]
  // device='CPU'; label='MklNameChangeOp'; T in [DT_FLOAT]

  // Now we just construct a search string to match what we are looking for.
  string search_string = kMklNameChangeOpLabelPattern;
  search_string += string(";") + string(" T in [");
  search_string += DataType_Name(T) + string("]");

  // Temporarily replacing earlier check by adding a type-specific check so
  // that we can selectively decide which type is supported by MKL operators.
  // That way kernel registration does not decide which operators we support.
  // We are using this change to temporarily disable BFLOAT16 support. Once
  // we want to enable it, we will go back to earlier check.
  bool isTypeAllowed = false;
  if (kernel.find(search_string) != string::npos) {
    isTypeAllowed = (T == DT_COMPLEX128 || T == DT_COMPLEX64 ||
                     T == DT_DOUBLE || T == DT_FLOAT);
    if (!isTypeAllowed) {
      if (T == DT_BFLOAT16) {
        if (IsBF16SupportedByOneDNNOnThisCPU()) {
          isTypeAllowed = true;
        } else {
          // Restrict bfloat16 ops to platforms with at least AVX512 support,
          // fall back to Eigen implementation otherwise.
          BF16UnsupportedWarning();
          isTypeAllowed = false;
        }
      }
    }
    return isTypeAllowed;
  }

  return false;
}

// Check if the operator with 'op_name' and type 'T' is an MKL operator that
// will either understand input tensors in MKL layout or will go through name
// rewrite that some operators go through.
static inline bool IsMklOp(const string& op_name, DataType T) {
  return IsMklLayoutDependentOp(op_name, T) || IsMklNameChangeOp(op_name, T);
}

static inline bool IsMklOp(const Node* n) {
  DataType T;
  return GetNodeAttr(n->def(), "T", &T).ok() && IsMklOp(n->type_string(), T);
}

// Check whether opname with type T is registered as MKL-compliant and
// is element-wise.
//
// @input: name of the op
// @input: T datatype to be used for checking op
// @return: true if opname is registered as element-wise Mkl op;
// false otherwise
static inline bool IsMklElementWiseOp(const string& op_name, DataType T) {
  if (!IsMklOp(op_name, T)) {
    return false;
  }
  bool result = (0 == op_name.compare(GetMklOpName("Add")) ||
                 0 == op_name.compare(GetMklOpName("AddV2")) ||
                 0 == op_name.compare(GetMklOpName("Sub")) ||
                 0 == op_name.compare(GetMklOpName("Mul")) ||
                 0 == op_name.compare(GetMklOpName("Maximum")) ||
                 0 == op_name.compare(GetMklOpName("SquaredDifference")));

  return result;
}
}  // namespace mkl_op_registry
}  // namespace tensorflow
#endif  // INTEL_MKL
#endif  // TENSORFLOW_CORE_GRAPH_MKL_GRAPH_UTIL_H_