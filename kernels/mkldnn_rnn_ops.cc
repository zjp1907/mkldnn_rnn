/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#ifdef INTEL_MKL

#define EIGEN_USE_THREADS

#include <stddef.h>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/kernel_def_builder.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_def_builder.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/util/env_var.h"

#include "tensorflow/contrib/mkldnn_rnn/mkl-dnn/include/mkldnn.hpp"

/*
 * This module implements ops that fuse a multi-layer multi-step RNN/LSTM model
 * using the underlying Mkldnn library.
 *
 * Similar to many other ops, the forward op has two flavors: training and
 * inference. When training is specified, additional data in reserve_space will
 * be produced for the backward pass. So there is a performance penalty.
 *
 */
namespace tensorflow {
using CPUDevice = Eigen::ThreadPoolDevice;

template <typename Device, typename T, typename Index>
class MkldnnRNNParamsSizeOp;

template <typename Device, typename T>
class MkldnnRNNForwardOp;

template <typename Device, typename T>
class MkldnnRNNBackwardOp;

using mkldnn::memory;
using mkldnn::algorithm;
using mkldnn::direction;
using mkldnn::input_mode;
using mkldnn::engine;
using mkldnn::prop_kind;
using mkldnn::stream;
using mkldnn::error;
using mkldnn::rnn_forward;
using mkldnn::rnn_backward;
using mkldnn::primitive;

// #define OP_DATA_DUMP

#ifdef OP_DATA_DUMP
template <typename T>
void dump_data(FILE *fp, const int size, T *data) {
  LOG(ERROR) << "array size: " << size;

  for (int i = 0; i < size; i++) {
    if (i % 5 == 0) {
      fprintf(fp, "\n");
    }
    fprintf(fp, "%f, ", ((float*)data)[i]);
  }
}
#endif

Status ParseRNNMode(const string& str, algorithm* rnn_mode) {
  if (str == "rnn_relu") {
    *rnn_mode = algorithm::rnn_relu;
    return Status::OK();
  } else if (str == "rnn_tanh") {
    *rnn_mode = algorithm::rnn_tanh;
    return Status::OK();
  } else if (str == "lstm") {
    *rnn_mode = algorithm::rnn_lstm;
    return Status::OK();
  } else if (str == "gru") {
    *rnn_mode = algorithm::rnn_gru;
    return Status::OK();
  }
  return errors::InvalidArgument("Invalid RNN mode: ", str);
}

Status ParseRNNInputMode(const string& str, input_mode* rnn_input_mode) {
  if (str == "linear_input") {
    *rnn_input_mode = input_mode::rnn_linear_input;
    return Status::OK();
  } else if (str == "skip_input") {
    *rnn_input_mode = input_mode::rnn_skip_input;
    return Status::OK();
  } else if (str == "auto_select") {
    *rnn_input_mode = input_mode::rnn_linear_input;
    return Status::OK();
  }
 
  return errors::InvalidArgument("Invalid RNN input mode: ", str);
}

Status ParseRNNDirectionMode(const string& str,
                             direction* rnn_dir_mode) {
  if (str == "unidirectional") {
    *rnn_dir_mode = direction::rnn_unidirectional;
    return Status::OK();
  } else if (str == "bidirectional") {
    *rnn_dir_mode = direction::rnn_bidirectional;
    return Status::OK();
  }
  return errors::InvalidArgument("Invalid RNN direction mode: ", str);
}

struct MkldnnModelTypes {
  algorithm rnn_mode;
  input_mode rnn_input_mode;
  direction rnn_direction_mode;
  bool HasInputC() const {
    // only LSTM has input-c. All other models use only input-h.
    return rnn_mode == algorithm::rnn_lstm;
  }
};

// A helper class that collects the shapes to describe a RNN model.
struct MkldnnModelShapes {
  int num_layers;
  int input_size;
  int num_units;
  int seq_length;
  int batch_size;
  int dir_count;
  TensorShape input_shape;
  TensorShape output_shape;
  TensorShape hidden_state_shape;
  // At present only fields related to cached RnnDescriptor are concerned.
  bool IsCompatibleWith(const MkldnnModelShapes& rhs) const {
    return num_layers == rhs.num_layers && input_size == rhs.input_size &&
           num_units == rhs.num_units && dir_count == rhs.dir_count;
  }
  string RnnDescDebugString() {
    return strings::Printf(
        "[num_layers, input_size, num_units, dir_count]: [%d, %d, %d, %d]",
        num_layers, input_size, num_units, dir_count);
  }
};

// Extract and checks the forward input tensors, parameters, and shapes from the
// OpKernelContext.
Status ExtractForwardInput(OpKernelContext* context,
                           const MkldnnModelTypes& model_types,
                           const Tensor** input, const Tensor** input_h,
                           const Tensor** input_c, const Tensor** params,
                           MkldnnModelShapes* model_shapes) {
  TF_RETURN_IF_ERROR(context->input("input", input));
  TF_RETURN_IF_ERROR(context->input("input_h", input_h));
  if (model_types.HasInputC()) {
    TF_RETURN_IF_ERROR(context->input("input_c", input_c));
  }
  TF_RETURN_IF_ERROR(context->input("params", params));

  // input layout: T x N x F
  #if 0
  LOG(ERROR) << "input dims: " << (*input)->dims();
  for (int i = 0; i < (*input)->dims(); i++) {
    LOG(ERROR) << (*input)->dim_size(i) << ", ";
  } 
  #endif
  if ((*input)->dims() == 2) {
    model_shapes->seq_length = 1;
    model_shapes->batch_size = (*input)->dim_size(0);
    model_shapes->input_size = (*input)->dim_size(1);
    model_shapes->input_shape = TensorShape({1, model_shapes->batch_size, model_shapes->input_size});
  } else {
    model_shapes->seq_length = (*input)->dim_size(0);
    model_shapes->batch_size = (*input)->dim_size(1);
    model_shapes->input_size = (*input)->dim_size(2);
    model_shapes->input_shape = (*input)->shape();
  }

  model_shapes->dir_count = (model_types.rnn_direction_mode == direction::rnn_bidirectional) ? 2 : 1;

  // hx layout: (L * dir_count) x N x num_units
  #if 0
  LOG(ERROR) << "input_h dims: " << (*input_h)->dims();
  for (int i = 0; i < (*input_h)->dims(); i++) {
    LOG(ERROR) << (*input_h)->dim_size(i) << ", ";
  }
  #endif
  if ((*input_h)->dims() == 2) {
    model_shapes->num_layers = 1;
    model_shapes->num_units = (*input_h)->dim_size(1); 
    model_shapes->hidden_state_shape = TensorShape({model_shapes->batch_size, model_shapes->num_units});
  } else {
    model_shapes->num_layers = (*input_h)->dim_size(0) / model_shapes->dir_count;
    model_shapes->num_units = (*input_h)->dim_size(2);
    model_shapes->hidden_state_shape = TensorShape({model_shapes->dir_count * model_shapes->num_layers,
                                                    model_shapes->batch_size, model_shapes->num_units});
  }

  // cx layout: (L * dir_count) x N x num_units
  if (model_types.HasInputC()) {
    if ((*input_h)->shape() != (*input_c)->shape()) {
      return errors::InvalidArgument(
          "input_h and input_c must have the same shape: ",
          (*input_h)->shape().DebugString(), " ",
          (*input_c)->shape().DebugString());
    }
  }

  // output layout: T x N x (dir_count * num_units)
  if ((*input)->dims() == 2) {
    model_shapes->output_shape = TensorShape({model_shapes->batch_size,
                                              model_shapes->dir_count * model_shapes->num_units});
  } else {
    model_shapes->output_shape = TensorShape({model_shapes->seq_length, model_shapes->batch_size,
                                              model_shapes->dir_count * model_shapes->num_units});
  }
  return Status::OK();
}


// A common base class for RNN kernels. It extracts common attributes and
// shape validations.
class MkldnnRNNKernelCommon : public OpKernel {
 protected:
  explicit MkldnnRNNKernelCommon(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("dropout", &dropout_));
    OP_REQUIRES_OK(context, context->GetAttr("seed", &seed_));
    OP_REQUIRES_OK(context, context->GetAttr("seed2", &seed2_));
    string str;
    OP_REQUIRES_OK(context, context->GetAttr("rnn_mode", &str));
    OP_REQUIRES_OK(context, ParseRNNMode(str, &model_types_.rnn_mode));
    OP_REQUIRES_OK(context, context->GetAttr("input_mode", &str));
    OP_REQUIRES_OK(context, ParseRNNInputMode(str, &model_types_.rnn_input_mode));
    OP_REQUIRES_OK(context, context->GetAttr("direction", &str));
    OP_REQUIRES_OK(context, ParseRNNDirectionMode(str, &model_types_.rnn_direction_mode));
  }

  bool HasInputC() const { return model_types_.HasInputC(); }
  algorithm rnn_mode() const { return model_types_.rnn_mode; }
  input_mode rnn_input_mode() const { return model_types_.rnn_input_mode; }
  direction rnn_direction_mode() const {
    return model_types_.rnn_direction_mode;
  }
  MkldnnModelTypes model_types() const { return model_types_; }
  float dropout() const { return dropout_; }
  uint64 seed() { return (static_cast<uint64>(seed_) << 32) | seed2_; }
 private:
  int seed_;
  int seed2_;
  float dropout_;
  // bool reset_rnd_gen_state_;

  MkldnnModelTypes model_types_;
};

int64 get_param_size(algorithm rnn_mode, int dir_count, int input_size, int num_units, int num_layers) {
  int first_layer_weights = 0;
  int higher_layer_weights = 0;
  int64 params_size = -1;

  switch (rnn_mode) {
    case algorithm::rnn_relu:
    case algorithm::rnn_tanh:
      first_layer_weights = num_units * (input_size + num_units + 2);
      higher_layer_weights = (num_layers - 1) * num_units * (num_units + num_units + 2);
      params_size = (first_layer_weights + higher_layer_weights) * dir_count;
      break;
    case algorithm::rnn_lstm:
      first_layer_weights = 4 * num_units * (input_size + num_units + 2);
      higher_layer_weights = 4 * (num_layers - 1) * num_units * (num_units + num_units + 2);
      params_size = (first_layer_weights + higher_layer_weights) * dir_count;
      break;
    case algorithm::rnn_gru:
      first_layer_weights = 3 * num_units * (input_size + num_units + 2);
      higher_layer_weights = 3 * (num_layers - 1) * num_units * (num_units + num_units + 2);
      params_size = (first_layer_weights + higher_layer_weights) * dir_count;
      break;
    default:
      LOG(WARNING) << "Invalid RNN mode: " << rnn_mode;
      break;
   }

   return params_size;
}

// A class that returns the size of the parameter buffer. The user should
// use that to create the actual parameter buffer for training. However, it
// should not be used for saving and restoring.
template <typename T, typename Index>
class MkldnnRNNParamsSizeOp<CPUDevice, T, Index> : public MkldnnRNNKernelCommon {
 public:
  typedef CPUDevice Device;
  explicit MkldnnRNNParamsSizeOp(OpKernelConstruction* context)
      : MkldnnRNNKernelCommon(context) {}

  void Compute(OpKernelContext* context) override {
    Index params_size = -1;
    int dir_count = rnn_direction_mode() == direction::rnn_unidirectional ? 1 : 2;

    const Tensor* num_layers_t = nullptr;
    context->input("num_layers", &num_layers_t);
    if (!TensorShapeUtils::IsScalar(num_layers_t->shape())) {
      LOG(ERROR) << "num_layers is not a scalar";
    }
    int num_layers = num_layers_t->scalar<int>()();

    const Tensor* num_units_t = nullptr;
    context->input("num_units", &num_units_t);
    if (!TensorShapeUtils::IsScalar(num_units_t->shape())) {
      LOG(ERROR) << "num_units is not a scalar";
    }
    int num_units = num_units_t->scalar<int>()();

    const Tensor* input_size_t = nullptr;
    context->input("input_size", &input_size_t);
    if (!TensorShapeUtils::IsScalar(input_size_t->shape())) {
      LOG(ERROR) << "input_size is not a scalar";
    }
    int input_size = input_size_t->scalar<int>()();

    params_size = get_param_size(rnn_mode(), dir_count, input_size, num_units, num_layers);

    Tensor* output_t = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, {1}, &output_t));
    *output_t->template flat<Index>().data() = params_size;
  }
};

REGISTER_KERNEL_BUILDER(Name("MkldnnRNNParamsSize")
                            .Device(DEVICE_CPU).TypeConstraint<float>("T").TypeConstraint<int32>("S"),
                        MkldnnRNNParamsSizeOp<CPUDevice, float, int32>);

// Run the forward operation of the RNN model.
template <typename T>
class MkldnnRNNForwardOp<CPUDevice, T> : public MkldnnRNNKernelCommon {
 public:
  typedef CPUDevice Device;
  explicit MkldnnRNNForwardOp(OpKernelConstruction* context)
      : MkldnnRNNKernelCommon(context) {
    OP_REQUIRES_OK(context, context->GetAttr("is_training", &is_training_));
  }

  void Compute(OpKernelContext* context) override {
    const Tensor* Tx = nullptr;
    const Tensor* Thx = nullptr;
    const Tensor* Tcx = nullptr;
    const Tensor* Tweights = nullptr;
    MkldnnModelShapes model_shapes;

    // LOG(ERROR) << "forward is called";

    OP_REQUIRES_OK(context,
                   ExtractForwardInput(context, model_types(), &Tx, &Thx,
                                       &Tcx, &Tweights, &model_shapes));

    const auto& hidden_state_shape = model_shapes.hidden_state_shape;
    const auto& output_shape = model_shapes.output_shape;

    Tensor* Ty = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &Ty));
    Tensor* Thy = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(1, hidden_state_shape, &Thy));
    Tensor* Tcy = nullptr;
    if (HasInputC()) {
      // Only LSTM uses input_c and output_c. So for all other models, we only
      // need to create dummy outputs.
      OP_REQUIRES_OK(
          context, context->allocate_output(2, hidden_state_shape, &Tcy));
    } else {
      OP_REQUIRES_OK(context, context->allocate_output(2, {}, &Tcy));
    }

    Tensor* Tworkspace = nullptr;
    if (!is_training_) {
      OP_REQUIRES_OK(context, context->allocate_output(3, {}, &Tworkspace));
    }

    memory *x;
    memory *hx;
    memory *cx;
    memory *y;
    memory *hy;
    memory *cy;
    memory *weights;
    memory *workspace;
    memory::desc *x_desc;
    memory::desc *hx_desc;
    memory::desc *y_desc;
    memory::desc *weights_desc;
    rnn_forward::primitive_desc *rnn_fwd_prim_desc;
    int state_outputs = 1;

    memory::data_type a_data_type = memory::data_type::f32;
    engine *eng = new engine(engine::kind::cpu, 0);

    const int total_w = get_param_size(rnn_mode(), model_shapes.dir_count, model_shapes.input_size, model_shapes.num_units, model_shapes.num_layers);

    x_desc = new memory::desc({model_shapes.seq_length, model_shapes.batch_size, model_shapes.input_size}, a_data_type, memory::format::rnx);
    hx_desc = new memory::desc({model_shapes.num_layers, model_shapes.batch_size, model_shapes.num_units}, a_data_type, memory::format::rnx);
    y_desc = new memory::desc({model_shapes.seq_length, model_shapes.batch_size, model_shapes.num_units * model_shapes.dir_count}, a_data_type, memory::format::rnx);
    weights_desc = new memory::desc({total_w}, a_data_type, memory::format::x);

    x = new memory({ *x_desc, *eng }, static_cast<void*>(const_cast<T*>(Tx->flat<T>().data())));
    hx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Thx->flat<T>().data())));
    y = new memory({ *y_desc, *eng }, static_cast<void*>(const_cast<T*>(Ty->flat<T>().data())));
    hy = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Thy->flat<T>().data())));
    weights = new memory({ *weights_desc, *eng }, static_cast<void*>(const_cast<T*>(Tweights->flat<T>().data())));
    if (HasInputC()) {
      cx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tcx->flat<T>().data())));
      cy = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tcy->flat<T>().data())));
    }

    prop_kind a_prop_kind = is_training_ ? prop_kind::forward_training : prop_kind::forward_inference;

    auto rnn_fwd_desc = mkldnn::rnn_forward::desc(a_prop_kind, rnn_mode(),
                                                  rnn_direction_mode(), rnn_input_mode(), model_shapes.num_units,
                                                  model_shapes.num_layers, model_shapes.seq_length,
                                                  state_outputs, *x_desc, *hx_desc, *y_desc, *weights_desc);
    rnn_fwd_prim_desc = new rnn_forward::primitive_desc(rnn_fwd_desc, *eng);

    std::vector<primitive> pipeline;
    auto s = stream(stream::kind::lazy);

    if (is_training_) {
      auto workspace_primitive_desc = rnn_fwd_prim_desc->workspace_primitive_desc();
      int workspace_size = workspace_primitive_desc.get_size() / sizeof(T);
      // LOG(ERROR) << "fwd workspace size is: " << workspace_size;
      OP_REQUIRES_OK(context, context->allocate_output(3, {workspace_size}, &Tworkspace));
      workspace = new memory(workspace_primitive_desc, static_cast<void*>(const_cast<T*>(Tworkspace->flat<T>().data())));
      if (HasInputC()) {
        auto l = rnn_forward(*rnn_fwd_prim_desc, x, hx, cx,
                             weights, y, hy, cy, workspace);
        pipeline.push_back(l);
        s.submit(pipeline).wait();
      } else {
        auto l = rnn_forward(*rnn_fwd_prim_desc, x, hx, nullptr,
                             weights, y, hy, nullptr, workspace);
        pipeline.push_back(l);
        s.submit(pipeline).wait();
      }
      delete workspace;
    } else {
      if (HasInputC()) {
        auto l = rnn_forward(*rnn_fwd_prim_desc, x, hx, cx,
                             weights, y, hy, cy, nullptr);
        pipeline.push_back(l);
        s.submit(pipeline).wait();
      } else {
        auto l = rnn_forward(*rnn_fwd_prim_desc, x, hx, nullptr,
                             weights, y, hy, nullptr, nullptr);
        pipeline.push_back(l);
        s.submit(pipeline).wait();
      }
    }

    if (HasInputC()) {
      delete cx;
      delete cy;
    }
    delete x;
    delete hx;
    delete y;
    delete hy;
    delete weights;
    delete x_desc;
    delete hx_desc;
    delete y_desc;
    delete weights_desc;
    delete rnn_fwd_prim_desc;
  }

 private:
  bool is_training_;
};

REGISTER_KERNEL_BUILDER(
    Name("MkldnnRNN").Device(DEVICE_CPU).TypeConstraint<float>("T"),
    MkldnnRNNForwardOp<CPUDevice, float>);


// Run the backward operation of the RNN model.
template <typename T>
class MkldnnRNNBackwardOp<CPUDevice, T> : public MkldnnRNNKernelCommon {
 public:
  typedef CPUDevice Device;

  explicit MkldnnRNNBackwardOp(OpKernelConstruction* context)
      : MkldnnRNNKernelCommon(context) {}

  void Compute(OpKernelContext* context) override {
    // LOG(ERROR) << "backward is called";
    const Tensor* Tx = nullptr;
    const Tensor* Thx = nullptr;
    const Tensor* Tcx = nullptr;
    const Tensor* Tweights = nullptr;
    MkldnnModelShapes model_shapes;
    OP_REQUIRES_OK(context,
                   ExtractForwardInput(context, model_types(), &Tx, &Thx,
                                       &Tcx, &Tweights, &model_shapes));

    // const auto& input_shape = model_shapes.input_shape;
    const auto& hidden_state_shape = model_shapes.hidden_state_shape;
    const auto& output_shape = model_shapes.output_shape;

    const Tensor* Tworkspace = nullptr;
    OP_REQUIRES_OK(context, context->input("reserve_space", &Tworkspace));
    const Tensor* Tdy = nullptr;
    OP_REQUIRES_OK(context, context->input("output_backprop", &Tdy));
    OP_REQUIRES(context, output_shape == Tdy->shape(),
                errors::InvalidArgument(
                    "h and c must have the same shape: ",
                    Thx->shape().DebugString(), " ",
                    Tcx->shape().DebugString()));
    const Tensor* Tdhy = nullptr;
    OP_REQUIRES_OK(context, context->input("output_h_backprop", &Tdhy));
    OP_REQUIRES(context, Tdhy->shape() == hidden_state_shape,
                errors::InvalidArgument(
                    "Invalid dhy shape: ", Tdhy->shape().DebugString(),
                    " ", hidden_state_shape.DebugString()));
    const Tensor* Tdcy = nullptr;
    if (HasInputC()) {
      // Only LSTM uses input_c and output_c. So for all other models, we only
      // need to create dummy outputs.
      OP_REQUIRES_OK(context, context->input("output_c_backprop", &Tdcy));
      OP_REQUIRES(context, Tdcy->shape() == hidden_state_shape,
                  errors::InvalidArgument("Invalid dcy shape: ",
                                          Tdcy->shape().DebugString(), " ",
                                          hidden_state_shape.DebugString()));
    }
    Tensor* Tdx = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, Tx->shape(), &Tdx));
    Tensor* Tdhx = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(1, Thx->shape(), &Tdhx));
    Tensor* Tdcx = nullptr;
    if (HasInputC()) {
      OP_REQUIRES_OK(context, context->allocate_output(2, Tcx->shape(), &Tdcx));
    } else {
      OP_REQUIRES_OK(context, context->allocate_output(2, {}, &Tdcx));
    }
    Tensor* Tdweights = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(3, Tweights->shape(), &Tdweights));

#ifdef OP_DATA_DUMP
    {
      FILE *fp = NULL;
      char f_name[256] = "data_bwd_in.txt";
      fp = fopen(f_name, "ab+");
      fprintf(fp, "\n------------x----------\n");
      dump_data(fp, Tx->NumElements(), Tx->flat<T>().data());

      fprintf(fp, "\n------------hx----------\n");
      dump_data(fp, Thx->NumElements(), Thx->flat<T>().data());

      if (HasInputC()) {
        fprintf(fp, "\n------------cx----------\n");
        dump_data(fp, Tcx->NumElements(), Tcx->flat<T>().data());
      }

      fprintf(fp, "\n------------weights----------\n");
      dump_data(fp, Tweights->NumElements(), Tweights->flat<T>().data());

      fprintf(fp, "\n------------dy----------\n");
      dump_data(fp, Tdy->NumElements(), Tdy->flat<T>().data());

      fprintf(fp, "\n------------dhy----------\n");
      dump_data(fp, Tdhy->NumElements(), Tdhy->flat<T>().data());

      if (HasInputC()) {
        fprintf(fp, "\n------------dcy----------\n");
        dump_data(fp, Tdcy->NumElements(), Tdcy->flat<T>().data());
      }

      fprintf(fp, "\n------------workspace----------\n");
      dump_data(fp, Tworkspace->NumElements(), Tworkspace->flat<T>().data());

      fclose(fp);
      fp = NULL;
   }
#endif

    memory *x;
    memory *hx;
    memory *cx;
    memory *dy;
    memory *dhy;
    memory *dcy;
    memory *dx;
    memory *dhx;
    memory *dcx;
    memory *weights;
    memory *workspace;
    memory *dweights;
    memory::desc *x_desc;
    memory::desc *hx_desc;
    memory::desc *y_desc;
    memory::desc *weights_desc;
    rnn_forward::primitive_desc *rnn_fwd_prim_desc;
    rnn_backward::primitive_desc *rnn_bwd_prim_desc;

    int state_outputs = 1;

    memory::data_type a_data_type = memory::data_type::f32;
    engine *eng = new engine(engine::kind::cpu, 0);

    const int total_w = get_param_size(rnn_mode(), model_shapes.dir_count, model_shapes.input_size, model_shapes.num_units, model_shapes.num_layers);
 
    x_desc = new memory::desc({model_shapes.seq_length, model_shapes.batch_size, model_shapes.input_size}, a_data_type, memory::format::rnx);
    hx_desc = new memory::desc({model_shapes.num_layers, model_shapes.batch_size, model_shapes.num_units}, a_data_type, memory::format::rnx);
    y_desc = new memory::desc({model_shapes.seq_length, model_shapes.batch_size, model_shapes.num_units * model_shapes.dir_count}, a_data_type, memory::format::rnx);
    weights_desc = new memory::desc({total_w}, a_data_type, memory::format::x);

    // clear dweights
    memset(static_cast<void*>(const_cast<T*>(Tdweights->flat<T>().data())), 0, Tweights->NumElements() * sizeof(T));

    x = new memory({ *x_desc, *eng }, static_cast<void*>(const_cast<T*>(Tx->flat<T>().data())));
    hx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Thx->flat<T>().data())));
    weights = new memory({ *weights_desc, *eng }, static_cast<void*>(const_cast<T*>(Tweights->flat<T>().data())));
    dx = new memory({ *x_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdx->flat<T>().data())));
    dhx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdhx->flat<T>().data())));
    dy = new memory({ *y_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdy->flat<T>().data())));
    dhy = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdhy->flat<T>().data())));
    dweights = new memory({ *weights_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdweights->flat<T>().data())));

    auto rnn_fwd_desc = rnn_forward::desc(prop_kind::forward_training, 
                                          static_cast<mkldnn::algorithm>(rnn_mode()),
                                          static_cast<mkldnn::direction>(rnn_direction_mode()),
                                          static_cast<mkldnn::input_mode>(rnn_input_mode()),
                                          model_shapes.num_units, model_shapes.num_layers, model_shapes.seq_length,
                                          state_outputs, *x_desc, *hx_desc, *y_desc, *weights_desc);
    rnn_fwd_prim_desc = new rnn_forward::primitive_desc(rnn_fwd_desc, *eng);

    auto rnn_bwd_desc = rnn_backward::desc(prop_kind::backward,
                                           static_cast<mkldnn::algorithm>(rnn_mode()),
                                           static_cast<mkldnn::direction>(rnn_direction_mode()),
                                           static_cast<mkldnn::input_mode>(rnn_input_mode()),
                                           model_shapes.num_units, model_shapes.num_layers, model_shapes.seq_length,
                                           state_outputs, *x_desc, *hx_desc, *y_desc, *weights_desc);
    rnn_bwd_prim_desc = new rnn_backward::primitive_desc(rnn_bwd_desc, *eng, *rnn_fwd_prim_desc);

    auto workspace_primitive_desc  = rnn_fwd_prim_desc->workspace_primitive_desc();
    workspace = new memory(workspace_primitive_desc, static_cast<void*>(const_cast<T*>(Tworkspace->flat<T>().data())));
    // LOG(ERROR) << "backward workspace size is: " << workspace_primitive_desc.get_size() / sizeof(T);

    std::vector<primitive> pipeline;
    auto s = stream(stream::kind::lazy);

    // TODO get workspace shape and creat output reserve space
    if (HasInputC()) {
      cx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tcx->flat<T>().data())));
      dcx = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdcx->flat<T>().data())));
      dcy = new memory({ *hx_desc, *eng }, static_cast<void*>(const_cast<T*>(Tdcy->flat<T>().data())));

      auto l = rnn_backward(*rnn_bwd_prim_desc, x, hx, cx,
                            dy, dhy, dcy, weights, workspace,
                            dx, dhx, dcx, dweights);
      pipeline.push_back(l);
      s.submit(pipeline).wait();
    } else {
      auto l = rnn_backward(*rnn_bwd_prim_desc, x, hx, nullptr,
                            dy, dhy, nullptr, weights, workspace,
                            dx, dhx, nullptr, dweights);
      pipeline.push_back(l);
      s.submit(pipeline).wait();
    }

#ifdef OP_DATA_DUMP
    {
      FILE *fp = NULL;
      char f_name[256] = "data_bwd_out.txt";
      fp = fopen(f_name, "ab+");
      fprintf(fp, "\n------------x----------\n");
      dump_data(fp, x->get_primitive_desc().get_size() / sizeof(T), (T*)x->get_data_handle());

      fprintf(fp, "\n------------hx----------\n");
      dump_data(fp, hx->get_primitive_desc().get_size() / sizeof(T), (T*)hx->get_data_handle());

      if (HasInputC()) {
        fprintf(fp, "\n------------cx----------\n");
        dump_data(fp, cx->get_primitive_desc().get_size() / sizeof(T), (T*)cx->get_data_handle());
      }

      fprintf(fp, "\n------------weights----------\n");
      dump_data(fp, weights->get_primitive_desc().get_size() / sizeof(T), (T*)weights->get_data_handle());

      fprintf(fp, "\n------------dy----------\n");
      dump_data(fp, dy->get_primitive_desc().get_size() / sizeof(T), (T*)dy->get_data_handle());

      fprintf(fp, "\n------------dhy----------\n");
      dump_data(fp, dhy->get_primitive_desc().get_size() / sizeof(T), (T*)dhy->get_data_handle());

      if (HasInputC()) {
        fprintf(fp, "\n------------dcy----------\n");
        dump_data(fp, dcy->get_primitive_desc().get_size() / sizeof(T), (T*)dcy->get_data_handle());
      }

      fprintf(fp, "\n------------workspace----------\n");
      dump_data(fp, workspace->get_primitive_desc().get_size() / sizeof(T), (T*)workspace->get_data_handle());

      fprintf(fp, "\n------------dx----------\n");
      dump_data(fp, dx->get_primitive_desc().get_size() / sizeof(T), (T*)dx->get_data_handle());

      fprintf(fp, "\n------------dhx----------\n");
      dump_data(fp, dhx->get_primitive_desc().get_size() / sizeof(T), (T*)dhx->get_data_handle());

      if (HasInputC()) {
        fprintf(fp, "\n------------dcx----------\n");
        dump_data(fp, dcx->get_primitive_desc().get_size() / sizeof(T), (T*)dcx->get_data_handle());
      }

      fprintf(fp, "\n------------dweights----------\n");
      dump_data(fp, dweights->get_primitive_desc().get_size() / sizeof(T), (T*)dweights->get_data_handle());

      fclose(fp);
      fp = NULL;
    }
#endif

    if (HasInputC()) {
      delete cx;
      delete dcx;
      delete dcy;
    }

    delete x;
    delete hx;
    delete dy;
    delete dhy;
    delete dx;
    delete dhx;
    delete weights;
    delete workspace;
    delete dweights;
    delete x_desc;
    delete hx_desc;
    delete y_desc;
    delete weights_desc;
    delete rnn_fwd_prim_desc;
    delete rnn_bwd_prim_desc;
  }
};

REGISTER_KERNEL_BUILDER(
    Name("MkldnnRNNBackprop").Device(DEVICE_CPU).TypeConstraint<float>("T"),
    MkldnnRNNBackwardOp<CPUDevice, float>);
}  // namespace tensorflow

#endif  // INTEL_MKL
