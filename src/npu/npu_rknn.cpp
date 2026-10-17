#include "npu_rknn.h"
#include <ros/ros.h>
#include <fstream>
#include <cstring>
#include <cmath>

RknnEngine::RknnEngine() : ctx_(0), in_h_(0), in_w_(0), in_c_(1), n_out_(0)
{
    std::memset(&io_num_, 0, sizeof(io_num_));
}

RknnEngine::~RknnEngine()
{
    if (ctx_)
        rknn_destroy(ctx_);
}

bool RknnEngine::init(const std::string &model_path, rknn_core_mask core_mask)
{
    std::ifstream ifs(model_path, std::ios::binary | std::ios::ate);
    if (!ifs)
    {
        ROS_ERROR_STREAM("[NPU] cannot open model: " << model_path);
        return false;
    }
    const std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(size));
    if (!ifs.read(buf.data(), size))
    {
        ROS_ERROR("[NPU] failed to read model file");
        return false;
    }

    int ret = rknn_init(&ctx_, buf.data(), static_cast<uint32_t>(size), RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret < 0)
    {
        ROS_ERROR("[NPU] rknn_init failed: %d", ret);
        ctx_ = 0;
        return false;
    }
    return bindCore(core_mask) && queryIo();
}

bool RknnEngine::dupFrom(RknnEngine &src, rknn_core_mask core_mask)
{
    if (!src.ctx_)
        return false;
    int ret = rknn_dup_context(&src.ctx_, &ctx_);
    if (ret < 0)
    {
        ROS_WARN("[NPU] rknn_dup_context failed %d, will try a full init", ret);
        ctx_ = 0;
        return false;
    }
    return bindCore(core_mask) && queryIo();
}

bool RknnEngine::bindCore(rknn_core_mask core_mask)
{
    int ret = rknn_set_core_mask(ctx_, core_mask);
    if (ret < 0)
    {
        ROS_WARN("[NPU] rknn_set_core_mask(0x%x) failed %d, keep auto", (unsigned)core_mask, ret);
        return true;
    }
    ROS_INFO("[NPU] bound context to core_mask=0x%x", (unsigned)core_mask);
    return true;
}

bool RknnEngine::queryIo()
{
    int ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0)
        return false;
    n_out_ = static_cast<int>(io_num_.n_output);
    if (io_num_.n_input == 0)
    {
        ROS_ERROR("[NPU] model has no inputs");
        return false;
    }
    in_attr_.resize(io_num_.n_input);
    out_attr_.resize(io_num_.n_output);

    for (uint32_t i = 0; i < io_num_.n_input; i++)
    {
        std::memset(&in_attr_[i], 0, sizeof(rknn_tensor_attr));
        in_attr_[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_[i], sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < io_num_.n_output; i++)
    {
        std::memset(&out_attr_[i], 0, sizeof(rknn_tensor_attr));
        out_attr_[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attr_[i], sizeof(rknn_tensor_attr));
    }

    // NHWC or NCHW: pick H/W from dims
    const rknn_tensor_attr &a = in_attr_[0];
    if (a.n_dims >= 4)
    {
        if (a.fmt == RKNN_TENSOR_NHWC)
        {
            in_h_ = a.dims[1];
            in_w_ = a.dims[2];
            in_c_ = a.dims[3];
        }
        else
        {
            in_c_ = a.dims[1];
            in_h_ = a.dims[2];
            in_w_ = a.dims[3];
        }
    }
    else if (a.n_dims == 3)
    {
        if (a.dims[0] <= 4)
        {
            in_c_ = a.dims[0];
            in_h_ = a.dims[1];
            in_w_ = a.dims[2];
        }
        else
        {
            in_h_ = a.dims[0];
            in_w_ = a.dims[1];
            in_c_ = a.dims[2] > 0 ? a.dims[2] : 1;
        }
    }
    if (in_h_ <= 0 || in_w_ <= 0)
    {
        in_h_ = 480;
        in_w_ = 640;
        in_c_ = 1;
        ROS_WARN("[NPU] could not parse input dims, assume 640x480x1");
    }
    ROS_INFO("[NPU] input %dx%dx%d, outputs=%d", in_w_, in_h_, in_c_, n_out_);
    return true;
}

bool RknnEngine::infer(const uint8_t *nhwc, int h, int w, int c,
                       std::vector<std::vector<float>> &outputs)
{
    return infer(nhwc, h, w, c, &outputs, 1);
}

bool RknnEngine::infer(const uint8_t *nhwc, int h, int w, int c,
                       std::vector<std::vector<float>> *outputs, int want_float)
{
    if (!ctx_)
        return false;

    rknn_input input;
    std::memset(&input, 0, sizeof(input));
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<uint32_t>(h * w * c);
    input.buf = const_cast<uint8_t *>(nhwc);
    input.pass_through = 0;

    int ret = rknn_inputs_set(ctx_, 1, &input);
    if (ret < 0)
        return false;
    ret = rknn_run(ctx_, nullptr);
    if (ret < 0)
        return false;

    std::vector<rknn_output> rknn_outs(n_out_);
    std::memset(rknn_outs.data(), 0, sizeof(rknn_output) * n_out_);
    for (int i = 0; i < n_out_; i++)
    {
        rknn_outs[i].want_float = want_float ? 1 : 0;
        rknn_outs[i].is_prealloc = 0;
    }
    ret = rknn_outputs_get(ctx_, n_out_, rknn_outs.data(), nullptr);
    if (ret < 0)
        return false;

    if (outputs)
    {
        outputs->resize(n_out_);
        for (int i = 0; i < n_out_; i++)
        {
            if (want_float)
            {
                const int n = static_cast<int>(rknn_outs[i].size / sizeof(float));
                const float *p = static_cast<const float *>(rknn_outs[i].buf);
                (*outputs)[i].assign(p, p + n);
            }
        }
    }
    rknn_outputs_release(ctx_, n_out_, rknn_outs.data());
    return true;
}
