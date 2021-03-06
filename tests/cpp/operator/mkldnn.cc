/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  \file mkldnn.cc
 *  \brief test functions in mkldnn.
 *  \author Da Zheng
 */

#if MXNET_USE_MKLDNN == 1

#include "gtest/gtest.h"
#include "mxnet/imperative.h"
#include "../../src/operator/nn/mkldnn/mkldnn_base-inl.h"

using namespace mxnet;

#if __GNUC__ >= 5
bool test_mem_align(void *mem, size_t size, size_t alignment, size_t space) {
  void *ret1, *ret2;
  size_t space1, space2;
  space1 = space;
  space2 = space;
  ret1 = mxnet::AlignMem(mem, size, alignment, &space1);
  ret2 = std::align(alignment, size, mem, space2);
  EXPECT_EQ(ret1, ret2);
  EXPECT_EQ(space1, space2);
  return ret1 == ret2;
}
#endif

TEST(MKLDNN_UTIL_FUNC, AlignMem) {
#if __GNUC__ >= 5
  size_t alignment = 4096;
  void *mem;
  size_t size, space;
  // When mem has been aligned.
  mem = reinterpret_cast<void *>(0x10000);
  size = 1000;
  space = 10000;
  test_mem_align(mem, size, alignment, space);

  // When mem isn't aligned and we have enough space for alignment.
  mem = reinterpret_cast<void *>(0x10010);
  size = 1000;
  space = 10000;
  test_mem_align(mem, size, alignment, space);

  // When mem isn't aligned and we don't have enough memory for alignment
  mem = reinterpret_cast<void *>(0x10010);
  size = 1000;
  space = 1001;
  test_mem_align(mem, size, alignment, space);

  for (size_t i = 0; i < 10000; i++) {
    mem = reinterpret_cast<void *>(random());
    size = random() % 2000;
    space = random() % 2000;
    test_mem_align(mem, size, alignment, space);
  }
#else
  // std::align is not supported in GCC < 5.0, this test case will be checked
  // with newer version
  LOG(INFO) << "Skipped for GCC " << __GNUC__ << "." << __GNUC_MINOR__;
#endif
}

// Init arrays with the default layout.
static void InitArray(NDArray *arr, bool is_rand = false) {
  const TBlob &blob = arr->data();
  mshadow::default_real_t *data = blob.dptr<mshadow::default_real_t>();
  size_t size = blob.Size();
  if (is_rand) {
    for (size_t i = 0; i < size; i++)
      data[i] = std::rand();
  } else {
    for (size_t i = 0; i < size; i++)
      data[i] = i;
  }
}

// Init arrays with the specified layout.
static void InitMKLDNNArray(NDArray *arr, const mkldnn::memory::primitive_desc &pd,
                            bool is_rand = false) {
  const TBlob &blob = arr->data();
  mshadow::default_real_t *data = blob.dptr<mshadow::default_real_t>();
  size_t size = blob.Size();
  if (is_rand) {
    for (size_t i = 0; i < size; i++)
      data[i] = std::rand();
  } else {
    for (size_t i = 0; i < size; i++)
      data[i] = i;
  }
  arr->MKLDNNDataReorderAsync(pd);
  arr->WaitToRead();
}

static void VerifyDefMem(const mkldnn::memory &mem) {
  mkldnn::memory::primitive_desc pd = mem.get_primitive_desc();
  mshadow::default_real_t *data
      = static_cast<mshadow::default_real_t *>(mem.get_data_handle());
  size_t size = pd.get_size() / sizeof(mshadow::default_real_t);
  size_t num_same = 0;
  for (size_t i = 0; i < size; i++)
    num_same += data[i] == static_cast<mshadow::default_real_t>(i);
  EXPECT_EQ(num_same, size);
}

static void VerifyMem(const mkldnn::memory &mem) {
  mkldnn::memory::primitive_desc pd = mem.get_primitive_desc();

  if (pd.desc().data.format == GetDefaultFormat(pd.desc())) {
    VerifyDefMem(mem);
  } else {
    mkldnn::memory::dims dims(pd.desc().data.ndims);
    for (size_t i = 0; i < dims.size(); i++)
      dims[i] = pd.desc().data.dims[i];
    mkldnn::memory::desc desc{dims,
                              static_cast<mkldnn::memory::data_type>(pd.desc().data.data_type),
                              static_cast<mkldnn::memory::format>(GetDefaultFormat(pd.desc()))};
    mkldnn::memory::primitive_desc new_pd(desc, CpuEngine::Get()->get_engine());
    mkldnn::memory new_mem(new_pd);

    std::vector<mkldnn::primitive> net;
    net.push_back(mkldnn::reorder(mem, new_mem));
    mkldnn::stream(mkldnn::stream::kind::eager).submit(net).wait();
    VerifyDefMem(new_mem);
  }
}

static mkldnn::memory::primitive_desc GetMemPD(const TShape s, int dtype,
                                               mkldnn::memory::format format) {
  mkldnn::memory::dims dims(s.ndim());
  for (size_t i = 0; i < dims.size(); i++)
    dims[i] = s[i];
  mkldnn::memory::desc desc{dims, get_mkldnn_type(dtype), format};
  return mkldnn::memory::primitive_desc(desc, CpuEngine::Get()->get_engine());
}

// This function gets special MKLDNN formats without knowing the specific
// hardware configuration. Certainly, it potentially misses some format if
// it's specific for certain array shapes. It covers at least one special format
// for each of the formats: nchw, oihw, goihw.
// To test the logic of the code in NDArray, these formats should be enough.
static std::vector<mkldnn::memory::format> GetMKLDNNFormat(size_t num_dims, int dtype) {
  if (num_dims == 4) {
    mkldnn::memory::dims data_dims{1, 3, 224, 224};
    mkldnn::memory::desc data_md{data_dims, get_mkldnn_type(dtype),
                                 mkldnn::memory::format::any};
    mkldnn::memory::dims weight_dims{96, 3, 11, 11};
    mkldnn::memory::desc weight_md{weight_dims, get_mkldnn_type(dtype),
                                   mkldnn::memory::format::any};
    mkldnn::memory::dims output_dims{1, 96, 54, 54};
    mkldnn::memory::desc out_md{output_dims, get_mkldnn_type(dtype),
                                mkldnn::memory::format::any};
    mkldnn::memory::dims strides{4, 4};
    mkldnn::memory::dims padding{0, 0};

    mkldnn::convolution_forward::desc desc(mkldnn::prop_kind::forward_training,
                                           mkldnn::algorithm::convolution_direct,
                                           data_md, weight_md, out_md, strides,
                                           padding, padding, mkldnn::padding_kind::zero);
    mkldnn::convolution_forward::primitive_desc pd(desc, CpuEngine::Get()->get_engine());
    std::vector<mkldnn::memory::format> ret(2);
    ret[0] = static_cast<mkldnn::memory::format>(pd.dst_primitive_desc().desc().data.format);
    ret[1] = static_cast<mkldnn::memory::format>(pd.weights_primitive_desc().desc().data.format);
    printf("format: %d, %d\n", ret[0], ret[1]);
    return ret;
  } else if (num_dims == 5) {
    mkldnn::memory::dims data_dims{1, 32, 112, 112};
    mkldnn::memory::desc data_md{data_dims, get_mkldnn_type(dtype),
                                 mkldnn::memory::format::any};
    mkldnn::memory::dims weight_dims{32, 1, 1, 3, 3};
    mkldnn::memory::desc weight_md{weight_dims, get_mkldnn_type(dtype),
                                   mkldnn::memory::format::any};
    mkldnn::memory::dims output_dims{1, 32, 112, 112};
    mkldnn::memory::desc out_md{output_dims, get_mkldnn_type(dtype),
                                mkldnn::memory::format::any};
    mkldnn::memory::dims strides{1, 1};
    mkldnn::memory::dims padding{1, 1};

    mkldnn::convolution_forward::desc desc(mkldnn::prop_kind::forward_training,
                                           mkldnn::algorithm::convolution_direct,
                                           data_md, weight_md, out_md, strides,
                                           padding, padding, mkldnn::padding_kind::zero);
    mkldnn::convolution_forward::primitive_desc pd(desc, CpuEngine::Get()->get_engine());
    std::vector<mkldnn::memory::format> ret(1);
    ret[0] = static_cast<mkldnn::memory::format>(pd.weights_primitive_desc().desc().data.format);
    printf("format: %d\n", ret[0]);
    return ret;
  } else {
    return std::vector<mkldnn::memory::format>();
  }
}

struct TestArrayShapes {
  std::vector<nnvm::TShape> shapes;
  std::vector<mkldnn::memory::primitive_desc> pds;
};

static TestArrayShapes GetTestArrayShapes() {
  int dtype = mshadow::DataType<mshadow::default_real_t>::kFlag;
  std::vector<TShape> shapes;
  std::vector<mkldnn::memory::primitive_desc> pds;
  {
    // 1D
    TShape s(1);
    s[0] = 279936;
    shapes.push_back(s);
    pds.push_back(GetMemPD(s, dtype, mkldnn::memory::format::x));
    s[0] = 34848;
    shapes.push_back(s);
    pds.push_back(GetMemPD(s, dtype, mkldnn::memory::format::x));
  }
  {
    // 2D
    TShape s(2);
    s[0] = 96;
    s[1] = 2916;
    shapes.push_back(s);
    pds.push_back(GetMemPD(s, dtype, mkldnn::memory::format::nc));
    s[0] = 96;
    s[1] = 363;
    shapes.push_back(s);
    pds.push_back(GetMemPD(s, dtype, mkldnn::memory::format::nc));
  }
  {
    // 4D
    TShape s1(4);
    s1[0] = 10; s1[1] = 96; s1[2] = 54; s1[3] = 54;
    shapes.push_back(s1);
    pds.push_back(GetMemPD(s1, dtype, mkldnn::memory::format::nchw));

    TShape s2(4);
    s2[0] = 96; s2[1] = 3; s2[2] = 11; s2[3] = 11;
    shapes.push_back(s2);
    pds.push_back(GetMemPD(s2, dtype, mkldnn::memory::format::oihw));

    std::vector<mkldnn::memory::format> formats = GetMKLDNNFormat(4, dtype);
    pds.push_back(GetMemPD(s1, dtype, formats[0]));
    pds.push_back(GetMemPD(s2, dtype, formats[1]));
  }
  {
    // 5D
    TShape s(5);
    s[0] = 96; s[1] = 1; s[2] = 3; s[3] = 11; s[4] = 11;
    shapes.push_back(s);
    pds.push_back(GetMemPD(s, dtype, mkldnn::memory::format::goihw));

    std::vector<mkldnn::memory::format> formats = GetMKLDNNFormat(5, dtype);
    pds.push_back(GetMemPD(s, dtype, formats[0]));
  }

  TestArrayShapes ret;
  ret.shapes = shapes;
  ret.pds = pds;
  return ret;
}

TEST(MKLDNN_NDArray, GetDataReorder) {
  TestArrayShapes tas = GetTestArrayShapes();
  std::vector<TShape> shapes = tas.shapes;
  std::vector<mkldnn::memory::primitive_desc> pds = tas.pds;


  // Reorder from the default to any other layout.
  for (auto s : shapes) {
    NDArray arr(s, Context());
    InitArray(&arr);
    for (auto pd : pds) {
      if (s.Size() == pd.get_size() / sizeof(mshadow::default_real_t)) {
        const mkldnn::memory *mem = arr.GetMKLDNNDataReorder(pd);
        printf("reorder from (");
        for (size_t i = 0; i < s.ndim(); i++)
          printf("%ld, ", s[i]);
        printf(") to (");
        for (int i = 0; i < pd.desc().data.ndims; i++)
          printf("%d, ", pd.desc().data.dims[i]);
        printf("), format: %d\n", pd.desc().data.format);
        MKLDNNStream::Get()->Submit(false);
        VerifyMem(*mem);
        MKLDNNStream::Get()->Cleanup();
      }
    }
  }

  // Reorder from a special layout to another layout.
  for (auto s : shapes) {
    for (auto from_pd : pds) {
      if (from_pd.get_size() / sizeof(mshadow::default_real_t) == s.Size()) {
        NDArray arr(s, Context());
        // There is possibility that the dimensions of an NDArray doesn't match
        // with the MKLDNN memory inside.
        printf("Init array (");
        for (size_t i = 0; i < s.ndim(); i++)
          printf("%ld, ", s[i]);
        printf(") with MKLDNN memory (");
        for (int i = 0; i < from_pd.desc().data.ndims; i++)
          printf("%d, ", from_pd.desc().data.dims[i]);
        printf("), format: %d\n", from_pd.desc().data.format);
        InitMKLDNNArray(&arr, from_pd);
        for (auto to_pd : pds) {
          if (to_pd.get_size() / sizeof(mshadow::default_real_t) == s.Size()) {
            const mkldnn::memory *mem = arr.GetMKLDNNDataReorder(to_pd);
            printf("reorder from (");
            for (size_t i = 0; i < s.ndim(); i++)
              printf("%ld, ", s[i]);
            printf("), format: %d to (",
                   arr.GetMKLDNNData()->get_primitive_desc().desc().data.format);
            for (int i = 0; i < to_pd.desc().data.ndims; i++)
              printf("%d, ", to_pd.desc().data.dims[i]);
            printf("), format: %d\n", to_pd.desc().data.format);
            MKLDNNStream::Get()->Submit(false);
            VerifyMem(*mem);
            MKLDNNStream::Get()->Cleanup();
          }
        }
      }
    }
  }
}

struct OpAttrs {
  nnvm::NodeAttrs attrs;
  std::vector<DispatchMode> dispatches;
};

OpAttrs GetCopyOp() {
  OpAttrs attrs;
  attrs.attrs.op = Op::Get("_copy");
  attrs.dispatches.resize(2);
  attrs.dispatches[0] = DispatchMode::kFCompute;
  attrs.dispatches[1] = DispatchMode::kFComputeEx;
  return attrs;
}

OpAttrs GetLeakyReluOp() {
  OpAttrs attrs;
  attrs.attrs.op = Op::Get("LeakyReLU");
  attrs.dispatches.resize(1);
  attrs.dispatches[0] = DispatchMode::kFCompute;
  return attrs;
}

OpAttrs GetSumOp() {
  OpAttrs attrs;
  attrs.attrs.op = Op::Get("elemwise_add");
  attrs.dispatches.resize(2);
  attrs.dispatches[0] = DispatchMode::kFCompute;
  attrs.dispatches[1] = DispatchMode::kFComputeEx;
  return attrs;
}

/*
 * We want to get a few types of NDArrays for testing:
 * 1. Normal NDArray
 * 2. Normal NDArray with MKLDNN layout (output from an MKLDNN operator)
 * 3. Normal NDArray with MKLDNN layout whose MKLDNN memory may have different
 *    dimensions from the NDArray (result of MKLDNNDataReorderAsync). However, this
 *    type of NDArrays only exists for weight arrays. I don't think we should
 *    pass them to all operators.
 *    In the inference mode, the MKLDNN memory in the weight array will be
 *    reordered to 5 dimensions.
 * 4. Reshaped/sliced NDArray
 * 5. Reshaped/sliced NDArray with MKLDNN layout (reshape/slice from Normal NDArray
 *    with MKLDNN layout)
 * 6. Reshaped/sliced NDArray with MKLDNN layout whose MKLDNN memory may have
 *    different dimensions from the NDArray (result of MKLDNNDataReorderAsync).
 *    However, this type of NDArrays only exists for weight arrays. I don't think
 *    we should pass them to all operators.
 *    In the inference mode, the MKLDNN memory in the weight array will be
 *    reordered to 5 dimensions.
 *
 */
std::vector<NDArray> GetTestInputArrays() {
  TestArrayShapes tas = GetTestArrayShapes();
  std::vector<nnvm::TShape> shapes = tas.shapes;
  std::vector<mkldnn::memory::primitive_desc> pds = tas.pds;

  std::vector<NDArray> in_arrs;
  for (auto shape : shapes) {
    in_arrs.emplace_back(shape, Context());
    InitArray(&in_arrs.back());
    for (auto pd : pds) {
      if (shape.Size() != pd.get_size() / sizeof(mshadow::default_real_t))
        continue;

      in_arrs.emplace_back(shape, Context());
      InitMKLDNNArray(&in_arrs.back(), pd);

      // Get a sliced version.
      NDArray arr(shape, Context());
      InitMKLDNNArray(&arr, pd);
      arr = arr.Slice(1, arr.shape()[0] - 1);
      in_arrs.emplace_back(arr);
    }
  }
  return in_arrs;
}

/*
 * We want to get a few types of NDArrays for testing:
 * 1. Normal NDArray
 * 2. Normal NDArray with MKLDNN layout (output from an MKLDNN operator)
 * 3. Normal NDArray with MKLDNN layout whose MKLDNN memory may have different
 *    dimensions from the NDArray (result of MKLDNNDataReorderAsync). However, this
 *    type of NDArrays only exists for weight arrays. I don't think we should
 *    pass them to all operators.
 *    In the inference mode, the MKLDNN memory in the weight array will be
 *    reordered to 5 dimensions.
 * 4. Reshaped/sliced NDArray
 * 5. Reused NDArray (this is created by the MXNet executor). This type of
 *    NDArrays can only be used as output arrays.
 * 6. Reused NDArray converted from an array with a different data type.
 * 7. Reused reshaped/sliced NDArray.
 * 8. Reused NDArray with MKLDNN layout.
 * 9. Reused NDArray with MKLDNN layout of different dimensions.
 */
std::vector<NDArray> GetTestOutputArrays(const TShape &shape,
                                         const std::vector<mkldnn::memory::primitive_desc> &pds) {
  std::vector<NDArray> in_arrs;
  // Type 1.
  in_arrs.emplace_back(shape, Context());
  InitArray(&in_arrs.back(), true);

  // Type 4.
  TShape tmp_shape = shape;
  tmp_shape[0] = shape[0] * 2;
  NDArray arr0(tmp_shape, Context());
  InitArray(&arr0, true);
  in_arrs.emplace_back(arr0.Slice(1, shape[0] + 1));

  // Type 5.
  // Get a reused version.
  nnvm::TShape s(1);
  s[0] = shape.Size();
  NDArray arr1(s, Context());
  arr1 = arr1.AsArray(shape, arr1.dtype());
  InitArray(&arr1, true);
  in_arrs.emplace_back(arr1);

  // Type 6.
  s[0] = shape.Size() * GetTypeSize(mshadow::default_type_flag);
  NDArray arr2(s, Context(), true, mshadow::kUint8);
  arr2 = arr2.AsArray(shape, mshadow::default_type_flag);
  InitArray(&arr2, true);
  in_arrs.emplace_back(arr2);

  // Type 7
  s[0] = shape.Size() * GetTypeSize(mshadow::default_type_flag) * 2;
  NDArray arr3(s, Context(), true, mshadow::kUint8);
  tmp_shape[0] = shape[0] * 2;
  arr3 = arr3.AsArray(tmp_shape, mshadow::default_type_flag);
  InitArray(&arr3, true);
  in_arrs.emplace_back(arr3.Slice(1, shape[0] + 1));

  for (auto pd : pds) {
    if (shape.Size() != pd.get_size() / sizeof(mshadow::default_real_t))
      continue;

    // Type 2, 3.
    in_arrs.emplace_back(shape, Context());
    InitMKLDNNArray(&in_arrs.back(), pd, true);

    // Type 8, 9.
    // Get a reused version.
    nnvm::TShape s(1);
    s[0] = shape.Size();
    NDArray arr = NDArray(s, Context());
    arr = arr.AsArray(shape, arr.dtype());
    InitMKLDNNArray(&arr, pd, true);
    in_arrs.emplace_back(arr);
  }
  return in_arrs;
}

using VerifyFunc = std::function<void (const std::vector<NDArray *> &in_arrs, const NDArray &arr)>;

void VerifyCopyResult(const std::vector<NDArray *> &in_arrs, const NDArray &arr) {
  NDArray tmp1 = in_arrs[0]->Reorder2Default();
  NDArray tmp2 = arr.Reorder2Default();
  EXPECT_EQ(tmp1.shape().Size(), tmp2.shape().Size());
  TBlob d1 = tmp1.data();
  TBlob d2 = tmp2.data();
  EXPECT_EQ(memcmp(d1.dptr_, d2.dptr_,
                   tmp1.shape().Size() * sizeof(mshadow::default_real_t)), 0);
}

void VerifySumResult(const std::vector<NDArray *> &in_arrs, const NDArray &arr) {
  NDArray in1 = in_arrs[0]->Reorder2Default();
  NDArray in2 = in_arrs[1]->Reorder2Default();
  NDArray out = arr.Reorder2Default();
  EXPECT_EQ(in1.shape().Size(), in2.shape().Size());
  EXPECT_EQ(in1.shape().Size(), out.shape().Size());

  mshadow::default_real_t *d1 = in1.data().dptr<mshadow::default_real_t>();
  mshadow::default_real_t *d2 = in2.data().dptr<mshadow::default_real_t>();
  mshadow::default_real_t *o = out.data().dptr<mshadow::default_real_t>();
  for (size_t i = 0; i < in1.shape().Size(); i++)
    EXPECT_EQ(d1[i] + d2[i], o[i]);
}

TEST(MKLDNN_NDArray, CopyFrom) {
  TestArrayShapes tas = GetTestArrayShapes();
  std::vector<mkldnn::memory::primitive_desc> pds = tas.pds;

  std::vector<NDArray> in_arrs = GetTestInputArrays();
  for (auto in_arr : in_arrs) {
    std::vector<NDArray> out_arrs = GetTestOutputArrays(in_arr.shape(), pds);
    for (auto out_arr : out_arrs) {
      if (in_arr.IsMKLDNNData() && in_arr.IsView())
        in_arr = in_arr.Reorder2Default();
      const mkldnn::memory *mem = in_arr.GetMKLDNNData();
      out_arr.CopyFrom(*mem);
      MKLDNNStream::Get()->Submit();
      std::vector<NDArray *> inputs(1);
      inputs[0] = &in_arr;
      VerifyCopyResult(inputs, out_arr);
    }
  }
}

void TestUnaryOp(const OpAttrs &attrs, VerifyFunc verify_fn) {
  std::vector<NDArray*> inputs(1);
  std::vector<NDArray*> outputs(1);
  std::vector<OpReqType> req(1);
  std::vector<DispatchMode> dispatches = attrs.dispatches;

  TestArrayShapes tas = GetTestArrayShapes();
  std::vector<mkldnn::memory::primitive_desc> pds = tas.pds;

  std::vector<NDArray> in_arrs = GetTestInputArrays();
  for (auto in_arr : in_arrs) {
    for (auto dispatch : dispatches) {
      std::vector<NDArray> out_arrs = GetTestOutputArrays(in_arr.shape(), pds);
      for (auto out_arr : out_arrs) {
        req[0] = kWriteTo;
        inputs[0] = &in_arr;
        outputs[0] = &out_arr;
        Imperative::Get()->InvokeOp(Context(), attrs.attrs, inputs,
                                    outputs, req, dispatch, mxnet::OpStatePtr());
        out_arr.WaitToRead();
        verify_fn(inputs, out_arr);
      }
    }
  }

  for (auto dispatch : dispatches) {
    in_arrs = GetTestInputArrays();
    for (auto arr : in_arrs) {
      // If the array is a view, we shouldn't write data to it.
      if (arr.IsView())
        continue;

      NDArray orig = arr.Copy(arr.ctx());
      req[0] = kWriteInplace;
      inputs[0] = &arr;
      outputs[0] = &arr;
      Imperative::Get()->InvokeOp(Context(), attrs.attrs, inputs, outputs, req,
                                  dispatch, mxnet::OpStatePtr());
      arr.WaitToRead();
      inputs[0] = &orig;
      verify_fn(inputs, arr);
    }
  }
}

void TestBinaryOp(const OpAttrs &attrs, VerifyFunc verify_fn) {
  std::vector<NDArray*> inputs(2);
  std::vector<NDArray*> outputs(1);
  std::vector<OpReqType> req(1);
  std::vector<DispatchMode> dispatches = attrs.dispatches;

  TestArrayShapes tas = GetTestArrayShapes();
  std::vector<mkldnn::memory::primitive_desc> pds = tas.pds;

  std::vector<NDArray> in_arrs = GetTestInputArrays();
  for (auto in_arr1 : in_arrs) {
    for (auto dispatch : dispatches) {
      std::vector<NDArray> out_arrs = GetTestOutputArrays(in_arr1.shape(), pds);
      for (auto out_arr : out_arrs) {
        req[0] = kWriteTo;
        inputs[0] = &in_arr1;
        inputs[1] = &in_arr1;
        outputs[0] = &out_arr;
        Imperative::Get()->InvokeOp(Context(), attrs.attrs, inputs,
                                    outputs, req, dispatch, mxnet::OpStatePtr());
        out_arr.WaitToRead();
        verify_fn(inputs, out_arr);
      }
    }
  }

  for (auto dispatch : dispatches) {
    in_arrs = GetTestInputArrays();
    for (auto arr : in_arrs) {
      // If the array is a view, we shouldn't write data to it.
      if (arr.IsView())
        continue;

      NDArray orig = arr.Copy(arr.ctx());
      req[0] = kWriteInplace;
      inputs[0] = &arr;
      inputs[1] = &arr;
      outputs[0] = &arr;
      Imperative::Get()->InvokeOp(Context(), attrs.attrs, inputs, outputs, req,
                                  dispatch, mxnet::OpStatePtr());
      arr.WaitToRead();
      std::vector<NDArray *> orig_inputs(2);
      orig_inputs[0] = &orig;
      orig_inputs[1] = &orig;
      verify_fn(orig_inputs, arr);
    }
  }
}

TEST(IMPERATIVE, UnaryOp) {
  OpAttrs attrs = GetCopyOp();
  TestUnaryOp(attrs, VerifyCopyResult);
}


TEST(IMPERATIVE, BinaryOp) {
  OpAttrs attrs = GetSumOp();
  TestBinaryOp(attrs, VerifySumResult);
}

#endif
